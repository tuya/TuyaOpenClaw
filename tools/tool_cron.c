/**
 * @file tool_cron.c
 * @brief MCP cron (scheduled task) tools for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 * Implements cron_add, cron_list, and cron_remove MCP tools
 * using the ai_mcp_server.h interface.
 */

#include "tool_cron.h"
#include "tool_files.h"
#include "cron_service.h"

#include "tal_api.h"
#include "tal_time_service.h"

#include <string.h>
#include <stdlib.h>

/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************function define**********************
***********************************************************/
static bool __get_int_prop(const MCP_PROPERTY_LIST_T *properties, const char *name, int *out);
static OPERATE_RET __resolve_local_datetime_to_epoch(int year, int month, int day,
                                                     int hour, int minute, int second,
                                                     int64_t *epoch_out, char *result,
                                                     size_t result_size);
/**
 * __to_local_tm - Convert a UTC epoch to local broken-down time.
 * @epoch:          UTC Unix timestamp (int64_t); pass 0 for current time.
 *                  Internally cast to TIME_T (uint32) for SDK call.
 * @tm_out:         Output broken-down time struct (POSIX_TM_S convention).
 * @tz_label_buf:   Buffer for a human-readable timezone label, e.g. "UTC+8:00".
 * @tz_label_size:  Size of tz_label_buf.
 *
 * Mirrors tal_log.c: delegates to tal_time_get_local_time_custom(), which
 * applies the cloud-synced timezone offset and summer-time adjustments.
 * The timezone label is derived from tal_time_get_time_zone_seconds().
 */
static void __to_local_tm(int64_t epoch, POSIX_TM_S *tm_out,
                          char *tz_label_buf, size_t tz_label_size)
{
    memset(tm_out, 0, sizeof(*tm_out));
    tal_time_get_local_time_custom((TIME_T)epoch, tm_out);

    int tz_sec = 0;
    tal_time_get_time_zone_seconds(&tz_sec);
    int tz_h = tz_sec / 3600;
    int tz_m = (tz_sec < 0 ? -tz_sec : tz_sec) % 3600 / 60;
    snprintf(tz_label_buf, tz_label_size, "UTC%+d:%02d", tz_h, tz_m);
}

/**
 * __local_datetime_to_epoch - Convert a local broken-down datetime to UTC epoch.
 *
 * The device's cloud-synced timezone offset is applied via
 * tal_time_get_time_zone_seconds() so no timezone knowledge is required from
 * the caller.
 *
 * @year:   Full year (e.g. 2026)
 * @month:  Month 1-12
 * @day:    Day 1-31
 * @hour:   Hour 0-23
 * @minute: Minute 0-59
 * @second: Second 0-59
 * Returns UTC epoch (int64_t), or -1 on invalid input.
 */
