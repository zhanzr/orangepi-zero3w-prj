/*
  touch.c - CHSC6417 capacitive touch over TWI0 (/dev/i2c-0) for the
  Orange Pi Zero 3W CO5300 module port.

  ACCESS RULE (verified on this firmware): register pointer writes must
  be 16-byte-aligned block bases (0x00/0x10/0x20/.../0xE0) and block
  reads work up to 32 bytes; a pointer like 0x23 or a 36-byte block
  read is rejected with EINVAL by the controller/driver. All reads here
  are 16-byte aligned blocks.

  Touch state is the point count in bits [2:0] of byte 0 at register
  0x00 (0 = released, 1..5 = contacts), matching both STM32 reference
  ports. main.c adds hysteresis on top.

  Coordinates: byte0 carries X[8]/Y[8] in bits 6/7 (vendor decode:
  X = ((b0 & 0x40) >> 6) << 8 | b1, Y = ((b0 & 0x80) >> 7) << 8 | b2).

  EVENT MODEL + AUTO-RELEASE (verified with a raw probe): the firmware
  writes the report buffer only on touch events; the content persists
  indefinitely between events and across app restarts. This file
  therefore treats report CHANGES as events: the first read after init
  is a baseline (never a contact), a changed report enters/leaves the
  contact state, and a contact whose report stops changing for
  TOUCH_STALE_MS (2 s) is auto-released - it is indistinguishable from
  a missed release report. CONSEQUENCE: the long-hold gesture (> 2 s)
  is not supported today; future re-design options (INT pin irq,
  kernel input driver, report sequence field) are described in
  docs/csi-ov5647-notes.md ("Touch auto-release design").
*/
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <unistd.h>

#include "touch.h"

#include "log.h"

static int s_fd = -1;
static uint8_t s_blk0[4];    /* last report (regs 0x00..0x03) */
static int      s_have;      /* baseline report captured */
static int      s_down;      /* current contact state */
static long long s_down_ms;  /* when the current contact began */
static long long s_last_ms;  /* last bus access */
static long long last_poll_ms;
static long long backoff_until_ms;

/* Firmware semantics (verified with a raw probe): the IC writes its
 * report buffer ONLY on touch events - the content persists between
 * events and even across app restarts, so a stale count=1 report
 * from a previous session must never register as a contact. Report
 * CHANGES are the signal; a held contact whose report stops changing
 * for TOUCH_STALE_MS missed its release report -> auto release.
 * Failures pace/back off without touching the state. */
#define TOUCH_MIN_INTERVAL_MS  100
#define TOUCH_FAIL_BACKOFF_MS  250
#define TOUCH_STALE_MS         2000

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* write the register pointer, then read the first `len` bytes
 * (combined transfer, pointer = 16-byte-aligned block base as the
 * firmware requires). Short transfers matter: ~10% of reads land in
 * a moment where the IC's scan cycle preempts its I2C slave mid-byte
 * (observed as arbitration-lost + stuck SDA), and the on-wire time of
 * a 16-byte block is 4x the exposure of the 4 status bytes we need. */
static int blk_read(uint8_t base, uint8_t *buf, uint8_t len)
{
    struct i2c_rdwr_ioctl_data msgset;
    struct i2c_msg msgs[2];

    msgs[0].addr  = TOUCH_ADDR_7BIT;
    msgs[0].flags = 0;                  /* write: register pointer */
    msgs[0].len   = 1;
    msgs[0].buf   = &base;
    msgs[1].addr  = TOUCH_ADDR_7BIT;
    msgs[1].flags = I2C_M_RD;           /* read: status block */
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    msgset.msgs  = msgs;
    msgset.nmsgs = 2;
    /* single attempt: the IC NACKs while its scan cycle runs; retries
     * within the same busy window only add kernel-log spam - the
     * caller's pacing and backoff handle recovery instead */
    {
        int transferred = ioctl(s_fd, I2C_RDWR, &msgset);

        /* I2C_RDWR returns the number of messages transferred, not zero. */
        if (transferred == (int)msgset.nmsgs)
            return 0;
        if (transferred >= 0)
            errno = EIO;
    }
    return -1;
}

int Touch_Init(void)
{
    s_fd = open(TOUCH_I2C_DEV, O_RDWR);
    if (s_fd < 0)
    {
        log_printf(LOG_LVL_ERROR, "open " TOUCH_I2C_DEV ": %s",
                   strerror(errno));
        return -1;
    }
    s_have = 0;
    s_down = 0;
    last_poll_ms = 0;
    backoff_until_ms = 0;
    return 0;
}

/* Event-driven sample: returns 1 while a contact is down, 0 while
 * idle, -1 when this call produced no information (pacing, bus
 * failure, torn read). Callers treat -1 as "keep previous state". */
int Touch_Sample(void)
{
    uint8_t st[4];
    long long t = now_ms();

    /* pacing + post-failure quiet period: touch the bus rarely */
    if (t < backoff_until_ms)
        return -1;
    if (s_have && t - s_last_ms < TOUCH_MIN_INTERVAL_MS)
        return -1;
    s_last_ms = t;

    if (blk_read(0x00, st, sizeof st) != 0)
    {
        backoff_until_ms = t + TOUCH_FAIL_BACKOFF_MS;
        return -1;
    }

    if (!s_have)
    {
        /* baseline: whatever is in the buffer right now may be a
         * stale report from a previous session - NOT a contact */
        memcpy(s_blk0, st, sizeof st);
        s_have = 1;
        s_down = 0;
        return 0;
    }

    if (memcmp(st, s_blk0, sizeof st) == 0)
    {
        /* no new event; a contact whose report stopped changing is
         * stale (its release report was missed) - release it */
        if (s_down && t - s_down_ms > TOUCH_STALE_MS)
            s_down = 0;
        return s_down ? 1 : 0;
    }

    /* new event: confirm with a second read - the IC updates its
     * buffer non-atomically, a torn read would mix old/new bytes */
    {
        uint8_t st2[4];

        if (blk_read(0x00, st2, sizeof st2) != 0)
            return -1;   /* keep previous state, resample later */
        if (memcmp(st, st2, sizeof st) != 0)
            return -1;   /* still mid-update */
    }

    memcpy(s_blk0, st, sizeof st);
    {
        uint8_t count = st[0] & 0x07U;

        if (count >= 1U && count <= 5U)
        {
            s_down = 1;
            s_down_ms = t;
            return 1;
        }
        s_down = 0;   /* release event */
        return 0;
    }
}

uint8_t Touch_SelfTest(void)
{
    /* the first read after open can NACK while the IC's scan cycle is
     * busy - retry hard before declaring the bus dead */
    for (int i = 0; i < 100; i++)
    {
        if (blk_read(0x00, s_blk0, 4) == 0)
            return 1;
        usleep(20000);
    }
    return 0;
}

int Touch_X(void)
{
    return (int)(((s_blk0[0] & 0x40) >> 6) << 8 | s_blk0[1]);
}

int Touch_Y(void)
{
    return (int)(((s_blk0[0] & 0x80) >> 7) << 8 | s_blk0[2]);
}
