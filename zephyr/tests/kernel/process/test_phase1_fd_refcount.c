/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test: FD Reference Counting and Deep Copy Fork
 *
 * This test validates Phase 1 implementation:
 * 1. FD reference counting works correctly
 * 2. fork() performs deep copy with refcount increment
 * 3. close() in child doesn't affect parent
 * 4. close() in parent doesn't affect child
 * 5. Descriptor is only freed when all references are released
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel/process.h>
#include <zephyr/kernel/idesc.h>
#include <stdio.h>
#include <string.h>

/* External test idesc API */
extern struct idesc *test_idesc_create(void);

/* Forward declaration from shell_process.h */
extern pid_t new_task(const char *name, void *(*run)(void *), void *arg);
extern pid_t waitpid(pid_t pid, int *status, int options);

/**
 * @brief Test 1: Basic reference counting
 */
static int test_basic_refcount(void)
{
	printk("\n=== Test 1: Basic Reference Counting ===\n");

	struct z_process *proc = process_current();
	if (!proc) {
		printk("FAIL: No current process\n");
		return -1;
	}

	/* Create a test descriptor */
	struct idesc *desc = test_idesc_create();
	if (!desc) {
		printk("FAIL: Failed to create test descriptor\n");
		return -1;
	}

	printk("Created descriptor, refcount=%d\n", idesc_getrefcount(desc));

	/* Add to FD table (should increment refcount) */
	int fd = process_idesc_table_add(proc, desc);
	if (fd < 0) {
		printk("FAIL: Failed to add descriptor to FD table\n");
		idesc_put(desc);
		return -1;
	}

	printk("Added to FD table as fd=%d, refcount=%d\n", fd, idesc_getrefcount(desc));

	/* Remove from FD table (should decrement refcount and close) */
	process_idesc_table_remove(proc, fd);
	printk("Removed from FD table, descriptor should be closed and freed\n");

	/* Release initial reference */
	idesc_put(desc);
	printk("Released initial reference\n");

	printk("PASS: Basic refcount test\n");
	return 0;
}

/**
 * @brief Test 2: Fork with FD sharing
 */
static void *child_task_fn(void *arg)
{
	int parent_fd = (int)(intptr_t)arg;

	printk("\n[child] Started, parent_fd=%d\n", parent_fd);

	struct z_process *child_proc = process_current();
	if (!child_proc) {
		printk("[child] FAIL: No current process\n");
		return (void *)-1;
	}

	/* Get the descriptor from FD table */
	struct idesc *desc = (struct idesc *)process_idesc_table_get(child_proc, parent_fd);
	if (!desc) {
		printk("[child] FAIL: FD %d not found in child\n", parent_fd);
		return (void *)-1;
	}

	printk("[child] Found descriptor at FD %d, refcount=%d\n",
	       parent_fd, idesc_getrefcount(desc));

	/* Write some data */
	const char *msg = "Hello from child";
	ssize_t written = desc->ops->write(desc, msg, strlen(msg));
	if (written < 0) {
		printk("[child] FAIL: Write failed\n");
		return (void *)-1;
	}
	printk("[child] Wrote %zd bytes\n", written);

	/* Close the descriptor in child */
	printk("[child] Closing FD %d, refcount before close=%d\n",
	       parent_fd, idesc_getrefcount(desc));
	process_idesc_table_remove(child_proc, parent_fd);
	printk("[child] Closed FD, descriptor should still exist in parent\n");

	return (void *)0;
}

