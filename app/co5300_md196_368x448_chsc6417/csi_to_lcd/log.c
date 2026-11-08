/*
  log.c - tiny shared logging module (see log.h).

  destinations: stdio (bare lines, like the old printf), file (timestamp
  + level prefix, size-rotated), syslog (standard library LOG_*).
  config: flat JSON config.json next to the executable, parsed here with
  a minimal purpose-built reader (strings, numbers, array of strings).
*/
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

#define LOG_MAX_DST 4

typedef struct {
    int used;
    int kind;               /* 0 stdio, 1 file, 2 syslog */
    FILE *fp;
    char path[256];
    size_t max_bytes;
    int keep;
} log_dst_t;

static log_dst_t s_dst[LOG_MAX_DST];
static log_level_t s_level = LOG_LVL_INFO;
static char s_app[64];

static const char *level_name(log_level_t lvl)
{
    switch (lvl)
    {
    case LOG_LVL_DEBUG: return "DEBUG";
    case LOG_LVL_INFO:  return "INFO";
    case LOG_LVL_WARN:  return "WARNING";
    default:        return "ERROR";
    }
}

log_level_t log_level_from_str(const char *s)
{
    if (!s)
        return LOG_LVL_INFO;
    if (!strcmp(s, "debug"))
        return LOG_LVL_DEBUG;
    if (!strcmp(s, "warning") || !strcmp(s, "warn"))
        return LOG_LVL_WARN;
    if (!strcmp(s, "error") || !strcmp(s, "err"))
        return LOG_LVL_ERROR;
    return LOG_LVL_INFO;
}

/* ---------- minimal flat-JSON reader ---------- */

/* return pointer just past the value of "key", or NULL; *len = value len */
static const char *json_value(const char *js, const char *key, size_t *len)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(js, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r')
        p++;
    if (!*p)
        return NULL;
    if (*p == '"')
    {
        p++;
        *len = 0;
        while (p[*len] && p[*len] != '"')
            (*len)++;
        return p;
    }
    if (*p == '[')
    {
        /* array: span up to the matching close bracket */
        const char *e = strchr(p, ']');
        *len = e ? (size_t)(e - p + 1) : strlen(p);
        return p;
    }
    *len = strspn(p, "-0123456789.eE");
    return *len ? p : NULL;
}

static int json_string(const char *js, const char *key, char *out, size_t n)
{
    size_t len;
    const char *v = json_value(js, key, &len);

    if (!v)
        return -1;
    if (len >= n)
        len = n - 1;
    memcpy(out, v, len);
    out[len] = 0;
    return 0;
}

static int json_int(const char *js, const char *key, long *out)
{
    size_t len;
    const char *v = json_value(js, key, &len);

    if (!v)
        return -1;
    *out = strtol(v, NULL, 10);
    return 0;
}

/* count entries of a string array value: ["a","b",...] */
static int json_array_count(const char *js, const char *key)
{
    size_t len;
    const char *v = json_value(js, key, &len);
    int cnt = 0;

    if (!v || *v != '[')
        return -1;
    v++;
    while (*v && *v != ']')
    {
        if (*v == '"')
        {
            cnt++;
            v++;
            while (*v && *v != '"')
                v++;
        }
        v++;
    }
    return cnt;
}

static int json_array_string(const char *js, const char *key, int idx,
                             char *out, size_t n)
{
    size_t len;
    const char *v = json_value(js, key, &len);
    int cur = 0;

    if (!v || *v != '[')
        return -1;
    v++;
    while (*v && *v != ']')
    {
        if (*v == '"')
        {
            v++;
            len = 0;
            while (*v && *v != '"')
            {
                v++;
                len++;
            }
            if (cur == idx)
            {
                if (len >= n)
                    len = n - 1;
                memcpy(out, v - len, len);
                out[len] = 0;
                return 0;
            }
            cur++;
        }
        v++;
    }
    return -1;
}

/* ---------- destinations ---------- */

int log_init(const char *app_name)
{
    snprintf(s_app, sizeof(s_app), "%s", app_name ? app_name : "app");
    s_dst[0].used = 1;
    s_dst[0].kind = 0;      /* stdio */
    return 0;
}

void log_set_level(log_level_t lvl)
{
    s_level = lvl;
}

static void file_rotate(void)
{
    char old[512], newp[512];
    int i;
    log_dst_t *d = &s_dst[1];

    fclose(d->fp);
    for (i = d->keep - 1; i >= 1; i--)
    {
        snprintf(old, sizeof(old), "%s.%d", d->path, i);
        snprintf(newp, sizeof(newp), "%s.%d", d->path, i + 1);
        rename(old, newp);
    }
    snprintf(newp, sizeof(newp), "%s.1", d->path);
    rename(d->path, newp);
    d->fp = fopen(d->path, "a");
}

