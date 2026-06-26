/**
 * @file cron_service.c
 * @brief Cron (scheduled task) service for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Implements a cron-like scheduler that supports recurring ("every") and
 * one-shot ("at") jobs. Jobs are persisted to a JSON file on the filesystem.
 * When a job fires, it injects a message via sys_bus_push_inbound so the
 * agent_loop handles the full AI interaction and forwards the reply to IM.
 */

#include "cron_service.h"
#include "tool_files.h"

#include "tal_api.h"
#include "tal_time_service.h"
#include "sys_bus.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>

/***********************************************************
************************macro define************************
***********************************************************/

#define CRON_FILE       CLAW_FS_ROOT_PATH "/cron.json"
#define CRON_FILE_MAX   8192
#ifndef CRON_THREAD_STACK
#define CRON_THREAD_STACK (6 * 1024)
#endif

/*
 * Threshold in seconds beyond which a one-shot job is considered overdue.
 * A job is naturally up to one check-interval late due to the sleep between
 * checks, so anything beyond two intervals is flagged as a delayed fire.
 */
#define CRON_OVERDUE_THRESHOLD_S  (CLAW_CRON_CHECK_INTERVAL_MS / 1000 * 2)

/***********************************************************
***********************variable define**********************
***********************************************************/

static cron_job_t    s_jobs[CLAW_CRON_MAX_JOBS];
static int           s_job_count   = 0;
static THREAD_HANDLE s_cron_thread = NULL;

/***********************************************************
***********************function define**********************
***********************************************************/

static OPERATE_RET cron_save_jobs(void);

static const char *cron_json_get_string(const cJSON *item)
{
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        return NULL;
    }

    return item->valuestring;
}

/**
 * @brief Get current epoch time
 */
static int64_t cron_now_epoch(void)
{
    return (int64_t)tal_time_get_posix();
}

/**
 * @brief Generate a random 8-char hex job ID
 */
static void cron_generate_id(char *id_buf, size_t id_buf_size)
{
    uint32_t r = ((uint32_t)tal_system_get_random(0x7fffffffu)) ^
                 (uint32_t)tal_system_get_millisecond();
    snprintf(id_buf, id_buf_size, "%08x", (unsigned int)r);
}

/**
 * @brief Compute initial next_run time for a new job
 */
static void compute_initial_next_run(cron_job_t *job)
{
    int64_t now = cron_now_epoch();

    if (job->kind == CRON_KIND_EVERY) {
        job->next_run = now + job->interval_s;
    } else if (job->kind == CRON_KIND_AT) {
        if (job->at_epoch > now) {
            job->next_run = job->at_epoch;
        } else {
            job->next_run = 0;
            job->enabled  = false;
        }
    }
}

/**
 * @brief Load cron jobs from JSON file
 */
