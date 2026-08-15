/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simplified new_task/waitpid stubs for Phase 1 testing
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel/process.h>
#include <errno.h>

#define TASK_STACK_SIZE 2048
#define MAX_STACK_POOL 8

/* Static stack pool */
K_THREAD_STACK_ARRAY_DEFINE(test_stack_pool, MAX_STACK_POOL, TASK_STACK_SIZE);
static bool test_stack_pool_used[MAX_STACK_POOL];
static struct k_mutex test_stack_pool_lock;

/* Process exit message structure */
struct process_exit_msg {
	pid_t pid;
	int exit_code;
};

/* Process wait queue for waitpid() */
K_MSGQ_DEFINE(test_process_exit_queue, sizeof(struct process_exit_msg), 16, 4);

/* Stack tracking for cleanup */
#define MAX_TRACKED_STACKS 16
static struct {
	pid_t pid;
	int stack_index;
	struct k_thread *thread;
} test_stack_tracker[MAX_TRACKED_STACKS];
static struct k_mutex test_stack_tracker_lock;

/* Thread entry point trampoline */
struct task_trampoline_arg {
	void *(*run)(void *);
	void *run_arg;
};

static void test_task_trampoline(void *arg1, void *arg2, void *arg3)
{
	struct task_trampoline_arg *arg = (struct task_trampoline_arg *)arg1;
	void *result;

	k_msleep(10); /* Small delay to ensure parent is ready */

	if (!arg || !arg->run) {
		return;
	}

	/* Execute user function */
	result = arg->run(arg->run_arg);

	/* Send exit notification */
	struct z_process *proc = process_current();
	if (proc) {
		struct process_exit_msg msg = {
			.pid = proc->pid,
			.exit_code = (int)(intptr_t)result,
		};
		k_msgq_put(&test_process_exit_queue, &msg, K_NO_WAIT);
	}

	/* Free trampoline argument */
	k_free(arg);

	/* Yield to let waitpid start processing before we exit */
	k_yield();
	k_msleep(100);
}

/**
 * @brief Create a new task/process
 */
pid_t new_task(const char *name, void *(*run)(void *), void *arg)
{
	if (!run) {
		return -EINVAL;
	}

	/* Get current process as parent */
	struct z_process *parent = process_current();
	if (!parent) {
		return -ESRCH;
	}

	/* Create new process */
	struct z_process *child = process_create(parent);
	if (!child) {
		return -ENOMEM;
	}

	/* Allocate thread stack from pool */
	k_thread_stack_t *stack = NULL;
	int stack_index = -1;

	k_mutex_lock(&test_stack_pool_lock, K_FOREVER);
	for (int i = 0; i < MAX_STACK_POOL; i++) {
		if (!test_stack_pool_used[i]) {
			test_stack_pool_used[i] = true;
			stack = &test_stack_pool[i][0];
			stack_index = i;
			break;
		}
	}
	k_mutex_unlock(&test_stack_pool_lock);

	if (!stack) {
		process_exit(child, -ENOMEM);
		return -ENOMEM;
	}

	/* Allocate thread structure */
	struct k_thread *thread = k_malloc(sizeof(struct k_thread));
	if (!thread) {
		k_mutex_lock(&test_stack_pool_lock, K_FOREVER);
		test_stack_pool_used[stack_index] = false;
		k_mutex_unlock(&test_stack_pool_lock);
		process_exit(child, -ENOMEM);
		return -ENOMEM;
	}

	/* Allocate trampoline argument */
	struct task_trampoline_arg *tramp_arg = k_malloc(sizeof(struct task_trampoline_arg));
	if (!tramp_arg) {
		k_free(thread);
		k_mutex_lock(&test_stack_pool_lock, K_FOREVER);
		test_stack_pool_used[stack_index] = false;
		k_mutex_unlock(&test_stack_pool_lock);
		process_exit(child, -ENOMEM);
		return -ENOMEM;
	}

	tramp_arg->run = run;
	tramp_arg->run_arg = arg;

	/* Register thread with process BEFORE starting it */
	process_register_thread(child, thread);

	/* Create and start thread */
	k_tid_t tid = k_thread_create(
		thread,
		stack,
		K_THREAD_STACK_SIZEOF(TASK_STACK_SIZE),
		test_task_trampoline,
		tramp_arg,
		NULL,
		NULL,
		K_PRIO_PREEMPT(7),
		0,
		K_NO_WAIT
	);

	if (!tid) {
		process_unregister_thread(child, thread);
		k_free(tramp_arg);
		k_free(thread);
		k_mutex_lock(&test_stack_pool_lock, K_FOREVER);
		test_stack_pool_used[stack_index] = false;
		k_mutex_unlock(&test_stack_pool_lock);
		process_exit(child, -EAGAIN);
		return -EAGAIN;
	}

	/* Store stack index and thread pointers for later cleanup */
	k_mutex_lock(&test_stack_tracker_lock, K_FOREVER);
	for (int i = 0; i < MAX_TRACKED_STACKS; i++) {
		if (test_stack_tracker[i].pid == 0) {
			test_stack_tracker[i].pid = child->pid;
			test_stack_tracker[i].stack_index = stack_index;
			test_stack_tracker[i].thread = thread;
			break;
		}
	}
	k_mutex_unlock(&test_stack_tracker_lock);

	/* Set thread name */
	if (name && name[0] != '\0') {
		k_thread_name_set(tid, name);
	} else {
		char default_name[16];
		snprintf(default_name, sizeof(default_name), "task_%d", child->pid);
		k_thread_name_set(tid, default_name);
	}

	return child->pid;
}

