/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel/process.h>
#include <zephyr/init.h>
#include <string.h>
#include <errno.h>

/* Default values if not configured */
#ifndef CONFIG_MAX_PROCESS_COUNT
#define CONFIG_MAX_PROCESS_COUNT 16
#endif

#ifndef CONFIG_MAX_FD_PER_PROCESS
#define CONFIG_MAX_FD_PER_PROCESS 16
#endif

/* Helper function to duplicate string */
static char *z_strdup(const char *str)
{
	if (!str) {
		return NULL;
	}

	size_t len = strlen(str) + 1;
	char *dup = k_malloc(len);
	if (dup) {
		memcpy(dup, str, len);
	}
	return dup;
}

/* Process table - static allocation for MCU */
static struct z_process process_table[CONFIG_MAX_PROCESS_COUNT];
static uint32_t process_allocated_mask;
static pid_t next_pid = PID_INIT;

/* Lock for process table operations */
static struct k_spinlock process_lock = {};

/**
 * @brief Allocate a PID
 */
static pid_t alloc_pid(void)
{
	k_spinlock_key_t key = k_spin_lock(&process_lock);

	for (int i = 0; i < CONFIG_MAX_PROCESS_COUNT; i++) {
		if (!(process_allocated_mask & BIT(i))) {
			process_allocated_mask |= BIT(i);
			pid_t pid = next_pid++;
			k_spin_unlock(&process_lock, key);
			return pid;
		}
	}

	k_spin_unlock(&process_lock, key);
	return PID_INVALID;
}

/**
 * @brief Free a PID
 */
static void free_pid(pid_t pid)
{
	k_spinlock_key_t key = k_spin_lock(&process_lock);

	for (int i = 0; i < CONFIG_MAX_PROCESS_COUNT; i++) {
		if (process_table[i].pid == pid) {
			process_allocated_mask &= ~BIT(i);
			break;
		}
	}

	k_spin_unlock(&process_lock, key);
}

/**
 * @brief Find free process slot
 */
static struct z_process *alloc_process_slot(void)
{
	k_spinlock_key_t key = k_spin_lock(&process_lock);

	for (int i = 0; i < CONFIG_MAX_PROCESS_COUNT; i++) {
		if (!(process_allocated_mask & BIT(i))) {
			k_spin_unlock(&process_lock, key);
			return &process_table[i];
		}
	}

	k_spin_unlock(&process_lock, key);
	return NULL;
}

void z_process_init(void)
{
	memset(process_table, 0, sizeof(process_table));
	process_allocated_mask = 0;
	next_pid = PID_INIT;

	/* Initialize init process (PID 1) */
	struct z_process *init_proc = process_create(NULL);
	if (init_proc) {
		init_proc->pid = PID_INIT;
		next_pid = PID_INIT + 1;
	}
}

/* Wrapper for SYS_INIT */
static int z_process_init_wrapper(void)
{
	z_process_init();
	return 0;
}

struct z_process *process_create(struct z_process *parent)
{
	struct z_process *proc = alloc_process_slot();
	if (!proc) {
		return NULL;
	}

	pid_t pid = alloc_pid();
	if (pid == PID_INVALID) {
		return NULL;
	}

	/* Initialize process structure */
	memset(proc, 0, sizeof(struct z_process));
	proc->pid = pid;
	proc->parent = parent;
	proc->main_thread = NULL;

	sys_dlist_init(&proc->children);
	sys_dlist_init(&proc->threads);

	atomic_set(&proc->ref_count, 1);
	proc->flags = 0;

	/* Initialize vfork semaphore */
	k_sem_init(&proc->vfork_sem, 0, 1);

	/* Initialize file descriptor table */
	memset(&proc->fd_table, 0, sizeof(struct idesc_table));

	/* Initialize environment variables (optimized - no malloc) */
	task_env_init(&proc->env);

	/* Add to parent's child list */
	if (parent) {
		k_spinlock_key_t key = k_spin_lock(&process_lock);
		sys_dlist_append(&parent->children, &proc->child_node);
		k_spin_unlock(&process_lock, key);
	}

	return proc;
}

struct z_process *process_get(pid_t pid)
{
	if (pid == PID_INVALID) {
		return NULL;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	for (int i = 0; i < CONFIG_MAX_PROCESS_COUNT; i++) {
		if ((process_allocated_mask & BIT(i)) &&
		    process_table[i].pid == pid) {
			k_spin_unlock(&process_lock, key);
			return &process_table[i];
		}
	}

	k_spin_unlock(&process_lock, key);
	return NULL;
}

struct z_process *process_current(void)
{
	struct k_thread *thread = k_current_get();
	if (!thread) {
		return NULL;
	}

