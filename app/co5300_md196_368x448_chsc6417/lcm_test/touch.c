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
*/
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <unistd.h>

#include "touch.h"

#include "log.h"

static int s_fd = -1;
static uint8_t s_blk0[16];   /* regs 0x00..0x0F: slot 0 (flags + coords) */

/* write the register pointer, then read 16 bytes (combined transfer,
 * pointer = 16-byte-aligned block base as the firmware requires).
 * The touch IC NACKs reads while its internal scan cycle is busy -
 * EINVAL from the adapter - so retry a few times. */
static int blk_read(uint8_t base, uint8_t *buf)
{
    struct i2c_rdwr_ioctl_data msgset;
    struct i2c_msg msgs[2];

    msgs[0].addr  = TOUCH_ADDR_7BIT;
    msgs[0].flags = 0;                  /* write: register pointer */
    msgs[0].len   = 1;
    msgs[0].buf   = &base;
    msgs[1].addr  = TOUCH_ADDR_7BIT;
    msgs[1].flags = I2C_M_RD;           /* read: 16-byte block */
    msgs[1].len   = 16;
    msgs[1].buf   = buf;

    msgset.msgs  = msgs;
    msgset.nmsgs = 2;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        int transferred = ioctl(s_fd, I2C_RDWR, &msgset);

        /* I2C_RDWR returns the number of messages transferred, not zero. */
        if (transferred == (int)msgset.nmsgs)
            return 0;
        if (transferred >= 0)
            errno = EIO;
        usleep(3000);
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
    return 0;
}

/* Take two reads to reject scan-cycle glitches. The status and
 * coordinates occupy the first four bytes; later slots can change
 * independently and are not part of this sample. */
int Touch_Sample(void)
{
    uint8_t first[16], second[16];
    uint8_t count;

    if (blk_read(0x00, first) != 0 || blk_read(0x00, second) != 0)
        return -1;
    if (memcmp(first, second, 4) != 0)
        return -1;

    memcpy(s_blk0, second, sizeof s_blk0);
    count = s_blk0[0] & 0x07U;
    return (count >= 1U && count <= 5U) ? 1U : 0U;
}

uint8_t Touch_SelfTest(void)
{
    /* the first read after open can NACK while the IC's scan cycle is
     * busy - retry hard before declaring the bus dead */
    for (int i = 0; i < 25; i++)
        if (blk_read(0x00, s_blk0) == 0)
            return 1;
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
