/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test I/O Descriptor - Simple implementation for testing idesc framework
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel/idesc.h>
#include <zephyr/kernel/process.h>
#include <string.h>
#include <errno.h>

/**
 * @brief Test descriptor - simple in-memory buffer
 */
struct test_idesc {
	struct idesc base;           /* Base idesc structure (must be first) */
	char buffer[256];            /* Internal buffer */
	size_t read_pos;             /* Read position */
	size_t write_pos;            /* Write position */
	bool closed;                 /* Closed flag */
};

/* Forward declarations */
static int test_idesc_close(struct idesc *desc);
static ssize_t test_idesc_read(struct idesc *desc, void *buf, size_t len);
static ssize_t test_idesc_write(struct idesc *desc, const void *buf, size_t len);
static int test_idesc_ioctl(struct idesc *desc, int request, void *arg);

/* Operations table */
static const struct idesc_ops test_idesc_ops = {
	.close = test_idesc_close,
	.read = test_idesc_read,
	.write = test_idesc_write,
	.ioctl = test_idesc_ioctl,
	.fstat = NULL,
};

/**
 * @brief Create a test descriptor
 *
 * @return Pointer to idesc or NULL on failure
 */
struct idesc *test_idesc_create(void)
{
	struct test_idesc *test_desc = k_malloc(sizeof(struct test_idesc));
	if (!test_desc) {
		return NULL;
	}

	/* Initialize base idesc with refcount = 1 */
	idesc_init(&test_desc->base, &test_idesc_ops, 0);

	/* Initialize test-specific fields */
	memset(test_desc->buffer, 0, sizeof(test_desc->buffer));
	test_desc->read_pos = 0;
	test_desc->write_pos = 0;
	test_desc->closed = false;

	printk("test_idesc: created at %p, refcount=%d\n",
	       test_desc, idesc_getrefcount(&test_desc->base));

	return &test_desc->base;
}

/**
 * @brief Close the test descriptor
 */
static int test_idesc_close(struct idesc *desc)
{
	if (!desc) {
		return -EINVAL;
	}

	struct test_idesc *test_desc = CONTAINER_OF(desc, struct test_idesc, base);

	printk("test_idesc: close called at %p, refcount=%d\n",
	       test_desc, idesc_getrefcount(desc));

	if (test_desc->closed) {
		printk("test_idesc: already closed!\n");
		return -EBADF;
	}

	test_desc->closed = true;

	/* Free the descriptor */
	printk("test_idesc: freeing memory at %p\n", test_desc);
	k_free(test_desc);

	return 0;
}

/**
 * @brief Read from test descriptor
 */
static ssize_t test_idesc_read(struct idesc *desc, void *buf, size_t len)
{
	if (!desc || !buf) {
		return -EINVAL;
	}

	struct test_idesc *test_desc = CONTAINER_OF(desc, struct test_idesc, base);

	if (test_desc->closed) {
		return -EBADF;
	}

	/* Calculate available data */
	size_t available = test_desc->write_pos - test_desc->read_pos;
	if (available == 0) {
		return 0;  /* EOF */
	}

	/* Read up to len bytes */
	size_t to_read = (len < available) ? len : available;
	memcpy(buf, test_desc->buffer + test_desc->read_pos, to_read);
	test_desc->read_pos += to_read;

	printk("test_idesc: read %zu bytes from %p\n", to_read, test_desc);

	return to_read;
}

/**
 * @brief Write to test descriptor
 */
static ssize_t test_idesc_write(struct idesc *desc, const void *buf, size_t len)
{
	if (!desc || !buf) {
		return -EINVAL;
	}

	struct test_idesc *test_desc = CONTAINER_OF(desc, struct test_idesc, base);

	if (test_desc->closed) {
		return -EBADF;
	}

	/* Calculate available space */
	size_t available = sizeof(test_desc->buffer) - test_desc->write_pos;
	if (available == 0) {
		return -ENOSPC;  /* Buffer full */
	}

	/* Write up to len bytes */
	size_t to_write = (len < available) ? len : available;
	memcpy(test_desc->buffer + test_desc->write_pos, buf, to_write);
	test_desc->write_pos += to_write;

	printk("test_idesc: wrote %zu bytes to %p\n", to_write, test_desc);

	return to_write;
}

/**
 * @brief I/O control (not implemented)
 */
static int test_idesc_ioctl(struct idesc *desc, int request, void *arg)
{
	return -ENOSYS;
}
