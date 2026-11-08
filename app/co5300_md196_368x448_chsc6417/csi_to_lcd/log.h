/*
  log.h - tiny shared logging module for the st7365_md350_320x480 apps.

  syslog-style levels; destinations: stdio (default), optional size-
  rotated file, optional syslog. Configured from a flat JSON
  `config.json` sitting next to the executable (log_config_from_argv).

  A subset of every app's printf() calls was converted to log_printf();
  per-frame/per-second status chatter is LOG_LVL_DEBUG and is hidden
  at the default `info` level.
*/
#ifndef LOG_H
#define LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3,
} log_level_t;

/* defaults: level = info, destination = stdio */
int  log_init(const char *app_name);

/* read <dir-of-argv0>/config.json if present (silent no-op otherwise);
 * keys: log_level ("debug"|"info"|"warning"|"error"),
 *       log_destinations (["stdio"] | may add "file", "syslog"),
 *       log_file, log_file_max_bytes, log_file_keep */
int  log_config_from_argv(int argc, char **argv);

void log_set_level(log_level_t lvl);
void log_add_file(const char *path, size_t max_bytes, int keep);
void log_add_syslog(void);
log_level_t log_level_from_str(const char *s);

/* newline is appended; stdio keeps messages bare, file/syslog lines
 * get "<ts> [<LEVEL>] " prefix */
void log_printf(log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* LOG_H */