void log_add_file(const char *path, size_t max_bytes, int keep)
{
    int i;
    log_dst_t *d = NULL;

    for (i = 1; i < LOG_MAX_DST; i++)
        if (!s_dst[i].used)
        {
            d = &s_dst[i];
            break;
        }
    if (!d)
        return;
    d->used = 1;
    d->kind = 1;
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->max_bytes = max_bytes ? max_bytes : 1024 * 1024;
    d->keep = keep ? keep : 3;
    d->fp = fopen(path, "a");
}

void log_add_syslog(void)
{
    int i;

    for (i = 1; i < LOG_MAX_DST; i++)
        if (!s_dst[i].used)
        {
            s_dst[i].used = 1;
            s_dst[i].kind = 2;
            openlog(s_app, LOG_PID | LOG_NDELAY, LOG_USER);
            return;
        }
}

void log_printf(log_level_t lvl, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    int i;

    if (lvl < s_level)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    for (i = 0; i < LOG_MAX_DST; i++)
    {
        log_dst_t *d = &s_dst[i];

        if (!d->used)
            continue;
        if (d->kind == 0)
        {
            /* stdio: bare line for info/debug, prefixed for warn/error */
            if (lvl >= LOG_LVL_WARN)
                fprintf(stderr, "%s: %s\n", level_name(lvl), msg);
            else
                fprintf(stdout, "%s\n", msg);
        }
        else if (d->kind == 1 && d->fp)
        {
            char ts[32];
            time_t t = time(NULL);
            struct tm tm;

            localtime_r(&t, &tm);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
            fprintf(d->fp, "%s [%-7s] %s\n", ts, level_name(lvl), msg);
            fflush(d->fp);
            if (d->max_bytes &&
                (size_t)ftell(d->fp) >= d->max_bytes)
                file_rotate();
        }
        else if (d->kind == 2)
        {
            static const int prio[] = {
                LOG_LVL_DEBUG, LOG_LVL_INFO, LOG_WARNING, LOG_ERR
            };
            syslog(prio[lvl], "%s", msg);
        }
    }
}

/* ---------- config.json ---------- */

int log_config_from_argv(int argc, char **argv)
{
    char exe[512], dir[512], cfg[512], val[256];
    FILE *fp;
    long v;
    char *js;
    size_t n;
    int i, ndst = -1, want_file = 0, want_syslog = 0;
    char path[512] = "";
    long max_bytes = 0, keep = 0;

    (void)argc;
    if (!argv || !argv[0])
        return -1;
    snprintf(exe, sizeof(exe), "%s", argv[0]);
    snprintf(dir, sizeof(dir), "%s", dirname(exe));
    snprintf(cfg, sizeof(cfg), "%s/config.json", dir);

    fp = fopen(cfg, "r");
    if (!fp)
        return -1;          /* no config: defaults (info, stdio) */
    fseek(fp, 0, SEEK_END);
    n = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n > 16384)
        n = 16384;
    js = malloc(n + 1);
    if (!js)
    {
        fclose(fp);
        return -1;
    }
    n = fread(js, 1, n, fp);
    js[n] = 0;
    fclose(fp);

    if (json_string(js, "log_level", val, sizeof(val)) == 0)
        s_level = log_level_from_str(val);

    ndst = json_array_count(js, "log_destinations");
    if (ndst > 0)
    {
        /* first destination: stdio keeps slot 0; file/syslog take slots */
        for (i = 0; i < ndst; i++)
        {
            if (json_array_string(js, "log_destinations", i,
                                  val, sizeof(val)) != 0)
                continue;
            if (!strcmp(val, "file"))
                want_file = 1;
            else if (!strcmp(val, "syslog"))
                want_syslog = 1;
            else if (!strcmp(val, "stdio"))
                s_dst[0].used = 1;
        }
    }

    if (json_string(js, "log_file", val, sizeof(val)) == 0)
        snprintf(path, sizeof(path), "%s", val);
    else
        snprintf(path, sizeof(path), "%s/%s.log", dir, s_app);
    if (json_int(js, "log_file_max_bytes", &v) == 0)
        max_bytes = v;
    if (json_int(js, "log_file_keep", &v) == 0)
        keep = (int)v;

    if (want_file)
        log_add_file(path, (size_t)max_bytes, (int)keep);
    if (want_syslog)
        log_add_syslog();

    free(js);
    return 0;
}
