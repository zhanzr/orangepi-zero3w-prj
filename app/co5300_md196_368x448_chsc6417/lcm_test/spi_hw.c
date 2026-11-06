/*
  spi_hw.c - SPI transport for the CO5300 panel (Orange Pi Zero 3W,
  /dev/spidev3.0, hw CS0 = PE0). See spi_hw.h for the framing rules.

  spidev asserts CS for the whole write() and deasserts afterwards,
  which is exactly one wrapped-protocol CS frame per call. The header
  (02 00 <cmd> 00) and the payload therefore go out in a single write();
  payloads are capped at the staging size and the callers (lcd.c) never
  exceed one pixel row.
*/
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "spi_hw.h"

#include "log.h"

#define SPI_DEV      "/dev/spidev3.0"
#define SPI_OPCODE   0x02           /* 1-data-line transfer opcode */
#define FRAME_MAX    4096           /* spidev bufsiz default */
#define HEADER_LEN   4

static int      s_fd = -1;
static uint8_t  s_mode = SPI_MODE_3;
static unsigned s_speed_hz;

/* header + one row of pixels (368 * 2 B) always fits */
static uint8_t s_frame[FRAME_MAX];

int spi_open(unsigned speed_hz)
{
    uint8_t bits = 8;

    s_speed_hz = speed_hz;
    s_fd = open(SPI_DEV, O_RDWR);
    if (s_fd < 0)
    {
        log_printf(LOG_LVL_ERROR, "open " SPI_DEV ": %s", strerror(errno));
        return -1;
    }
    if (ioctl(s_fd, SPI_IOC_WR_MODE, &s_mode) < 0 ||
        ioctl(s_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(s_fd, SPI_IOC_WR_MAX_SPEED_HZ, &s_speed_hz) < 0)
    {
        perror("spi ioctl");
        close(s_fd);
        s_fd = -1;
        return -1;
    }
    log_printf(LOG_LVL_INFO,
               "spi3: " SPI_DEV " mode %u @ %u Hz requested (driver may clamp)",
               s_mode, s_speed_hz);
    return 0;
}

void spi_close(void)
{
    if (s_fd >= 0)
        close(s_fd);
    s_fd = -1;
}

int spi_xfer(uint8_t cmd, const uint8_t *data, size_t len)
{
    size_t n = HEADER_LEN + len;

    if (s_fd < 0)
        return -1;
    if (n > FRAME_MAX)
    {
        log_printf(LOG_LVL_ERROR,
                   "spi_xfer: %zu B exceeds one CS frame (cmd 0x%02X)",
                   len, cmd);
        return -1;
    }
    s_frame[0] = SPI_OPCODE;
    s_frame[1] = 0x00;
    s_frame[2] = cmd;
    s_frame[3] = 0x00;
    if (len)
        memcpy(s_frame + HEADER_LEN, data, len);
    if (write(s_fd, s_frame, n) < 0)
    {
        perror("spi write");
        return -1;
    }
    return 0;
}

unsigned spi_requested_khz(void)
{
    return s_speed_hz / 1000;
}

/* The sunxi spidev driver keeps per-controller statistics; the average
 * effective SCK shows how the requested rate was actually served. */
unsigned spi_actual_khz(void)
{
    char path[160];
    FILE *fp;
    unsigned khz = 0;

    snprintf(path, sizeof(path),
             "/sys/class/spi_master/spi3/statistics/transfer_speed_hz");
    fp = fopen(path, "r");
    if (fp)
    {
        unsigned long hz = 0;

        if (fscanf(fp, "%lu", &hz) == 1 && hz)
            khz = (unsigned)(hz / 1000);
        fclose(fp);
    }
    return khz;
}