static int64_t __local_datetime_to_epoch(int year, int month, int day,
                                         int hour, int minute, int second)
{
    /* Basic range validation */
    if (year < 1970 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return -1;
    }

    /* Days in each month (non-leap year) */
    static const int days_in_month[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

    /* Leap year check */
    bool is_leap = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    int max_day  = days_in_month[month - 1];
    if (month == 2 && is_leap) {
        max_day = 29;
    }
    if (day > max_day) {
        return -1;
    }

    /* Compute days since Unix epoch (1970-01-01) using the algorithm from
     * https://howardhinnant.github.io/date_algorithms.html (civil_from_days) */
    int y = year;
    int m = month;
    int d = day;
    if (m <= 2) {
        y -= 1;
        m += 9;
    } else {
        m -= 3;
    }
    int era        = (y >= 0 ? y : y - 399) / 400;
    int yoe        = y - era * 400;
    int doy        = (153 * m + 2) / 5 + d - 1;
    int doe        = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days   = (int64_t)era * 146097 + doe - 719468; /* days since 1970-01-01 */

    int64_t utc_epoch = days * 86400 + hour * 3600 + minute * 60 + second;

    /* Subtract timezone offset: local time = UTC + tz_offset → UTC = local - tz_offset */
    int tz_sec = 0;
    tal_time_get_time_zone_seconds(&tz_sec);
    utc_epoch -= (int64_t)tz_sec;

    return utc_epoch;
}

static OPERATE_RET __resolve_local_datetime_to_epoch(int year, int month, int day,
                                                     int hour, int minute, int second,
                                                     int64_t *epoch_out, char *result,
                                                     size_t result_size)
{
    if (!epoch_out) {
        return OPRT_INVALID_PARM;
    }

    /* Fill missing date fields from current local time. */
    if (year < 1970 || month <= 0 || day <= 0) {
        TIME_T now = tal_time_get_posix();
        POSIX_TM_S tm_now;
        memset(&tm_now, 0, sizeof(tm_now));
        tal_time_get_local_time_custom(now, &tm_now);
        if (year < 1970)  year  = tm_now.tm_year + 1900;
        if (month <= 0)   month = tm_now.tm_mon  + 1;
        if (day   <= 0)   day   = tm_now.tm_mday;
    }

    int64_t epoch = __local_datetime_to_epoch(year, month, day, hour, minute, second);
    if (epoch < 0) {
        if (result && result_size > 0) {
            snprintf(result, result_size,
                     "Error: invalid datetime %04d-%02d-%02d %02d:%02d:%02d",
                     year, month, day, hour, minute, second);
        }
        return OPRT_INVALID_PARM;
    }
    *epoch_out = epoch;

    TIME_T now_t = tal_time_get_posix();
    int64_t now  = (int64_t)now_t;
    int64_t diff = epoch - now;

    char tz_label[16];
    int tz_sec = 0;
    tal_time_get_time_zone_seconds(&tz_sec);
    int tz_h = tz_sec / 3600;
    int tz_m = (tz_sec < 0 ? -tz_sec : tz_sec) % 3600 / 60;
    snprintf(tz_label, sizeof(tz_label), "UTC%+d:%02d", tz_h, tz_m);

    if (result && result_size > 0) {
        if (diff < 0) {
            snprintf(result, result_size,
                     "epoch=%lld (local: %04d-%02d-%02d %02d:%02d:%02d %s) "
                     "WARNING: this time is %lld seconds in the past.",
                     (long long)epoch, year, month, day, hour, minute, second,
                     tz_label, (long long)(-diff));
        } else {
            snprintf(result, result_size,
                     "epoch=%lld (local: %04d-%02d-%02d %02d:%02d:%02d %s, "
                     "in %lld seconds from now)",
                     (long long)epoch, year, month, day, hour, minute, second,
                     tz_label, (long long)diff);
        }
    }

    PR_DEBUG("time_to_epoch: %04d-%02d-%02d %02d:%02d:%02d -> epoch=%lld",
             year, month, day, hour, minute, second, (long long)epoch);
    return OPRT_OK;
}

/**
 * __tool_get_current_time - MCP tool callback: get_current_time
 *
 * Returns the device's current UTC epoch and the local time formatted
 * according to the cloud-synced timezone (via tal_time_get_local_time_custom).
 */
static OPERATE_RET __tool_get_current_time(const MCP_PROPERTY_LIST_T *properties,
                                           MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    TIME_T now = tal_time_get_posix();

    if (now <= 0) {
        ai_mcp_return_value_set_str(ret_val, "Error: device time not synced yet");
        return OPRT_COM_ERROR;
    }

    POSIX_TM_S tm_local;
    char tz_label[16];
    /* Pass 0 so tal_time_get_local_time_custom uses current time internally */
    __to_local_tm(0, &tm_local, tz_label, sizeof(tz_label));

    char *result = (char *)claw_malloc(512);
    if (!result) { return OPRT_MALLOC_FAILED; }
    snprintf(result, 512,
             "Current time: %04d-%02d-%02d %02d:%02d:%02d %s (UTC epoch=%lld). "
             "To schedule a reminder, call cron_add with the desired "
             "year/month/day/hour/minute/second. For relative delays such as "
             "'in 5 minutes', first compute the next absolute local time, then "
             "pass those fields directly to cron_add.",
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
             tz_label, (long long)now);

    /* ai_mcp_return_value_set_str() copies via mm_strdup, safe to free after */
    ai_mcp_return_value_set_str(ret_val, result);
    PR_DEBUG("get_current_time: epoch=%lld local=%04d-%02d-%02d %02d:%02d:%02d %s",
             (long long)now,
             tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
             tz_label);
    claw_free(result);
    return OPRT_OK;
}

/**
 * @brief Helper to extract a string property from MCP property list
 */
static const char *__get_str_prop(const MCP_PROPERTY_LIST_T *properties, const char *name)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_STRING) {
        return prop->default_val.str_val;
    }
    return NULL;
}

