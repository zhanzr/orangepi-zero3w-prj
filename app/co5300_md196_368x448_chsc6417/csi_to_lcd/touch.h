/*
  touch.h - CHSC6417 capacitive touch over TWI0 (/dev/i2c-0) for the
  Orange Pi Zero 3W CO5300 module port.

  JP4.5 = PB2 (TWI0_SCL), JP4.3 = PB3 (TWI0_SDA). 7-bit slave address
  0x2E. Protocol per both STM32 reference ports:
    reg 0x00, byte0: bits[2:0] = point count (0 = released, 1..5 =
    contacts), bit6 = X[8], bit7 = Y[8]; byte1 = X[7:0]; byte2 = Y[7:0].
    Access rules: reads
    must be 16-byte-aligned block bases, <= 32 bytes, with retries
    (the IC NACKs reads raced against its scan cycle). After release
    the block freezes at the last report with bit0 cleared.
*/
#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

#define TOUCH_I2C_DEV "/dev/i2c-0"
#define TOUCH_ADDR_7BIT 0x2EU

int      Touch_Init(void);      /* 0 ok / -1 bus error       */
int      Touch_Sample(void);    /* 1 = down, 0 = up, -1 = no
                                 * consistent report; refreshes X/Y */
uint8_t  Touch_SelfTest(void);  /* 1 = bus ACKs (with retries) */
int      Touch_X(void);         /* last reported X (0..368)   */
int      Touch_Y(void);         /* last reported Y (0..448)   */

#endif /* TOUCH_H */