static OPERATE_RET cron_load_jobs(void)
{
    TUYA_FILE f = claw_fopen(CRON_FILE, "r");
    if (!f) {
        PR_INFO("No cron file found, starting fresh");
        s_job_count = 0;
        return OPRT_OK;
    }

    int fsize = claw_fgetsize(CRON_FILE);
    if (fsize <= 0 || fsize > CRON_FILE_MAX) {
        PR_WARN("Cron file invalid size: %d", fsize);
        claw_fclose(f);
        s_job_count = 0;
        return OPRT_OK;
    }

    char *buf = (char *)claw_malloc((size_t)fsize + 1);
    if (!buf) {
        claw_fclose(f);
        return OPRT_MALLOC_FAILED;
    }

    int n = claw_fread(buf, fsize, f);
    claw_fclose(f);
    if (n < 0) {
        claw_free(buf);
        return OPRT_COM_ERROR;
    }
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    claw_free(buf);
    if (!root) {
        PR_WARN("Failed to parse cron json");
        s_job_count = 0;
        return OPRT_OK;
    }

    cJSON *jobs_arr = cJSON_GetObjectItem(root, "jobs");
    if (!jobs_arr || !cJSON_IsArray(jobs_arr)) {
        cJSON_Delete(root);
        s_job_count = 0;
        return OPRT_OK;
    }

    s_job_count = 0;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, jobs_arr)
    {
        if (s_job_count >= CLAW_CRON_MAX_JOBS) {
            break;
        }

        cron_job_t *job = &s_jobs[s_job_count];
        memset(job, 0, sizeof(*job));

        const char *id       = cron_json_get_string(cJSON_GetObjectItem(item, "id"));
        const char *name     = cron_json_get_string(cJSON_GetObjectItem(item, "name"));
        const char *kind_str = cron_json_get_string(cJSON_GetObjectItem(item, "kind"));
        const char *message  = cron_json_get_string(cJSON_GetObjectItem(item, "message"));

        if (!id || !name || !kind_str || !message) {
            continue;
        }

        strncpy(job->id, id, sizeof(job->id) - 1);
        strncpy(job->name, name, sizeof(job->name) - 1);
        strncpy(job->message, message, sizeof(job->message) - 1);

        cJSON *enabled_j = cJSON_GetObjectItem(item, "enabled");
        job->enabled     = enabled_j ? cJSON_IsTrue(enabled_j) : true;

        cJSON *delete_j       = cJSON_GetObjectItem(item, "delete_after_run");
        job->delete_after_run = delete_j ? cJSON_IsTrue(delete_j) : false;

        if (strcmp(kind_str, "every") == 0) {
            job->kind       = CRON_KIND_EVERY;
            cJSON *interval = cJSON_GetObjectItem(item, "interval_s");
            job->interval_s = (interval && cJSON_IsNumber(interval))
                              ? (uint32_t)interval->valuedouble : 0;
        } else if (strcmp(kind_str, "at") == 0) {
            job->kind       = CRON_KIND_AT;
            cJSON *at_epoch = cJSON_GetObjectItem(item, "at_epoch");
            job->at_epoch   = (at_epoch && cJSON_IsNumber(at_epoch))
                              ? (int64_t)at_epoch->valuedouble : 0;
        } else {
            continue;
        }

        cJSON *last_run = cJSON_GetObjectItem(item, "last_run");
        job->last_run   = (last_run && cJSON_IsNumber(last_run))
                          ? (int64_t)last_run->valuedouble : 0;

        cJSON *next_run = cJSON_GetObjectItem(item, "next_run");
        job->next_run   = (next_run && cJSON_IsNumber(next_run))
                          ? (int64_t)next_run->valuedouble : 0;

        s_job_count++;
    }

    cJSON_Delete(root);

    PR_INFO("Loaded %d cron jobs", s_job_count);
    return OPRT_OK;
}

/**
 * @brief Save all cron jobs to JSON file
 */
static OPERATE_RET cron_save_jobs(void)
{
    cJSON *root     = cJSON_CreateObject();
    cJSON *jobs_arr = cJSON_CreateArray();
    if (!root || !jobs_arr) {
        cJSON_Delete(root);
        cJSON_Delete(jobs_arr);
        return OPRT_MALLOC_FAILED;
    }

    for (int i = 0; i < s_job_count; i++) {
        cron_job_t *job  = &s_jobs[i];
        cJSON      *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }

        cJSON_AddStringToObject(item, "id", job->id);
        cJSON_AddStringToObject(item, "name", job->name);
        cJSON_AddBoolToObject(item, "enabled", job->enabled);
        cJSON_AddStringToObject(item, "kind",
                                job->kind == CRON_KIND_EVERY ? "every" : "at");

        if (job->kind == CRON_KIND_EVERY) {
            cJSON_AddNumberToObject(item, "interval_s", job->interval_s);
        } else {
            cJSON_AddNumberToObject(item, "at_epoch", (double)job->at_epoch);
        }

        cJSON_AddStringToObject(item, "message", job->message);
        cJSON_AddNumberToObject(item, "last_run", (double)job->last_run);
        cJSON_AddNumberToObject(item, "next_run", (double)job->next_run);
        cJSON_AddBoolToObject(item, "delete_after_run", job->delete_after_run);

        cJSON_AddItemToArray(jobs_arr, item);
    }

    cJSON_AddItemToObject(root, "jobs", jobs_arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        return OPRT_MALLOC_FAILED;
    }

    TUYA_FILE f = claw_fopen(CRON_FILE, "w");
    if (!f) {
        cJSON_free(json_str);
        PR_ERR("Failed to open %s for writing", CRON_FILE);
        return OPRT_COM_ERROR;
    }

    size_t len = strlen(json_str);
    int    wn  = claw_fwrite(json_str, (int)len, f);
    claw_fclose(f);
    cJSON_free(json_str);

    if (wn < 0 || (size_t)wn != len) {
        PR_ERR("Cron save incomplete: %d/%u", wn, (unsigned)len);
        return OPRT_COM_ERROR;
    }

    PR_DEBUG("Saved %d cron jobs", s_job_count);
    return OPRT_OK;
}

