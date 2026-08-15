/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell command to run Phase 1 tests
 */

#include <zephyr/shell/shell.h>

/* External test function */
extern int test_phase1_main(void);

static int cmd_test_phase1(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Starting Phase 1 tests...");

	int result = test_phase1_main();

	if (result == 0) {
		shell_print(sh, "All tests passed!");
	} else {
		shell_error(sh, "Some tests failed!");
	}

	return result;
}

SHELL_CMD_REGISTER(test_phase1, NULL,
                   "Run Phase 1 FD reference counting tests",
                   cmd_test_phase1);
