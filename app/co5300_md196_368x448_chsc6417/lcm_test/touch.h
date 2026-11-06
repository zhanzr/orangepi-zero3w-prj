/*
  touch.h - CHSC6417 capacitive touch over TWI0 (/dev/i2c-0) for the
  Orange Pi Zero 3W CO5300 module port.

  JP4.5 = PB2 (TWI0_SCL), JP4.3 = PB3 (TWI0_SDA). 7-bit slave address
  0x2E. Touch data block at register 0x00: byte 0 = {[1:0] point count,
  bit 6 = X bit 9, bit 7 = Y bit 9}, byte 1 = X[7:0], byte 2 = Y[7:0].
  Chip ID at register 0xA7 (same layout three vendor sources agree on;
  see the nucleo-l4r5 reference for the source-by-source note).
*/
#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

#define TOUCH_I2C_DEV "/dev/i2c-0"
#define TOUCH_ADDR_7BIT 0x2EU

int      Touch_Init(void);                 /* 0 ok / -1 bus error  */
uint8_t  Touch_SelfTest(void);             /* 1 = address ACKs     */
uint8_t  Touch_ChipId(void);               /* reg 0xA7, 0 on error */
void     Touch_Read(uint8_t *buf, uint8_t len); /* reg 0x00, zeroed on error */

#endif /* TOUCH_H */
