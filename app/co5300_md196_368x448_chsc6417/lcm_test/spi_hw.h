/*
  spi_hw.h - SPI transport (/dev/spidev3.0) for the CO5300 368x448
  panel on the Orange Pi Zero 3W JP4 header:

    SCK  -> JP4.23 (PE1, SPI3_CLK)
    MOSI -> JP4.19 (PE2, SPI3_MOSI)
    CS   -> JP4.24 (PE0, SPI3_CS0 - hardware CS via spidev)
    MISO -> JP4.21 (PE3, SPI3_MISO) - NOT wired (panel is write-only)

  Unlike the rk3506 st7365 transport (4-wire DC panel, CS toggling
  between chunks is harmless), the CO5300 wrapped protocol puts the
  4-byte header `02 00 <cmd> 00` INSIDE the CS frame together with the
  payload: one transaction = one write(). A CS toggle mid-payload would
  feed the panel a headerless frame and corrupt the stream, so a
  transaction must never be split - max payload here is one row
  (368 px = 736 B), far below the spidev 4096 B bufsiz.
*/
#ifndef SPI_HW_H
#define SPI_HW_H

#include <stdint.h>
#include <stddef.h>

/* mode 3 (CPOL=1/CPHA=1), MSB first - the CO5300 samples on the rise */
int  spi_open(unsigned speed_hz);   /* opens /dev/spidev3.0, SPI mode 3  */
void spi_close(void);

/* One wrapped transaction: header 02 00 <cmd> 00 + data, ONE CS frame
 * (one write()). len must fit the staging buffer (4092 B). */
int  spi_xfer(uint8_t cmd, const uint8_t *data, size_t len);

/* requested rate, kHz */
unsigned spi_requested_khz(void);
/* effective rate the driver reports, kHz (0 if not readable) */
unsigned spi_actual_khz(void);

#endif /* SPI_HW_H */
