/*
  touch.c - CHSC6417 capacitive touch over TWI0 (/dev/i2c-0) for the
  Orange Pi Zero 3W CO5300 module port. Protocol per the nucleo-l4r5 /
  nucleo-u575 references: data block at register 0x00 (pointer write +
  read), chip id at 0xA7, 7-bit address 0x2E. The module carries its
  own pull-ups; TWI0 runs 100 kHz.
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

/* write the register pointer, then read len bytes (combined transfer,
 * so the pointer write and the data read stay one I2C transaction) */
static int mem_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
    struct i2c_rdwr_ioctl_data msgset;
    struct i2c_msg msgs[2];
    uint8_t addr8 = TOUCH_ADDR_7BIT << 1;

    (void)addr8;
    msgs[0].addr  = TOUCH_ADDR_7BIT;
    msgs[0].flags = 0;                  /* write: register pointer */
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;
    msgs[1].addr  = TOUCH_ADDR_7BIT;
    msgs[1].flags = I2C_M_RD;           /* read: data */
    msgs[1].len   = len;
    msgs[1].buf   = buf;

    msgset.msgs  = msgs;
    msgset.nmsgs = 2;
    if (ioctl(s_fd, I2C_RDWR, &msgset) < 0)
        return -1;
    return 0;
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

/* Probe the address: reading the chip id both ACKs the address and
 * returns a value, so a wiring/address problem is distinguishable from
 * "no finger down". */
uint8_t Touch_SelfTest(void)
{
    uint8_t id = 0;

    return (mem_read(0xA7, &id, 1) == 0) ? 1U : 0U;
}

uint8_t Touch_ChipId(void)
{
    uint8_t id = 0;

    (void)mem_read(0xA7, &id, 1);
    return id;
}

/* Read len bytes starting at the touch data block (register 0x00,
 * auto-increment). On a bus error the buffer is zeroed (the caller
 * then sees "no touch"). */
void Touch_Read(uint8_t *buf, uint8_t len)
{
    memset(buf, 0, len);

    if (mem_read(0x00, buf, len) != 0)
        memset(buf, 0, len);
}