/**
 * @brief Helper to extract an integer property from MCP property list
 */
static bool __get_int_prop(const MCP_PROPERTY_LIST_T *properties, const char *name, int *out)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_INTEGER) {
        *out = prop->default_val.int_val;
        return true;
    }
    return false;
}

/**
 * @brief Helper to extract a boolean property from MCP property list
 */
static bool __get_bool_prop(const MCP_PROPERTY_LIST_T *properties, const char *name, bool *out)
{
    const MCP_PROPERTY_T *prop = ai_mcp_property_list_find(properties, name);
    if (prop && prop->type == MCP_PROPERTY_TYPE_BOOLEAN) {
        *out = prop->default_val.bool_val;
        return true;
    }
    return false;
}

/**
 * @brief MCP tool callback: cron_add
 *
 * Adds a new cron job (recurring "every" or one-shot "at").
 *
 * Properties:
 * - name (string, required): Descriptive name for the job
 * - schedule_type (string, required): "every" or "at"
 * - message (string, required): Message to send when job fires
 * - interval_s (int, optional): Interval in seconds for "every" schedule
 * - at_epoch (int, optional): Unix timestamp for "at" schedule
 * - delete_after_run (bool, optional): Whether to delete after firing (default true for "at")
 */
static OPERATE_RET __tool_cron_add(const MCP_PROPERTY_LIST_T *properties,
                                   MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *name          = __get_str_prop(properties, "name");
    const char *schedule_type = __get_str_prop(properties, "schedule_type");
    const char *message       = __get_str_prop(properties, "message");

    if (!name || !schedule_type || !message) {
        ai_mcp_return_value_set_str(ret_val, "Error: missing required fields (name, schedule_type, message)");
        return OPRT_INVALID_PARM;
    }

    if (message[0] == '\0') {
        ai_mcp_return_value_set_str(ret_val, "Error: message must not be empty");
        return OPRT_INVALID_PARM;
    }

    cron_job_t job;
    memset(&job, 0, sizeof(job));
    strncpy(job.name, name, sizeof(job.name) - 1);
    strncpy(job.message, message, sizeof(job.message) - 1);

    if (strcmp(schedule_type, "every") == 0) {
        job.kind = CRON_KIND_EVERY;

        int interval_s = 0;
        if (!__get_int_prop(properties, "interval_s", &interval_s) || interval_s <= 0) {
            ai_mcp_return_value_set_str(ret_val, "Error: 'every' schedule requires positive 'interval_s'");
            return OPRT_INVALID_PARM;
        }

        job.interval_s       = (uint32_t)interval_s;
        job.delete_after_run = false;
    } else if (strcmp(schedule_type, "at") == 0) {
        job.kind = CRON_KIND_AT;

        /* Compute at_epoch inside cron_add from local datetime fields.
         * at_epoch is kept as a compatibility fallback for older callers. */
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        int at_epoch = 0;
        bool has_at_epoch = __get_int_prop(properties, "at_epoch", &at_epoch) && at_epoch > 0;

        bool has_hour   = __get_int_prop(properties, "hour",   &hour);
        bool has_minute = __get_int_prop(properties, "minute", &minute);

        if (has_hour || has_minute) {
            char err_msg[160];

            __get_int_prop(properties, "year",   &year);
            __get_int_prop(properties, "month",  &month);
            __get_int_prop(properties, "day",    &day);
            __get_int_prop(properties, "second", &second);

            if (__resolve_local_datetime_to_epoch(year, month, day,
                                                  hour, minute, second,
                                                  &job.at_epoch, err_msg,
                                                  sizeof(err_msg)) != OPRT_OK) {
                ai_mcp_return_value_set_str(ret_val, err_msg);
                return OPRT_INVALID_PARM;
            }
        } else if (has_at_epoch) {
            job.at_epoch = (int64_t)at_epoch;
            PR_DEBUG("cron_add: using compatibility at_epoch=%d", at_epoch);
        } else {
            ai_mcp_return_value_set_str(ret_val,
                "Error: 'at' schedule requires year/month/day/hour/minute "
                "or compatibility at_epoch");
            return OPRT_INVALID_PARM;
        }

        int64_t now = (int64_t)tal_time_get_posix();
        if (job.at_epoch <= now) {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg),
                     "Error: at_epoch %lld is in the past (now=%lld)",
                     (long long)job.at_epoch, (long long)now);
            ai_mcp_return_value_set_str(ret_val, err_msg);
            return OPRT_INVALID_PARM;
        }

        bool delete_after = true;
        if (__get_bool_prop(properties, "delete_after_run", &delete_after)) {
            job.delete_after_run = delete_after;
        } else {
            job.delete_after_run = true;
        }
    } else {
        ai_mcp_return_value_set_str(ret_val, "Error: schedule_type must be 'every' or 'at'");
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = cron_add_job(&job);
    if (rt != OPRT_OK) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Error: failed to add job (rt=%d)", rt);
        ai_mcp_return_value_set_str(ret_val, err_msg);
        return rt;
    }

    /* Tool-chain verification: confirm the job is present in the persisted list.
     * cron_add_job() saves to CRON_FILE (/sdcard/cron.json or equivalent).
     * Reading back via cron_list_jobs() ensures the write was successful. */
    const cron_job_t *verify_jobs  = NULL;
    int               verify_count = 0;
    cron_list_jobs(&verify_jobs, &verify_count);

    bool found = false;
    for (int i = 0; i < verify_count && !found; i++) {
        if (strcmp(verify_jobs[i].id, job.id) == 0) {
            found = true;
        }
    }

    if (!found) {
        char *err_msg = (char *)claw_malloc(256);
        if (!err_msg) { return OPRT_MALLOC_FAILED; }
        snprintf(err_msg, 256,
                 "Error: job '%s' (id=%s) was scheduled but not found in stored list "
                 "- possible file write failure. Please retry.",
                 job.name, job.id);
        PR_ERR("cron_add verify failed: %s", err_msg);
        /* ai_mcp_return_value_set_str() copies via mm_strdup, safe to free after */
        ai_mcp_return_value_set_str(ret_val, err_msg);
        claw_free(err_msg);
        return OPRT_FILE_WRITE_FAILED;
    }

    /* Format local time of the next run for user-readable confirmation */
    POSIX_TM_S tm_next;
    char tz_label[16];
    __to_local_tm(job.next_run, &tm_next, tz_label, sizeof(tz_label));

    char *result = (char *)claw_malloc(320);
    if (!result) {
        return OPRT_MALLOC_FAILED;
    }
    if (job.kind == CRON_KIND_EVERY) {
        snprintf(result, 320,
                 "OK: Added recurring job '%s' (id=%s), runs every %lu seconds. "
                 "Next run at epoch %lld (%04d-%02d-%02d %02d:%02d:%02d %s). "
                 "Verified in stored list.",
                 job.name, job.id, (unsigned long)job.interval_s, (long long)job.next_run,
                 tm_next.tm_year + 1900, tm_next.tm_mon + 1, tm_next.tm_mday,
                 tm_next.tm_hour, tm_next.tm_min, tm_next.tm_sec, tz_label);
    } else {
        POSIX_TM_S tm_at;
        char tz_label_at[16];
        __to_local_tm(job.at_epoch, &tm_at, tz_label_at, sizeof(tz_label_at));
        snprintf(result, 320,
                 "OK: Added one-shot job '%s' (id=%s), fires at epoch %lld "
                 "(%04d-%02d-%02d %02d:%02d:%02d %s).%s Verified in stored list.",
                 job.name, job.id, (long long)job.at_epoch,
                 tm_at.tm_year + 1900, tm_at.tm_mon + 1, tm_at.tm_mday,
                 tm_at.tm_hour, tm_at.tm_min, tm_at.tm_sec, tz_label_at,
                 job.delete_after_run ? " Will be deleted after firing." : "");
    }

    /* ai_mcp_return_value_set_str() copies via mm_strdup, safe to free after */
    ai_mcp_return_value_set_str(ret_val, result);
    PR_DEBUG("cron_add: %s", result);
    claw_free(result);
    return OPRT_OK;
}