/**
 * cron_process_due_jobs - Check and fire all due cron jobs.
 *
 * Three-way logic per job:
 *   1. Not yet due (scheduled time in the future) → skip.
 *   2. Overdue (AT job whose scheduled time passed beyond CRON_OVERDUE_THRESHOLD_S
 *      and has never been executed) → report to AI, disable job, let AI call
 *      cron_remove after confirming with the user.
 *   3. Reached reminder time (within normal window) → send reminder message to AI,
 *      then delete (or disable) the job.
 */
static void cron_process_due_jobs(void)
{
    bool changed = false;

    /* Abort if device clock is not synced yet */
    int64_t check_now = cron_now_epoch();
    PR_NOTICE("cron_now_epoch: %lld, job count: %d", (long long)check_now, s_job_count);
    if (check_now < 1000000000LL) {
        return;
    }

    for (int i = 0; i < s_job_count; i++) {
        int64_t now = cron_now_epoch();

        cron_job_t *job = &s_jobs[i];
        if (!job->enabled) {
            continue;
        }

        /* Determine reference epoch: AT jobs use at_epoch, EVERY jobs use next_run */
        int64_t due_epoch = (job->kind == CRON_KIND_AT) ? job->at_epoch : job->next_run;

        /* Case 1: Not yet due → skip */
        if (due_epoch <= 0 || due_epoch > now) {
            PR_DEBUG("cron skip: '%s' due= %lld now= %lld", job->name,
                     (long long)due_epoch, (long long)now);
            continue;
        }

        /* Case 2 (AT only): Overdue — scheduled time passed beyond threshold, never run */
        if (job->kind == CRON_KIND_AT) {
            int64_t late_secs = now - job->at_epoch;
            bool is_overdue   = (late_secs > (int64_t)CRON_OVERDUE_THRESHOLD_S)
                                && (job->last_run == 0);

            if (is_overdue) {
                PR_WARN("Cron overdue: '%s' (%s) missed by %llds", job->name, job->id,
                        (long long)late_secs);
                char *msg = (char *)claw_malloc(512);
                if (!msg) {
                    PR_ERR("cron: malloc failed for overdue msg");
                    job->enabled = false;
                    changed = true;
                    continue;
                }
                snprintf(msg, 512,
                         "[Task Overdue] Task '%s' (id=%s) was scheduled at epoch %lld "
                         "but missed its trigger window by %llds. "
                         "Please inform the user that this reminder was not delivered on time, "
                         "and ask whether to delete or reschedule it. "
                         "To delete, call cron_remove with job_id='%s'.",
                         job->name, job->id, (long long)job->at_epoch,
                         (long long)late_secs, job->id);
                PR_INFO("Cron overdue msg: %s", msg);
                sys_msg_t im = {0};
                strncpy(im.channel, SYS_CHAN_CRON, sizeof(im.channel) - 1);
                im.content = msg;
                (void)sys_bus_push_inbound(&im);
                /* Disable so it is not reported again on the next check */
                job->enabled = false;
                changed = true;
                continue;
            }
        }

        /* Case 3: Reached reminder time → remind user, then delete/reschedule */
        PR_INFO("Cron firing: '%s' (%s)", job->name, job->id);
        {
            /* Wrap the raw message with a [Cron Reminder] frame so the AI
             * knows this is a scheduled reminder to relay, not a user message. */
            const char *fmt =
                "[Cron Reminder] The following scheduled reminder just fired "
                "(job '%s', id=%s). Deliver this message to the user in a "
                "warm, friendly reminder tone:\n%s";
            size_t needed = strlen(fmt) + strlen(job->name) +
                            strlen(job->id) + strlen(job->message) + 1;
            sys_msg_t im = {0};
            strncpy(im.channel, SYS_CHAN_CRON, sizeof(im.channel) - 1);
            im.content = claw_malloc(needed);
            if (im.content) {
                snprintf(im.content, needed, fmt, job->name, job->id, job->message);
                (void)sys_bus_push_inbound(&im);
            } else {
                PR_ERR("cron: malloc failed for job '%s'", job->name);
            }
        }
        job->last_run = now;

        if (job->kind == CRON_KIND_AT) {
            if (job->delete_after_run) {
                PR_INFO("Deleting one-shot cron: %s", job->id);
                for (int j = i; j < s_job_count - 1; j++) {
                    s_jobs[j] = s_jobs[j + 1];
                }
                s_job_count--;
                i--;
            } else {
                job->enabled  = false;
                job->next_run = 0;
            }
        } else {
            job->next_run = now + job->interval_s;
        }
        changed = true;
    }

    if (changed) {
        (void)cron_save_jobs();
    }
}