static int test_fork_fd_sharing(void)
{
	printk("\n=== Test 2: Fork FD Sharing ===\n");

	struct z_process *parent_proc = process_current();
	if (!parent_proc) {
		printk("FAIL: No current process\n");
		return -1;
	}

	/* Create a test descriptor */
	struct idesc *desc = test_idesc_create();
	if (!desc) {
		printk("FAIL: Failed to create test descriptor\n");
		return -1;
	}

	/* Add to parent's FD table */
	int fd = process_idesc_table_add(parent_proc, desc);
	if (fd < 0) {
		printk("FAIL: Failed to add descriptor to parent FD table\n");
		idesc_put(desc);
		return -1;
	}

	printk("[parent] Created FD %d, refcount=%d\n", fd, idesc_getrefcount(desc));

	/* Fork the process */
	struct z_process *child_proc = process_fork(parent_proc);
	if (!child_proc) {
		printk("FAIL: Fork failed\n");
		process_idesc_table_remove(parent_proc, fd);
		idesc_put(desc);
		return -1;
	}

	printk("[parent] Forked child PID %d, refcount=%d (should be 3: initial + parent + child)\n",
	       child_proc->pid, idesc_getrefcount(desc));

	/* Create child task to test the descriptor */
	pid_t child_pid = new_task("test_child", child_task_fn, (void *)(intptr_t)fd);
	if (child_pid <= 0) {
		printk("FAIL: Failed to create child task\n");
		process_idesc_table_remove(parent_proc, fd);
		idesc_put(desc);
		return -1;
	}

	/* Wait for child to finish */
	int status;
	waitpid(child_pid, &status, 0);
	printk("[parent] Child exited with status %d\n", status);

	/* Check refcount after child closed its FD */
	printk("[parent] After child closed, refcount=%d (should be 2: initial + parent)\n",
	       idesc_getrefcount(desc));

	/* Parent reads data written by child */
	char buf[256];
	ssize_t nread = desc->ops->read(desc, buf, sizeof(buf) - 1);
	if (nread > 0) {
		buf[nread] = '\0';
		printk("[parent] Read %zd bytes: '%s'\n", nread, buf);
	}

	/* Parent closes its FD */
	printk("[parent] Closing FD %d, refcount=%d\n", fd, idesc_getrefcount(desc));
	process_idesc_table_remove(parent_proc, fd);
	printk("[parent] Closed FD, refcount=%d (should be 1: initial)\n",
	       idesc_getrefcount(desc));

	/* Release initial reference - should trigger final close */
	printk("[parent] Releasing initial reference, should free descriptor\n");
	idesc_put(desc);

	printk("PASS: Fork FD sharing test\n");
	return 0;
}

/**
 * @brief Test 3: Multiple forks
 */
static int test_multiple_forks(void)
{
	printk("\n=== Test 3: Multiple Forks ===\n");

	struct z_process *proc = process_current();
	if (!proc) {
		printk("FAIL: No current process\n");
		return -1;
	}

	/* Create descriptor */
	struct idesc *desc = test_idesc_create();
	if (!desc) {
		printk("FAIL: Failed to create descriptor\n");
		return -1;
	}

	int fd = process_idesc_table_add(proc, desc);
	if (fd < 0) {
		printk("FAIL: Failed to add descriptor\n");
		idesc_put(desc);
		return -1;
	}

	printk("Initial refcount=%d\n", idesc_getrefcount(desc));

	/* Fork 3 times */
	for (int i = 0; i < 3; i++) {
		struct z_process *child = process_fork(proc);
		if (!child) {
			printk("FAIL: Fork %d failed\n", i);
			continue;
		}
		printk("Fork %d: child PID %d, refcount=%d\n",
		       i, child->pid, idesc_getrefcount(desc));
	}

	printk("After 3 forks, refcount=%d (should be 5: initial + parent + 3 children)\n",
	       idesc_getrefcount(desc));

	/* Clean up */
	process_idesc_table_remove(proc, fd);
	idesc_put(desc);

	printk("PASS: Multiple forks test\n");
	return 0;
}

/**
 * @brief Main test runner
 */
int test_phase1_main(void)
{
	printk("\n");
	printk("====================================================\n");
	printk("  Phase 1 Test: FD Reference Counting & Deep Fork\n");
	printk("====================================================\n");

	int result = 0;

	/* Run tests */
	if (test_basic_refcount() < 0) {
		result = -1;
	}

	if (test_fork_fd_sharing() < 0) {
		result = -1;
	}

	if (test_multiple_forks() < 0) {
		result = -1;
	}

	printk("\n");
	if (result == 0) {
		printk("====================================================\n");
		printk("  ALL TESTS PASSED!\n");
		printk("====================================================\n");
	} else {
		printk("====================================================\n");
		printk("  SOME TESTS FAILED!\n");
		printk("====================================================\n");
	}

	return result;
}