/**
 * @brief Wait for a process to complete
 */
pid_t waitpid(pid_t pid, int *status, int options)
{
	struct process_exit_msg msg;
	int ret;

	/* Wait for exit message from the specified process */
	while (1) {
		ret = k_msgq_get(&test_process_exit_queue, &msg, K_FOREVER);
		if (ret == 0) {
			if (msg.pid == pid || pid == -1) {
				if (status) {
					*status = msg.exit_code;
				}

				/* Wait for the thread to fully exit before cleanup */
				struct z_process *proc = process_get(msg.pid);

				if (proc && proc->main_thread) {
					/* Retrieve stack index from tracker */
					int stack_index = -1;

					k_mutex_lock(&test_stack_tracker_lock, K_FOREVER);
					for (int i = 0; i < MAX_TRACKED_STACKS; i++) {
						if (test_stack_tracker[i].pid == msg.pid) {
							stack_index = test_stack_tracker[i].stack_index;
							test_stack_tracker[i].pid = 0;
							test_stack_tracker[i].stack_index = -1;
							test_stack_tracker[i].thread = NULL;
							break;
						}
					}
					k_mutex_unlock(&test_stack_tracker_lock);

					/* Join the thread to ensure it's completely done */
					k_thread_join(proc->main_thread, K_FOREVER);

					/* Unregister thread from process */
					process_unregister_thread(proc, proc->main_thread);

					/* Free thread structure */
					k_free(proc->main_thread);

					/* Return stack to pool */
					if (stack_index >= 0) {
						k_mutex_lock(&test_stack_pool_lock, K_FOREVER);
						test_stack_pool_used[stack_index] = false;
						k_mutex_unlock(&test_stack_pool_lock);
					}

					/* Clean up process */
					process_exit(proc, msg.exit_code);
				}

				return msg.pid;
			} else {
				/* Not the PID we're waiting for, put it back */
				k_msgq_put(&test_process_exit_queue, &msg, K_NO_WAIT);
				k_msleep(10);
			}
		}
	}

	return -ECHILD;
}

/* Initialize mutexes */
static int test_shell_process_init(void)
{
	k_mutex_init(&test_stack_pool_lock);
	k_mutex_init(&test_stack_tracker_lock);
	return 0;
}

SYS_INIT(test_shell_process_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
