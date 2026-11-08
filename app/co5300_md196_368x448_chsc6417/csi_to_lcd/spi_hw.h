/*
  spi_hw.h - SPI transport (/dev/spidev3.0) for the CO5300 368x448
  panel on the Orange Pi Zero 3W JP4 header:

    SCK  -> JP4.23 (PE1, SPI3_CLK)
    SI00 -> JP4.19 (PE2, SPI3_MOSI)
    SI01 -> JP4.21 (PE3, SPI3_MISO), unused by standard SPI writes
    CS   -> JP4.24 (PE0, SPI3_CS0 - hardware CS via spidev)

  Commands and pixel rows use `02 00 <cmd> 00`, each in one CS frame.
*/
#ifndef SPI_HW_H
#define SPI_HW_H

#include <stdint.h>
#include <stddef.h>

/* mode 3 (CPOL=1/CPHA=1), MSB first - the CO5300 samples on the rise */
int  spi_open(unsigned speed_hz);
void spi_close(void);
void spi_set_speed(unsigned hz);    /* request runtime SCK change       */

/* One wrapped command transaction: header 02 00 <cmd> 00 + data. */
int  spi_xfer(uint8_t cmd, const uint8_t *data, size_t len);

/* Send separate wrapped row transactions in batched SPI ioctls. */
int  spi_xfer_rows(uint8_t first_cmd, const uint8_t *rows,
                   size_t row_len, size_t row_count);

/* requested rate, kHz */
unsigned spi_requested_khz(void);
/* optional kernel statistic, kHz (0 if unavailable) */
unsigned spi_actual_khz(void);

#endif /* SPI_HW_H */
