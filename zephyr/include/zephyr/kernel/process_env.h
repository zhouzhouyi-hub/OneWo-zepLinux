/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optimized Environment Variables with Pre-allocated Storage
 * Based on Embox's task_env design
 */

#ifndef ZEPHYR_INCLUDE_KERNEL_PROCESS_ENV_H_
#define ZEPHYR_INCLUDE_KERNEL_PROCESS_ENV_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Configuration */
#ifndef CONFIG_MAX_ENV_VARS
#define CONFIG_MAX_ENV_VARS 8  // 减少到 8 个（从 16）
#endif

#ifndef CONFIG_MAX_ENV_STR_LEN
#define CONFIG_MAX_ENV_STR_LEN 32  // 减少到 32 字节（从 64）
#endif

/**
 * @brief Pre-allocated environment variable storage
 *
 * Similar to Embox's task_env, uses fixed-size storage to avoid
 * malloc fragmentation.
 */
struct task_env {
	struct {
		char key[CONFIG_MAX_ENV_STR_LEN];
		char value[CONFIG_MAX_ENV_STR_LEN];
	} entries[CONFIG_MAX_ENV_VARS];
	uint16_t allocated_mask;  /* Bitmap of used slots */
};

/**
 * @brief Initialize environment storage
 */
static inline void task_env_init(struct task_env *env)
{
	if (env) {
		env->allocated_mask = 0;
	}
}

/**
 * @brief Get environment variable
 *
 * @param env Environment storage
 * @param name Variable name
 * @return Value or NULL if not found
 */
static inline const char *task_env_get(struct task_env *env, const char *name)
{
	if (!env || !name) {
		return NULL;
	}

	for (int i = 0; i < CONFIG_MAX_ENV_VARS; i++) {
		if ((env->allocated_mask & BIT(i)) &&
		    strcmp(env->entries[i].key, name) == 0) {
			return env->entries[i].value;
		}
	}

	return NULL;
}

/**
 * @brief Set environment variable
 *
 * @param env Environment storage
 * @param name Variable name
 * @param value Variable value
 * @return 0 on success, negative errno on failure
 */
static inline int task_env_set(struct task_env *env, const char *name, const char *value)
{
	if (!env || !name) {
		return -EINVAL;
	}

	/* Check if already exists */
	for (int i = 0; i < CONFIG_MAX_ENV_VARS; i++) {
		if ((env->allocated_mask & BIT(i)) &&
		    strcmp(env->entries[i].key, name) == 0) {
			/* Update existing */
			strncpy(env->entries[i].value, value ? value : "",
				CONFIG_MAX_ENV_STR_LEN - 1);
			env->entries[i].value[CONFIG_MAX_ENV_STR_LEN - 1] = '\0';
			return 0;
		}
	}

	/* Find free slot */
	for (int i = 0; i < CONFIG_MAX_ENV_VARS; i++) {
		if (!(env->allocated_mask & BIT(i))) {
			strncpy(env->entries[i].key, name, CONFIG_MAX_ENV_STR_LEN - 1);
			env->entries[i].key[CONFIG_MAX_ENV_STR_LEN - 1] = '\0';
			strncpy(env->entries[i].value, value ? value : "",
				CONFIG_MAX_ENV_STR_LEN - 1);
			env->entries[i].value[CONFIG_MAX_ENV_STR_LEN - 1] = '\0';
			env->allocated_mask |= BIT(i);
			return 0;
		}
	}

	return -ENOMEM;  /* No free slots */
}

/**
 * @brief Unset environment variable
 *
 * @param env Environment storage
 * @param name Variable name
 * @return 0 on success, negative errno on failure
 */
static inline int task_env_unset(struct task_env *env, const char *name)
{
	if (!env || !name) {
		return -EINVAL;
	}

	for (int i = 0; i < CONFIG_MAX_ENV_VARS; i++) {
		if ((env->allocated_mask & BIT(i)) &&
		    strcmp(env->entries[i].key, name) == 0) {
			env->allocated_mask &= ~BIT(i);
			return 0;
		}
	}

	return -ENOENT;
}

/**
 * @brief Copy environment from parent to child (for fork)
 *
 * @param child Child environment
 * @param parent Parent environment
 */
static inline void task_env_inherit(struct task_env *child, const struct task_env *parent)
{
	if (child && parent) {
		/* Simple memcpy - no malloc needed! */
		memcpy(child, parent, sizeof(struct task_env));
	}
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_KERNEL_PROCESS_ENV_H_ */
