/*
  spi_hw.c - SPI transport for the CO5300 panel (Orange Pi Zero 3W,
  /dev/spidev3.0, hw CS0 = PE0). See spi_hw.h for the framing rules.

  Each command or pixel row is one standard wrapped-command CS frame.
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
#define BATCH_ROWS   5

static int      s_fd = -1;
static unsigned s_speed_hz;

/* header + one row of pixels (368 * 2 B) always fits */
static uint8_t s_frame[FRAME_MAX];
static uint8_t s_row_frames[BATCH_ROWS][FRAME_MAX];

int spi_open(unsigned speed_hz)
{
    uint8_t bits = 8;
    uint8_t standard_mode = SPI_MODE_3;

    s_speed_hz = speed_hz;
    s_fd = open(SPI_DEV, O_RDWR);
    if (s_fd < 0)
    {
        log_printf(LOG_LVL_ERROR, "open " SPI_DEV ": %s", strerror(errno));
        return -1;
    }
    if (ioctl(s_fd, SPI_IOC_WR_MODE, &standard_mode) < 0 ||
        ioctl(s_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(s_fd, SPI_IOC_WR_MAX_SPEED_HZ, &s_speed_hz) < 0)
    {
        perror("spi ioctl");
        close(s_fd);
        s_fd = -1;
        return -1;
    }
    log_printf(LOG_LVL_INFO,
               "spi3: " SPI_DEV " mode 3, standard @ %u Hz requested (actual SCK not measured)",
               s_speed_hz);
    return 0;
}

void spi_close(void)
{
    if (s_fd >= 0)
        close(s_fd);
    s_fd = -1;
}

/* Request a runtime SCK change. The ioctl value is a requested ceiling,
 * not readback of the clock measured on the wire. */
void spi_set_speed(unsigned hz)
{
    s_speed_hz = hz;
    if (s_fd >= 0 && ioctl(s_fd, SPI_IOC_WR_MAX_SPEED_HZ, &s_speed_hz) < 0)
        perror("spi speed ioctl");
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
    ssize_t written = write(s_fd, s_frame, n);

    if (written < 0 || (size_t)written != n)
    {
        log_printf(LOG_LVL_ERROR, "spi write: %s",
                   written < 0 ? strerror(errno) : "short transfer");
        return -1;
    }
    return 0;
}

static int spi_message_rows(struct spi_ioc_transfer *xfers, size_t count,
                            size_t expected)
{
    int rc;

    switch (count)
    {
    case 1: rc = ioctl(s_fd, SPI_IOC_MESSAGE(1), xfers); break;
    case 2: rc = ioctl(s_fd, SPI_IOC_MESSAGE(2), xfers); break;
    case 3: rc = ioctl(s_fd, SPI_IOC_MESSAGE(3), xfers); break;
    case 4: rc = ioctl(s_fd, SPI_IOC_MESSAGE(4), xfers); break;
    case 5: rc = ioctl(s_fd, SPI_IOC_MESSAGE(5), xfers); break;
    default:
        errno = EINVAL;
        rc = -1;
        break;
    }

    if (rc < 0 || (size_t)rc != expected)
    {
        log_printf(LOG_LVL_ERROR, "spi row batch: %s",
                   rc < 0 ? strerror(errno) : "short transfer");
        return -1;
    }
    return 0;
}

int spi_xfer_rows(uint8_t first_cmd, const uint8_t *rows,
                  size_t row_len, size_t row_count)
{
    struct spi_ioc_transfer xfers[BATCH_ROWS];
    size_t frame_len = HEADER_LEN + row_len;
    size_t max_rows, row = 0;

    if (s_fd < 0 || !rows || !row_count || frame_len > FRAME_MAX)
    {
        errno = EINVAL;
        return -1;
    }

    max_rows = FRAME_MAX / frame_len;
    if (max_rows > BATCH_ROWS)
        max_rows = BATCH_ROWS;
    if (!max_rows)
    {
        errno = EMSGSIZE;
        return -1;
    }

    while (row < row_count)
    {
        size_t count = row_count - row;
        if (count > max_rows)
            count = max_rows;

        memset(xfers, 0, count * sizeof xfers[0]);
        for (size_t i = 0; i < count; i++)
        {
            uint8_t *frame = s_row_frames[i];
            frame[0] = SPI_OPCODE;
            frame[1] = 0x00;
            frame[2] = row == 0 && i == 0 ? first_cmd : 0x3C;
            frame[3] = 0x00;
            memcpy(frame + HEADER_LEN, rows + (row + i) * row_len, row_len);

            xfers[i].tx_buf = (uintptr_t)frame;
            xfers[i].len = (uint32_t)frame_len;
            xfers[i].cs_change = i + 1 < count;
        }

        if (spi_message_rows(xfers, count, count * frame_len) != 0)
            return -1;
        row += count;
    }
    return 0;
}

unsigned spi_requested_khz(void)
{
    return s_speed_hz / 1000;
}

/* Some kernel builds expose this optional statistic. The current vendor
 * kernel does not, so this normally returns zero on the board. */
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