	/* Return thread's associated process */
	if (thread->process) {
		/* Validate that process pointer is within process_table bounds */
		uintptr_t proc_addr = (uintptr_t)thread->process;
		uintptr_t table_start = (uintptr_t)&process_table[0];
		uintptr_t table_end = (uintptr_t)&process_table[CONFIG_MAX_PROCESS_COUNT];

		if (proc_addr < table_start || proc_addr >= table_end) {
			/* Pointer outside process table, clear it */
			thread->process = NULL;
		} else {
			/* Pointer is within table, safe to access */
			/* Validate it's still allocated and matches table entry */
			size_t index = (proc_addr - table_start) / sizeof(struct z_process);
			if (index < CONFIG_MAX_PROCESS_COUNT &&
			    (process_allocated_mask & BIT(index)) &&
			    thread->process == &process_table[index]) {
				return thread->process;
			}
			/* Process pointer is stale or misaligned, clear it */
			thread->process = NULL;
		}
	}

	/* Fallback to init process if no process assigned */
	return process_get(PID_INIT);
}

/**
 * @brief Reparent all children to init process (handle orphans)
 *
 * Based on Embox's task_make_children_daemons()
 */
static void process_reparent_children(struct z_process *proc)
{
	struct z_process *init_proc = process_get(PID_INIT);
	if (!init_proc || init_proc == proc) {
		return;
	}

	sys_dnode_t *node, *tmp;
	SYS_DLIST_FOR_EACH_NODE_SAFE(&proc->children, node, tmp) {
		struct z_process *child = CONTAINER_OF(node, struct z_process, child_node);

		/* Remove from current parent */
		sys_dlist_remove(&child->child_node);

		/* Add to init process */
		child->parent = init_proc;
		sys_dlist_append(&init_proc->children, &child->child_node);
	}
}

void process_exit(struct z_process *proc, int exit_code)
{
	if (!proc) {
		return;
	}

	proc->exit_code = exit_code;

	/* Wake parent if in vfork */
	process_vfork_wake_parent(proc);

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	/* Reparent all children to init before exit (handle orphans) */
	process_reparent_children(proc);

	/* Remove from parent's child list */
	if (proc->parent) {
		sys_dlist_remove(&proc->child_node);
		proc->parent = NULL;  /* Clear parent pointer */
	}

	/* Environment variables are pre-allocated, no cleanup needed */

	/* Close all open file descriptors (with reference counting) */
	for (int fd = 0; fd < CONFIG_MAX_FD_PER_PROCESS; fd++) {
		if (proc->fd_table.allocated_mask & BIT(fd)) {
			struct idesc *desc = (struct idesc *)proc->fd_table.entries[fd].idesc;
			if (desc) {
				/* Decrement reference count - may trigger close() */
				idesc_put(desc);
			}
		}
	}

	/* Clear file descriptor table */
	memset(&proc->fd_table, 0, sizeof(struct idesc_table));

	/* Free PID */
	free_pid(proc->pid);

	/* Mark process as invalid */
	proc->pid = PID_INVALID;

	k_spin_unlock(&process_lock, key);
}

void *process_idesc_table_get(struct z_process *proc, int fd)
{
	if (!proc || fd < 0 || fd >= CONFIG_MAX_FD_PER_PROCESS) {
		return NULL;
	}

	if (!(proc->fd_table.allocated_mask & BIT(fd))) {
		return NULL;
	}

	return proc->fd_table.entries[fd].idesc;
}

int process_idesc_table_add(struct z_process *proc, void *idesc_ptr)
{
	if (!proc || !idesc_ptr) {
		return -EINVAL;
	}

	struct idesc *desc = (struct idesc *)idesc_ptr;

	/* Increment reference count - caller is sharing this descriptor */
	idesc_get(desc);

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	/* Find first free FD */
	for (int fd = 0; fd < CONFIG_MAX_FD_PER_PROCESS; fd++) {
		if (!(proc->fd_table.allocated_mask & BIT(fd))) {
			proc->fd_table.entries[fd].idesc = idesc_ptr;
			proc->fd_table.entries[fd].flags = 0;
			proc->fd_table.allocated_mask |= BIT(fd);
			k_spin_unlock(&process_lock, key);
			return fd;
		}
	}

	k_spin_unlock(&process_lock, key);

	/* Failed to allocate FD, release the reference */
	idesc_put(desc);
	return -EMFILE;  /* Too many open files */
}

int process_idesc_table_remove(struct z_process *proc, int fd)
{
	if (!proc || fd < 0 || fd >= CONFIG_MAX_FD_PER_PROCESS) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	if (!(proc->fd_table.allocated_mask & BIT(fd))) {
		k_spin_unlock(&process_lock, key);
		return -EBADF;  /* Bad file descriptor */
	}

	/* Get the descriptor before clearing the entry */
	struct idesc *desc = (struct idesc *)proc->fd_table.entries[fd].idesc;

	/* Clear the FD entry */
	proc->fd_table.entries[fd].idesc = NULL;
	proc->fd_table.entries[fd].flags = 0;
	proc->fd_table.allocated_mask &= ~BIT(fd);

	k_spin_unlock(&process_lock, key);

	/* Decrement reference count - may trigger close() if this was the last reference */
	if (desc) {
		idesc_put(desc);
	}

	return 0;
}

const char *process_getenv(struct z_process *proc, const char *name)
{
	if (!proc || !name) {
		return NULL;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);
	const char *value = task_env_get(&proc->env, name);
	k_spin_unlock(&process_lock, key);

	return value;
}

int process_setenv(struct z_process *proc, const char *name, const char *value)
{
	if (!proc || !name) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);
	int ret = task_env_set(&proc->env, name, value);
	k_spin_unlock(&process_lock, key);

