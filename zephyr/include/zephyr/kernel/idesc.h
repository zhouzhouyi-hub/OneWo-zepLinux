/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I/O Descriptor (idesc) - Reference Counted File Descriptor Base Class
 * Adapted from Embox's idesc infrastructure
 */

#ifndef ZEPHYR_INCLUDE_KERNEL_IDESC_H_
#define ZEPHYR_INCLUDE_KERNEL_IDESC_H_

#include <zephyr/sys/atomic.h>
#include <zephyr/kernel.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct idesc;
struct idesc_ops;

/**
 * @brief I/O Descriptor operations (virtual function table)
 *
 * Similar to Embox's idesc_ops, provides polymorphic file operations.
 */
struct idesc_ops {
	/**
	 * @brief Close the descriptor
	 *
	 * Called when reference count reaches zero.
	 * Should release underlying resources.
	 *
	 * @param desc Descriptor to close
	 * @return 0 on success, negative errno on failure
	 */
	int (*close)(struct idesc *desc);

	/**
	 * @brief Read from descriptor
	 *
	 * @param desc Descriptor to read from
	 * @param buf Buffer to store data
	 * @param len Maximum bytes to read
	 * @return Number of bytes read, or negative errno
	 */
	ssize_t (*read)(struct idesc *desc, void *buf, size_t len);

	/**
	 * @brief Write to descriptor
	 *
	 * @param desc Descriptor to write to
	 * @param buf Data to write
	 * @param len Number of bytes to write
	 * @return Number of bytes written, or negative errno
	 */
	ssize_t (*write)(struct idesc *desc, const void *buf, size_t len);

	/**
	 * @brief I/O control operation
	 *
	 * @param desc Descriptor
	 * @param request Control request code
	 * @param arg Request-specific argument
	 * @return 0 on success, negative errno on failure
	 */
	int (*ioctl)(struct idesc *desc, int request, void *arg);

	/**
	 * @brief Get status/flags
	 *
	 * @param desc Descriptor
	 * @return Status flags
	 */
	int (*fstat)(struct idesc *desc, void *stat_buf);
};

/**
 * @brief I/O Descriptor base structure
 *
 * All file-like objects should embed this structure.
 * Provides reference counting and polymorphic operations.
 */
struct idesc {
	const struct idesc_ops *ops;  /**< Operations table */
	atomic_t refcount;            /**< Reference count */
	uint32_t flags;               /**< Descriptor flags */
	void *priv;                   /**< Private data pointer */
};

/**
 * @brief Initialize an idesc structure
 *
 * Must be called before first use. Sets refcount to 1.
 *
 * @param desc Descriptor to initialize
 * @param ops Operations table
 * @param flags Initial flags
 */
static inline void idesc_init(struct idesc *desc, const struct idesc_ops *ops,
			       uint32_t flags)
{
	if (desc) {
		desc->ops = ops;
		atomic_set(&desc->refcount, 1);
		desc->flags = flags;
		desc->priv = NULL;
	}
}

/**
 * @brief Increment reference count
 *
 * Call when sharing a descriptor between processes or threads.
 *
 * @param desc Descriptor to reference
 * @return The same descriptor pointer (for convenience)
 */
static inline struct idesc *idesc_get(struct idesc *desc)
{
	if (desc) {
		atomic_inc(&desc->refcount);
	}
	return desc;
}

/**
 * @brief Decrement reference count
 *
 * When count reaches zero, calls close() operation and may free the descriptor.
 *
 * @param desc Descriptor to dereference
 */
static inline void idesc_put(struct idesc *desc)
{
	if (!desc) {
		return;
	}

	/* atomic_dec returns the OLD value before decrement */
	if (atomic_dec(&desc->refcount) == 1) {
		/* Reference count reached zero, close the descriptor */
		if (desc->ops && desc->ops->close) {
			desc->ops->close(desc);
		}
		/* Note: close() is responsible for freeing desc if needed */
	}
}

/**
 * @brief Get current reference count (for debugging)
 *
 * @param desc Descriptor
 * @return Current reference count
 */
static inline int idesc_getrefcount(struct idesc *desc)
{
	if (!desc) {
		return 0;
	}
	return atomic_get(&desc->refcount);
}

/**
 * @brief Check if descriptor is valid
 *
 * @param desc Descriptor to check
 * @return true if valid, false otherwise
 */
static inline bool idesc_is_valid(struct idesc *desc)
{
	return desc && desc->ops && atomic_get(&desc->refcount) > 0;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_KERNEL_IDESC_H_ */
