#pragma once

#include <stdint.h>
#include <stdbool.h>

// ── Auris Log Daemon ──
//
// Central logging facility for the entire Auris firmware.
// Starts as early as possible after storage mount.
// Queue-based: any task on any core can log without blocking on I/O.
//
// Log file:   /sdcard/logs/auris.log
// Archives:   /sdcard/logs/auris_NNNN.log.gz
// Rotation:   when current log exceeds AURIS_LOG_MAX_SIZE
// Retention:  last AURIS_LOG_MAX_ARCHIVES compressed archives kept

#define AURIS_LOG_MAX_SIZE      (32 * 1024)   // 32 KB per log file
#define AURIS_LOG_MAX_ARCHIVES  8

void auris_log_init(void);
void auris_log_write(const char *module, char level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ALOG_I(mod, fmt, ...) auris_log_write(mod, 'I', fmt, ##__VA_ARGS__)
#define ALOG_W(mod, fmt, ...) auris_log_write(mod, 'W', fmt, ##__VA_ARGS__)
#define ALOG_E(mod, fmt, ...) auris_log_write(mod, 'E', fmt, ##__VA_ARGS__)
