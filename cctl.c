/*
 * cctl - Lightweight Clevo P15 performance profile & fan controller
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 bhusann
 *
 * Pure C. Direct EC port I/O for fan control, sysfs writes for CPU power
 * management. The visual GPU monitor uses POSIX threads and libm.
 *
 * Build:  make
 * Usage:  sudo ./cctl set <profile>
 *         sudo ./cctl fan <mode> [value]
 *         sudo ./cctl status
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/io.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sched.h>
#include <sys/wait.h>
#include <limits.h>
#include <pwd.h>
#include <sys/types.h>
#include <signal.h>
#include <termios.h>
#include <sys/file.h>
#include <stdarg.h>

#define CCTL_VERSION      "4.6.3"
/* NOTE FOR DEVELOPERS / AI AGENTS:
 * Always increment CCTL_MICROVERSION (a 6-digit integer) whenever making code
 * changes and committing. 'cctl install' checks this hidden value to determine
 * if a local binary is newer than /usr/local/bin/cctl. Do NOT document this in
 * README or help menus. */
#ifndef CCTL_MICROVERSION
#define CCTL_MICROVERSION 100073
#endif


/* Subsystems stay in one translation unit to share the private hardware helpers. */
#include "cctl_core.inc"
#include "cctl_controls.inc"
#include "cctl_platform.inc"
#include "cctl_keyboard.inc"
#include "cctl_telemetry.inc"
#include "cctl_runtime.inc"
#include "cctl_nvidia.inc"
#include "cctl_snapshot.inc"
#include "cctl_commands.inc"
#include "cctl_drivers_cli.inc"