/**
 * @brief Cron background thread main loop
 */
static void cron_task_main(void *arg)
{
    (void)arg;

    while (1) {
        tal_system_sleep(CLAW_CRON_CHECK_INTERVAL_MS);
        cron_process_due_jobs();
    }
}

/* ===== Public API ===== */

OPERATE_RET cron_service_init(void)
{
    return cron_load_jobs();
}

OPERATE_RET cron_service_start(void)
{
    if (s_cron_thread) {
        PR_WARN("Cron thread already running");
        return OPRT_OK;
    }

    int64_t now = cron_now_epoch();
    for (int i = 0; i < s_job_count; i++) {
        cron_job_t *job = &s_jobs[i];
        if (job->enabled && job->next_run <= 0) {
            if (job->kind == CRON_KIND_EVERY) {
                job->next_run = now + job->interval_s;
            } else if (job->kind == CRON_KIND_AT && job->at_epoch > now) {
                job->next_run = job->at_epoch;
            }
        }
    }

    THREAD_CFG_T cfg = {0};
    cfg.stackDepth   = CRON_THREAD_STACK;
    cfg.priority     = THREAD_PRIO_1;
    cfg.thrdname     = "claw_cron";
#if defined(ENABLE_EXT_RAM) && (ENABLE_EXT_RAM == 1)
    cfg.psram_mode = 1;
#endif

    OPERATE_RET rt = tal_thread_create_and_start(&s_cron_thread, NULL, NULL,
                                                  cron_task_main, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("Create cron thread failed: %d", rt);
        s_cron_thread = NULL;
        return rt;
    }
    PR_INFO("Device Free heap after cron start %u", (unsigned)tal_system_get_free_heap_size());

    PR_INFO("Cron started (%d jobs, check every %u sec)", s_job_count,
            (unsigned)(CLAW_CRON_CHECK_INTERVAL_MS / 1000));
    return OPRT_OK;
}

void cron_service_stop(void)
{
    if (s_cron_thread) {
        (void)tal_thread_delete(s_cron_thread);
        s_cron_thread = NULL;
        PR_INFO("Cron stopped");
    }
}

OPERATE_RET cron_add_job(cron_job_t *job)
{
    if (!job) {
        return OPRT_INVALID_PARM;
    }

    if (s_job_count >= CLAW_CRON_MAX_JOBS) {
        PR_WARN("Max cron jobs reached (%d)", CLAW_CRON_MAX_JOBS);
        return OPRT_COM_ERROR;
    }

    cron_generate_id(job->id, sizeof(job->id));

    job->enabled  = true;
    job->last_run = 0;
    compute_initial_next_run(job);

    s_jobs[s_job_count] = *job;
    s_job_count++;

    (void)cron_save_jobs();

    PR_INFO("Added cron job: %s (%s) kind=%s next=%lld",
            job->name, job->id,
            job->kind == CRON_KIND_EVERY ? "every" : "at",
            (long long)job->next_run);
    return OPRT_OK;
}

OPERATE_RET cron_remove_job(const char *job_id)
{
    if (!job_id || job_id[0] == '\0') {
        return OPRT_INVALID_PARM;
    }

    for (int i = 0; i < s_job_count; i++) {
        if (strcmp(s_jobs[i].id, job_id) == 0) {
            for (int j = i; j < s_job_count - 1; j++) {
                s_jobs[j] = s_jobs[j + 1];
            }
            s_job_count--;
            (void)cron_save_jobs();
            return OPRT_OK;
        }
    }

    return OPRT_NOT_FOUND;
}

void cron_list_jobs(const cron_job_t **jobs, int *count)
{
    if (jobs) {
        *jobs = s_jobs;
    }
    if (count) {
        *count = s_job_count;
    }
}