	return ret;
}

struct z_process *process_fork(struct z_process *parent)
{
	if (!parent) {
		return NULL;
	}

	struct z_process *child = process_create(parent);
	if (!child) {
		return NULL;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	/* Deep copy file descriptor table with reference counting (Embox style) */
	for (int fd = 0; fd < CONFIG_MAX_FD_PER_PROCESS; fd++) {
		if (parent->fd_table.allocated_mask & BIT(fd)) {
			struct idesc *desc = (struct idesc *)parent->fd_table.entries[fd].idesc;
			if (desc) {
				/* Increment reference count - child shares the descriptor */
				child->fd_table.entries[fd].idesc = idesc_get(desc);
				child->fd_table.entries[fd].flags = parent->fd_table.entries[fd].flags;
				child->fd_table.allocated_mask |= BIT(fd);
			}
		}
	}

	/* Copy environment variables (optimized - simple memcpy, no malloc!) */
	task_env_inherit(&child->env, &parent->env);

	k_spin_unlock(&process_lock, key);

	return child;
}

int process_register_thread(struct z_process *proc, struct k_thread *thread)
{
	if (!proc || !thread) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	/* Set thread's process pointer */
	thread->process = proc;

	/* Add thread to process's thread list */
	sys_dlist_append(&proc->threads, &thread->process_thread_node);

	/* If this is the first thread, set it as main thread */
	if (!proc->main_thread) {
		proc->main_thread = thread;
	}

	k_spin_unlock(&process_lock, key);

	return 0;
}

int process_unregister_thread(struct z_process *proc, struct k_thread *thread)
{
	if (!proc || !thread) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	/* Remove from thread list */
	sys_dlist_remove(&thread->process_thread_node);

	/* Clear process pointer */
	thread->process = NULL;

	/* If this was the main thread, clear it */
	if (proc->main_thread == thread) {
		proc->main_thread = NULL;
	}

	k_spin_unlock(&process_lock, key);

	return 0;
}

/**
 * @brief vfork implementation
 *
 * Creates child process that shares address space with parent.
 * Parent blocks until child calls exec or exit.
 * Based on Embox's vfork resource module.
 */
pid_t process_vfork(void)
{
	struct z_process *parent = process_current();
	if (!parent) {
		return -ESRCH;
	}

	/* Create child process (shares resources with parent) */
	struct z_process *child = process_create(parent);
	if (!child) {
		return -ENOMEM;
	}

	/* Mark parent as in vfork state */
	k_spinlock_key_t key = k_spin_lock(&process_lock);
	parent->flags |= PROCESS_FLAG_IN_VFORK;
	k_spin_unlock(&process_lock, key);

	/* Child shares parent's FD table and environment (shallow copy for vfork) */
	/* Note: In real vfork, child uses parent's stack too, but that's architecture-specific */
	memcpy(&child->fd_table, &parent->fd_table, sizeof(struct idesc_table));
	memcpy(&child->env, &parent->env, sizeof(struct task_env));

	/* Parent blocks here until child calls exec or exit */
	/* In child context, return 0 immediately */
	/* In parent context, block on semaphore */

	/* This is simplified - real implementation needs architecture support */
	/* For now, we just mark the state and return child PID */

	return child->pid;
}

/**
 * @brief Wake parent after vfork child exits or execs
 */
void process_vfork_wake_parent(struct z_process *child)
{
	if (!child || !child->parent) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&process_lock);

	if (child->parent->flags & PROCESS_FLAG_IN_VFORK) {
		/* Clear vfork flag */
		child->parent->flags &= ~PROCESS_FLAG_IN_VFORK;

		/* Wake parent */
		k_sem_give(&child->parent->vfork_sem);
	}

	k_spin_unlock(&process_lock, key);
}

/* Initialize process subsystem at kernel init */
SYS_INIT(z_process_init_wrapper, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
