/**
 * @file cron_service.h
 * @brief Cron (scheduled task) service for TuyaOpenClaw
 * @version 0.1
 * @date 2025-03-25
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __CRON_SERVICE_H__
#define __CRON_SERVICE_H__

#include "tuya_cloud_types.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* Maximum number of cron jobs */
#ifndef CLAW_CRON_MAX_JOBS
#define CLAW_CRON_MAX_JOBS 8
#endif

/* Cron check interval in milliseconds (60 seconds) */
#ifndef CLAW_CRON_CHECK_INTERVAL_MS
#define CLAW_CRON_CHECK_INTERVAL_MS (10 * 1000)
#endif

/***********************************************************
***********************typedef define***********************
***********************************************************/

/**
 * @brief Cron job schedule kind
 */
typedef enum {
    CRON_KIND_EVERY = 0, /**< Recurring: fires every N seconds */
    CRON_KIND_AT    = 1, /**< One-shot: fires at a specific epoch time */
} cron_kind_t;

/**
 * @brief Cron job definition
 */
typedef struct {
    char        id[9];        /**< Auto-generated unique job ID */
    char        name[32];     /**< Descriptive name */
    bool        enabled;      /**< Whether the job is active */
    cron_kind_t kind;         /**< Schedule type */
    uint32_t    interval_s;   /**< Interval for EVERY kind (seconds) */
    int64_t     at_epoch;     /**< Target epoch for AT kind */
    char        message[256]; /**< Message to inject when job fires */
    int64_t     last_run;     /**< Last execution epoch */
    int64_t     next_run;     /**< Next scheduled epoch */
    bool        delete_after_run; /**< Auto-delete after firing (AT kind) */
} cron_job_t;

/***********************************************************
********************function declaration********************
***********************************************************/

/**
 * @brief Initialize cron service (load jobs from file)
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET cron_service_init(void);

/**
 * @brief Start cron background thread
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET cron_service_start(void);

/**
 * @brief Stop cron background thread
 */
void cron_service_stop(void);

/**
 * @brief Add a new cron job
 * @param job Job definition (id and next_run will be auto-filled)
 * @return OPERATE_RET OPRT_OK on success
 */
OPERATE_RET cron_add_job(cron_job_t *job);

/**
 * @brief Remove a cron job by ID
 * @param job_id Job ID string
 * @return OPERATE_RET OPRT_OK on success, OPRT_NOT_FOUND if not found
 */
OPERATE_RET cron_remove_job(const char *job_id);

/**
 * @brief List all cron jobs
 * @param jobs Output pointer to the jobs array
 * @param count Output job count
 */
void cron_list_jobs(const cron_job_t **jobs, int *count);

#ifdef __cplusplus
}
#endif

#endif /* __CRON_SERVICE_H__ */