/**
 * @brief MCP tool callback: cron_list
 *
 * Lists all scheduled cron jobs.
 */
static OPERATE_RET __tool_cron_list(const MCP_PROPERTY_LIST_T *properties,
                                    MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const cron_job_t *jobs  = NULL;
    int               count = 0;
    cron_list_jobs(&jobs, &count);

    if (count == 0) {
        ai_mcp_return_value_set_str(ret_val, "No cron jobs scheduled.");
        return OPRT_OK;
    }

    /* Build listing string */
    size_t buf_size = 4096;
    char *buf = (char *)claw_malloc(buf_size);
    if (!buf) {
        return OPRT_MALLOC_FAILED;
    }

    size_t off = 0;
    off += (size_t)snprintf(buf + off, buf_size - off, "Scheduled jobs (%d):\n", count);

    for (int i = 0; i < count && off < buf_size - 1; i++) {
        const cron_job_t *j = &jobs[i];

        if (j->kind == CRON_KIND_EVERY) {
            off += (size_t)snprintf(buf + off, buf_size - off,
                                    "  %d. [%s] \"%s\" - every %lus, %s, next=%lld, last=%lld\n",
                                    i + 1, j->id, j->name,
                                    (unsigned long)j->interval_s,
                                    j->enabled ? "enabled" : "disabled",
                                    (long long)j->next_run, (long long)j->last_run);
        } else {
            off += (size_t)snprintf(buf + off, buf_size - off,
                                    "  %d. [%s] \"%s\" - at %lld, %s, last=%lld%s\n",
                                    i + 1, j->id, j->name,
                                    (long long)j->at_epoch,
                                    j->enabled ? "enabled" : "disabled",
                                    (long long)j->last_run,
                                    j->delete_after_run ? " (auto-delete)" : "");
        }
    }

    ai_mcp_return_value_set_str(ret_val, buf);
    claw_free(buf);

    PR_DEBUG("cron_list: %d jobs", count);
    return OPRT_OK;
}

