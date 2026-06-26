/**
 * @file skill_loader.h
 * @brief Skill loader for TuyaOpenClaw
 *
 * Manages skill files (.md) on the filesystem. Built-in skills are
 * installed on first boot; users / the AI can create custom skills
 * at runtime via write_file. skill_loader_build_summary() produces
 * a compact index that can be injected into the system prompt so the
 * AI knows which skills are available.
 *
 * Ported from TuyaOpen/apps/mimiclaw/skills.
 */

#pragma once

#include "tuya_cloud_types.h"

/* ---------- configurable paths (override before including) ---------- */

#ifndef CLAW_SKILLS_DIR
#include "app_base_config.h"
#define CLAW_SKILLS_DIR     CLAW_FS_ROOT_PATH "/skills"
#endif

#ifndef CLAW_SKILLS_PREFIX
#define CLAW_SKILLS_PREFIX  CLAW_SKILLS_DIR "/"
#endif

/* ---------- public API ---------- */

/**
 * @brief Initialize the skill loader
 *
 * Creates the skills directory if missing and installs built-in
 * skill files that don't already exist on disk.
 *
 * @return OPRT_OK on success
 */
OPERATE_RET skill_loader_init(void);

/**
 * @brief Build a one-line-per-skill summary into @p buf
 *
 * Scans CLAW_SKILLS_DIR for *.md files, reads each title and
 * first-paragraph description, and writes Markdown bullet items.
 *
 * @param buf   Destination buffer
 * @param size  Buffer capacity (bytes)
 * @return      Number of bytes written (excluding NUL)
 */
size_t skill_loader_build_summary(char *buf, size_t size);