/**
 * @brief MCP tool callback: cron_remove
 *
 * Removes a cron job by its ID.
 *
 * Properties:
 * - job_id (string, required): The ID of the cron job to remove
 */
static OPERATE_RET __tool_cron_remove(const MCP_PROPERTY_LIST_T *properties,
                                      MCP_RETURN_VALUE_T *ret_val, void *user_data)
{
    const char *job_id = __get_str_prop(properties, "job_id");
    if (!job_id || job_id[0] == '\0') {
        ai_mcp_return_value_set_str(ret_val, "Error: missing 'job_id' field");
        return OPRT_INVALID_PARM;
    }

    OPERATE_RET rt = cron_remove_job(job_id);

    char result[128];
    if (rt == OPRT_OK) {
        snprintf(result, sizeof(result), "OK: Removed cron job %s", job_id);
    } else if (rt == OPRT_NOT_FOUND) {
        snprintf(result, sizeof(result), "Error: job '%s' not found", job_id);
    } else {
        snprintf(result, sizeof(result), "Error: failed to remove job (rt=%d)", rt);
    }

    ai_mcp_return_value_set_str(ret_val, result);
    PR_DEBUG("cron_remove: %s -> %d", job_id, rt);
    return rt;
}

/**
 * @brief Register all cron MCP tools
 *
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET tool_cron_register(void)
{
    OPERATE_RET rt = OPRT_OK;

    /* get_current_time tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "get_current_time",
        "Get the device's current local date and time (and UTC epoch).\n"
        "Call this when the user asks what time/date it is.\n"
        "For reminders, ALWAYS call this tool first if the user gives a relative\n"
        "delay such as 'in 5 minutes'. Use the returned current time to compute\n"
        "the next absolute local target time, then call cron_add with those\n"
        "year/month/day/hour/minute/second fields.",
        __tool_get_current_time,
        NULL
    ));

    /* cron_add tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "cron_add",
        "Add a scheduled cron job.\n"
        "Parameters:\n"
        "- name (string): Descriptive name for the job.\n"
        "- schedule_type (string): 'every' for recurring or 'at' for one-shot.\n"
        "- message (string): The reminder content to deliver when the job fires.\n"
        "  Write a brief, factual note about what the user wanted to be reminded of\n"
        "  (e.g. 'Time to drink water', 'Meeting starts in 10 minutes').\n"
        "  Do NOT write it in a conversational tone — the system frames it as a reminder automatically.\n"
        "- interval_s (int): Interval in seconds (required for 'every' type).\n"
        "For 'at' type, use this workflow:\n"
        "  1. If the request is relative (e.g. 'in 5 minutes'), call get_current_time first.\n"
        "  2. Compute the NEXT absolute local target time.\n"
        "  3. Call cron_add with year/month/day/hour/minute/second.\n"
        "  4. cron_add will internally convert that local date/time to the correct epoch.\n"
        "Primary parameters for 'at' type:\n"
        "- year (int): Full year, e.g. 2026.\n"
        "- month (int): Month 1-12.\n"
        "- day (int): Day 1-31.\n"
        "- hour (int): Hour 0-23.\n"
        "- minute (int): Minute 0-59.\n"
        "- second (int): Optional, default 0.\n"
        "- at_epoch (int): Compatibility fallback for older callers; new flows should not need it.\n"
        "- delete_after_run (bool): Whether to auto-delete after firing (default true for 'at').\n"
        "Important:\n"
        "- DO NOT pass minute=10 or minute=5 as a relative offset.\n"
        "- Always convert relative reminders into an absolute local date/time first.\n"
        "Response:\n"
        "- Returns job ID and schedule details on success, including 'Verified in stored list'.\n"
        "- If the response does NOT contain 'Verified in stored list', the file write failed;\n"
        "  call cron_list to check existing jobs, then retry cron_add.",
        __tool_cron_add,
        NULL,
        MCP_PROP_STR("name", "Descriptive name for the cron job"),
        MCP_PROP_STR("schedule_type", "'every' for recurring or 'at' for one-shot"),
        MCP_PROP_STR("message", "Message content to send when the job fires"),
        &(MCP_PROPERTY_DEF_T){ .name = "interval_s", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "Interval in seconds for 'every' schedule type",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        &(MCP_PROPERTY_DEF_T){ .name = "hour", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "Absolute local clock hour 0-23 for 'at' type; not a relative offset",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        &(MCP_PROPERTY_DEF_T){ .name = "minute", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "Absolute local clock minute 0-59 for 'at' type; not a relative offset like '5 minutes from now'",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        &(MCP_PROPERTY_DEF_T){ .name = "year", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "Full year e.g. 2026 (optional, defaults to today)",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        &(MCP_PROPERTY_DEF_T){ .name = "month", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "Month 1-12 (optional, defaults to today)",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        &(MCP_PROPERTY_DEF_T){ .name = "day", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "Day 1-31 (optional, defaults to today)",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        &(MCP_PROPERTY_DEF_T){ .name = "second", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "Second 0-59 (optional, default 0)",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        &(MCP_PROPERTY_DEF_T){ .name = "at_epoch", .type = MCP_PROPERTY_TYPE_INTEGER,
            .description = "UTC epoch compatibility fallback for older callers; new reminder flows should pass local date/time fields instead",
            .has_default = TRUE, .default_val.int_val = 0, .has_range = FALSE },
        MCP_PROP_BOOL_DEF("delete_after_run", "Auto-delete after firing (default true for 'at')", true)
    ));

    /* cron_list tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "cron_list",
        "List all scheduled cron jobs.\n"
        "Response:\n"
        "- Returns a list of all cron jobs with their IDs, names, schedules, and status.",
        __tool_cron_list,
        NULL
    ));

    /* cron_remove tool */
    TUYA_CALL_ERR_RETURN(AI_MCP_TOOL_ADD(
        "cron_remove",
        "Remove a scheduled cron job by its ID.\n"
        "Parameters:\n"
        "- job_id (string): The ID of the cron job to remove (from cron_list output).\n"
        "Response:\n"
        "- Returns confirmation of removal or error if not found.",
        __tool_cron_remove,
        NULL,
        MCP_PROP_STR("job_id", "The ID of the cron job to remove")
    ));

    PR_DEBUG("Cron MCP tools registered successfully");
    return OPRT_OK;
}
