/*
  csi_to_lcd - display MIPI CSI camera(s) on the CO5300 368x448 OLED
  panel (Orange Pi Zero 3W, IMX219 on CAM1/CAM2).

  Capture uses the vendor ISP pipeline: the AWISPdemo tool drives the
  CSI->ISP->VIPP chain (the BSP vin driver does not deliver frames to a
  plain V4L2 app until that pipeline is configured). This app:

    1. checks the requested camera route is available
    2. runs AWISPdemo once (configures formats + captures 1 frame set
       to /tmp/fb<idx>_*.bin)
    3. binds the video input before setting its capture format, streams
       with plain V4L2 (REQBUFS/MMAP), converts each frame to RGB565,
       then bilinearly scales a centered crop to the panel

  Modes (-csi):
    auto  probe CAM1 then CAM2, use the first available (default)
    1     force CAM1 (/dev/video0)
    2     force CAM2 (/dev/video8)
    dual  capture both cameras alternately (the shared ISP/TDM input
          is switched per frame pair), side-by-side on the panel

  Usage: csi_to_lcd [-csi auto|1|2|dual] [-s speed_kHz] [-b bright_hex]
                    [-W cap_w] [-H cap_h] [-n frames]
*/
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "lcd.h"
#include "log.h"
#include "isp_replay.h"
#include "ref/lcd_font_1608.h"
#include "spi_hw.h"
#include "touch.h"

#define PANEL_W   368
#define PANEL_H   448
#define BAND_H    24
#define VIEW_H    (PANEL_H - BAND_H)
#define HALF_W    (PANEL_W / 2)

#define CAM_W 640
#define CAM_H 480

enum { MODE_AUTO, MODE_CAM1, MODE_CAM2, MODE_DUAL };

typedef struct
{
    int vid;
    const char *dev;
    const char *sname;
    const char *csi_entity;
    const char *i2c_id;
    const char *label;
} cam_route_t;

static const cam_route_t ROUTES[2] = {
    { 0, "/dev/video0", "imx219",   "sunxi_csi.0", "11-0010", "CAM1" },
    { 8, "/dev/video8", "imx219_2", "sunxi_csi.1", "9-0010",  "CAM2" },
};

static uint8_t wire[PANEL_W * 2 * PANEL_H];

/* measured chroma bias of the imx219 V26 profile: U/V means
 * 120/104 on typical scenes - neutralize in conversion (args -wb) */
static int wb_u = 8;
static int wb_v = 24;

static volatile sig_atomic_t quit_req;

static void on_signal(int sig)
{
    (void)sig;
    quit_req = 1;
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void put_px(int x, int y, uint16_t v)
{
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H)
        return;
    uint8_t *p = &wire[(y * PANEL_W + x) * 2];
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void fb_string_t(const char *s, int x, int y, uint16_t fg)
{
    while (*s)
    {
        uint8_t c = (uint8_t)*s;

        if (c < 0x20 || c > 0x7E)
            c = '?';
        int idx = c - 0x20;

        for (int row = 0; row < 16; row++)
        {
            uint8_t bits = asc2_1608[idx * 16 + row];
            for (int col = 0; col < 8; col++)
                if ((bits >> col) & 1)
                    put_px(x + col, y + row, fg);
        }
        x += 8;
        s++;
    }
}

static int route_available(const cam_route_t *r)
{
    char p[128];
    struct stat st;

    snprintf(p, sizeof p, "/sys/bus/i2c/devices/%s", r->i2c_id);
    if (stat(p, &st) != 0)
        return 0;
    return access(r->dev, F_OK) == 0;
}

/* bootstrap the shared ISP via the other camera's AWISPdemo run:
 * after a fresh boot, an AWISPdemo run on one camera initializes the
 * ISP hardware; the selected camera may need it configured first
 * (empirically CAM2-first works after every boot) */
static void awisp_bootstrap(const cam_route_t *r, int w, int h)
{
    const cam_route_t *other = (r == &ROUTES[0]) ? &ROUTES[1] : &ROUTES[0];
    char cmd[160];

    if (!route_available(other))
    {
        log_printf(LOG_LVL_WARN, "[ISP] bootstrap: %s unavailable",
               other->label);
        return;
    }
    log_printf(LOG_LVL_INFO,
           "[ISP] bootstrapping shared ISP via %s AWISPdemo...",
           other->label);
    snprintf(cmd, sizeof cmd,
             "timeout 20 AWISPdemo %d 0 %d %d /tmp 1 1 >/dev/null 2>&1",
             other->vid, w, h);
    (void)system(cmd);
}

/* run AWISPdemo once: configures formats + captures 1 frame set */
static int awisp_configure(const cam_route_t *r, int w, int h)
{
    char cmd[160];

    /* clear any orphaned AWISPdemo from a previous interrupted run -
     * it would hold the video node and make every new run fail */
    (void)system("pkill -f AWISPdemo >/dev/null 2>&1");
    usleep(200000);

    /* timeout-bounded: if the ISP wedges, AWISPdemo can hang forever
     * and block the app inside system() */
    snprintf(cmd, sizeof cmd,
             "timeout 20 AWISPdemo %d 0 %d %d /tmp 1 1 >/dev/null 2>&1",
             r->vid, w, h);
    log_printf(LOG_LVL_INFO, "[ISP] configuring %s via AWISPdemo (%dx%d)...",
           r->label, w, h);
    int rc = system(cmd);
    if (rc != 0)
    {
        log_printf(LOG_LVL_WARN, "AWISPdemo rc=%d - bootstrapping ISP,"
                   " retrying", rc);
        awisp_bootstrap(r, w, h);
        usleep(300000);
        rc = system(cmd);
        if (rc != 0)
        {
            log_printf(LOG_LVL_ERROR, "AWISPdemo failed again (rc=%d)", rc);
            return -1;
        }
    }
    return 0;
}

/* switch the shared ISP/TDM input between CAM1 and CAM2 */
static void tdm_switch(int cam_idx)
{
    char cmd[200];

    snprintf(cmd, sizeof cmd,
             "media-ctl -d /dev/media0 -l"
             " '\"sunxi_csi.0\":1->\"sunxi_tdm_rx.0\":0[%d]'"
             " '\"sunxi_csi.1\":1->\"sunxi_tdm_rx.0\":0[%d]'"
             " >/dev/null 2>&1",
             cam_idx == 0 ? 1 : 0, cam_idx == 0 ? 0 : 1);
    (void)system(cmd);
}

/* (re)configure the sensor-side pipeline: links + RAW10 pad formats.
 * Needed after every vin module reload - the pad formats do not
 * survive rmmod/modprobe. The sensor->mipi->csi and
 * tdm->isp->scaler->vin_cap links are enabled by the driver itself
 * (verified via media-ctl -p); we route the csi->tdm mux EXCLUSIVELY
 * to the selected camera (both enabled confuses the driver's
 * route bookkeeping: S_INPUT then fails with ENODEV on the other
 * camera) and set the sensor-side bus formats. ISP/scaler pads are
 * left to the vendor AWISPdemo, which configures them correctly. */
static void media_setup(const cam_route_t *sel)
{
    /* exclusive tdm mux routing for the selected camera first */
    tdm_switch(sel == &ROUTES[0] ? 0 : 8);

    static const char *const fmts[] = {
        "media-ctl -d /dev/media0 --set-v4l2 '\"imx219\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
        "media-ctl -d /dev/media0 --set-v4l2 '\"sunxi_mipi.0\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
        "media-ctl -d /dev/media0 --set-v4l2 '\"sunxi_csi.0\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
        "media-ctl -d /dev/media0 --set-v4l2 '\"sunxi_tdm_rx.0\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
        "media-ctl -d /dev/media0 --set-v4l2 '\"imx219_2\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
        "media-ctl -d /dev/media0 --set-v4l2 '\"sunxi_mipi.1\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
        "media-ctl -d /dev/media0 --set-v4l2 '\"sunxi_csi.1\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
        "media-ctl -d /dev/media0 --set-v4l2 '\"sunxi_isp.0\":0[SRGGB10_1X10/1920x1080]' 2>/dev/null",
    };

    /* best-effort: re-applying an existing link/format returns EINVAL
     * which is fine - the driver has these enabled already */
    for (unsigned i = 0; i < sizeof fmts / sizeof fmts[0]; i++)
        (void)system(fmts[i]);
}

/* ---- V4L2 MPLANE capture ---- */
#define CAM_NBUF 5   /* matches the vendor AWISPdemo */
typedef struct
{
    int fd;
    int type;
    int nplanes;
    int nbufs;
    int width;
    int height;
    uint32_t pixelformat;
    int full_range;
    int matrix_709;
    size_t stride[3];
    struct { void *p[3]; size_t len[3]; } buf[CAM_NBUF];
} cam_stream_t;

static int cam_xioctl(int fd, unsigned req, void *arg)
{
    int v;
    do { v = ioctl(fd, req, arg); } while (v < 0 && errno == EINTR);
    return v;
}

static int cam_open_stream(cam_stream_t *c, const char *dev,
                           int w, int h)
{
    memset(c, 0, sizeof *c);
    c->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    c->fd = open(dev, O_RDWR | O_NONBLOCK);
    if (c->fd < 0)
    {
        log_printf(LOG_LVL_ERROR, "[CAM] open %s: %s", dev, strerror(errno));
        return -1;
    }

    /* Mimic the vendor AWISPdemo's exact video-node sequence (verified
     * by LD_PRELOAD ioctl tracing). The order matters on this BSP:
     * S_INPUT first binds the sensor->vinc route in the driver's
     * bookkeeping ("imx219 is not used" errors otherwise), then
     * S_PARM/S_FMT/REQBUFS work as expected. */
    int input = 0;
    if (cam_xioctl(c->fd, VIDIOC_S_INPUT, &input) < 0)
    {
        /* fatal: without the route binding, touching the pipeline can
         * crash the kernel (observed on a mismatched tdm mux state) */
        log_printf(LOG_LVL_ERROR, "[CAM] S_INPUT: %s - route not bound,"
                   " aborting before pipeline setup", strerror(errno));
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (cam_xioctl(c->fd, VIDIOC_S_PARM, &parm) < 0)
        log_printf(LOG_LVL_WARN, "[CAM] S_PARM: %s", strerror(errno));

    /* AWISPdemo's 2-byte private set between S_PARM and S_FMT */
    unsigned char cf2[2] = { 0x00, 0x00 };
    if (cam_xioctl(c->fd, (unsigned)0xc00256cf, cf2) < 0)
        log_printf(LOG_LVL_WARN, "[CAM] private 0xc00256cf: %s",
                   strerror(errno));

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (cam_xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        log_printf(LOG_LVL_ERROR, "[CAM] S_FMT YUV420M %dx%d: %s",
                   w, h, strerror(errno));
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    c->width = (int)fmt.fmt.pix_mp.width;
    c->height = (int)fmt.fmt.pix_mp.height;
    c->pixelformat = fmt.fmt.pix_mp.pixelformat;
    c->nplanes = (int)fmt.fmt.pix_mp.num_planes;
    if (c->width <= 0 || c->height <= 0 ||
        (c->width & 1) || (c->height & 1) ||
        (c->pixelformat != V4L2_PIX_FMT_YUV420M &&
         c->pixelformat != V4L2_PIX_FMT_NV12M) ||
        (c->pixelformat == V4L2_PIX_FMT_YUV420M && c->nplanes != 3) ||
        (c->pixelformat == V4L2_PIX_FMT_NV12M && c->nplanes != 2))
    {
        log_printf(LOG_LVL_ERROR,
                   "[CAM] unsupported capture format %c%c%c%c %dx%d planes=%d",
                   c->pixelformat & 0xff, (c->pixelformat >> 8) & 0xff,
                   (c->pixelformat >> 16) & 0xff,
                   (c->pixelformat >> 24) & 0xff,
                   c->width, c->height, c->nplanes);
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    c->stride[0] = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    c->stride[1] = fmt.fmt.pix_mp.plane_fmt[1].bytesperline;
    c->stride[2] = fmt.fmt.pix_mp.plane_fmt[2].bytesperline;
    if (c->stride[0] < (size_t)c->width)
        c->stride[0] = (size_t)c->width;
    size_t min_uv_stride = c->pixelformat == V4L2_PIX_FMT_NV12M ?
                           (size_t)c->width : (size_t)c->width / 2;
    if (c->stride[1] < min_uv_stride)
        c->stride[1] = min_uv_stride;
    if (c->nplanes == 3 && c->stride[2] < (size_t)c->width / 2)
        c->stride[2] = (size_t)c->width / 2;
    c->full_range =
        fmt.fmt.pix_mp.quantization == V4L2_QUANTIZATION_FULL_RANGE ||
        (fmt.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT &&
         (fmt.fmt.pix_mp.colorspace == V4L2_COLORSPACE_JPEG ||
          fmt.fmt.pix_mp.colorspace == V4L2_COLORSPACE_SRGB));
    c->matrix_709 =
        fmt.fmt.pix_mp.ycbcr_enc == V4L2_YCBCR_ENC_709 ||
        fmt.fmt.pix_mp.ycbcr_enc == V4L2_YCBCR_ENC_XV709 ||
        (fmt.fmt.pix_mp.ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT &&
         fmt.fmt.pix_mp.colorspace == V4L2_COLORSPACE_REC709);
    log_printf(LOG_LVL_INFO, "[CAM] format %c%c%c%c %dx%d planes=%d"
           " stride=%zu/%zu/%zu %s-range BT.%s plane0=%uB",
           c->pixelformat & 0xff,
           (c->pixelformat >> 8) & 0xff,
           (c->pixelformat >> 16) & 0xff,
           (c->pixelformat >> 24) & 0xff,
           c->width, c->height, c->nplanes,
           c->stride[0], c->stride[1],
           c->nplanes == 3 ? c->stride[2] : 0,
           c->full_range ? "full" : "limited",
           c->matrix_709 ? "709" : "601",
           fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof req);
    req.count = CAM_NBUF;
    req.type = c->type;
    req.memory = V4L2_MEMORY_MMAP;
    if (cam_xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0)
    {
        log_printf(LOG_LVL_ERROR, "[CAM] REQBUFS: %s", strerror(errno));
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    c->nbufs = req.count;
    if (c->nbufs < 1 || c->nbufs > CAM_NBUF)
    {
        log_printf(LOG_LVL_ERROR, "[CAM] invalid buffer count %d", c->nbufs);
        close(c->fd);
        c->fd = -1;
        return -1;
    }

    for (int i = 0; i < c->nbufs; i++)
    {
        struct v4l2_buffer b;
        struct v4l2_plane p[3];
        memset(&b, 0, sizeof b);
        memset(p, 0, sizeof p);
        b.type = c->type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.length = c->nplanes; b.m.planes = p;
        if (cam_xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0)
        { perror("[CAM] querybuf"); return -1; }
        for (int pl = 0; pl < c->nplanes; pl++)
        {
            size_t rows = pl == 0 ? (size_t)c->height :
                          ((size_t)c->height + 1U) / 2U;
            if ((size_t)p[pl].length < c->stride[pl] * rows)
            {
                log_printf(LOG_LVL_ERROR,
                           "[CAM] plane %d buffer too small (%u < %zu)",
                           pl, p[pl].length, c->stride[pl] * rows);
                return -1;
            }
            c->buf[i].len[pl] = p[pl].length;
            c->buf[i].p[pl] = mmap(NULL, p[pl].length, PROT_READ,
                                   MAP_SHARED, c->fd, p[pl].m.mem_offset);
            if (c->buf[i].p[pl] == MAP_FAILED)
            { perror("[CAM] mmap"); return -1; }
        }
    }

    int type = c->type;
    for (int i = 0; i < c->nbufs; i++)
    {
        struct v4l2_buffer b;
        struct v4l2_plane p[3];
        memset(&b, 0, sizeof b);
        memset(p, 0, sizeof p);
        b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.length = c->nplanes; b.m.planes = p;
        if (cam_xioctl(c->fd, VIDIOC_QBUF, &b) < 0) { perror("qbuf"); return -1; }
    }

    /* AWISPdemo's ordering: buffers queued, then the ISP parameter
     * table is fired at the isp.0/h3a.0 subdevs, then STREAMON.
     * Without this replay the ISP drops every frame. */
    if (isp_replay_apply() != 0)
        log_printf(LOG_LVL_WARN, "[ISP] parameter replay had failures");
    else
        log_printf(LOG_LVL_INFO, "[ISP] %u+%u parameter blocks applied",
               ISP_REPLAY_ISP_N, ISP_REPLAY_H3A_N);

    return 0;
}

/* STREAMON (after buffers are queued and ISP params applied) */
static int cam_stream_start(cam_stream_t *c)
{
    int type = c->type;

    if (cam_xioctl(c->fd, VIDIOC_STREAMON, &type) < 0)
    {
        log_printf(LOG_LVL_ERROR, "[CAM] STREAMON: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int cam_dq(const cam_stream_t *c, int *idx)
{
    struct v4l2_buffer b;
    struct v4l2_plane p[3];
    fd_set fds;
    struct timeval tv = { 2, 0 };
    static int dq_errs, sel_tos;

    FD_ZERO(&fds);
    FD_SET(c->fd, &fds);
    if (select(c->fd + 1, &fds, NULL, NULL, &tv) <= 0)
    {
        if (sel_tos++ % 30 == 0)
            log_printf(LOG_LVL_WARN, "[CAM] select timeout (%d)", sel_tos);
        return -1;
    }
    memset(&b, 0, sizeof b);
    memset(p, 0, sizeof p);
    b.type = c->type;
    b.memory = V4L2_MEMORY_MMAP;
    /* planes array capacity - must cover vb->num_planes or DQBUF
     * fails EINVAL (and the buffer stays queued) */
    b.length = c->nplanes;
    b.m.planes = p;
    if (cam_xioctl(c->fd, VIDIOC_DQBUF, &b) < 0)
    {
        /* the driver can signal readable on error events every few
         * microseconds - throttle hard to avoid a log flood */
        if (dq_errs++ < 3)
            log_printf(LOG_LVL_WARN, "[CAM] DQBUF: %s", strerror(errno));
        usleep(10000);
        return -1;
    }
    *idx = b.index;
    return 0;
}

static void cam_q(const cam_stream_t *c, int idx)
{
    struct v4l2_buffer b;
    struct v4l2_plane p[3];
    memset(&b, 0, sizeof b);
    memset(p, 0, sizeof p);
    b.type = c->type;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = idx;
    b.length = c->nplanes;
    b.m.planes = p;
    cam_xioctl(c->fd, VIDIOC_QBUF, &b);
}

static void cam_streamoff_close(cam_stream_t *c)
{
    int type = c->type;
    cam_xioctl(c->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < c->nbufs; i++)
        for (int pl = 0; pl < c->nplanes; pl++)
            if (c->buf[i].p[pl])
                munmap(c->buf[i].p[pl], c->buf[i].len[pl]);
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

/* ---- Full/limited-range YUV420M / NV12M -> RGB565 crop-fill ---- */
#define CLAMP8(x) ((x) < 0 ? 0 : (x) > 255 ? 255 : (x))

typedef struct
{
    int lo;
    int hi;
    unsigned frac;
} axis_sample_t;

static void make_axis_map(axis_sample_t *map, int out_size, int in_size,
                          double crop_start, double crop_size)
{
    double step = crop_size / out_size;

    for (int i = 0; i < out_size; i++)
    {
        double pos = crop_start + (i + 0.5) * step - 0.5;
        if (pos < 0.0)
            pos = 0.0;
        else if (pos > in_size - 1)
            pos = in_size - 1;

        map[i].lo = (int)pos;
        map[i].hi = map[i].lo + 1 < in_size ? map[i].lo + 1 : map[i].lo;
        map[i].frac = (unsigned)((pos - map[i].lo) * 256.0);
    }
}

static int sample_bilinear(const uint8_t *plane, size_t stride,
                           int pixel_stride, const axis_sample_t *x,
                           const axis_sample_t *y)
{
    unsigned wx0 = 256U - x->frac;
    unsigned wy0 = 256U - y->frac;
    const uint8_t *row0 = plane + (size_t)y->lo * stride;
    const uint8_t *row1 = plane + (size_t)y->hi * stride;
    unsigned top =
        row0[(size_t)x->lo * pixel_stride] * wx0 +
        row0[(size_t)x->hi * pixel_stride] * x->frac;
    unsigned bottom =
        row1[(size_t)x->lo * pixel_stride] * wx0 +
        row1[(size_t)x->hi * pixel_stride] * x->frac;

    return (int)((top * wy0 + bottom * y->frac + 32768U) >> 16);
}

static void yuv_to_wire(const cam_stream_t *c, int idx,
                        int dst_x, int dst_w, int dst_y, int dst_h)
{
    const uint8_t *yp = (const uint8_t *)c->buf[idx].p[0];
    const uint8_t *up = (const uint8_t *)c->buf[idx].p[1];
    const uint8_t *vp = (const uint8_t *)c->buf[idx].p[2];
    int uv_w = c->width / 2;
    int uv_h = c->height / 2;
    axis_sample_t x_map[PANEL_W], x_uv_map[PANEL_W];
    axis_sample_t y_map[VIEW_H], y_uv_map[VIEW_H];
    double crop_w = c->width;
    double crop_h = c->height;
    double target_aspect = (double)dst_w / dst_h;
    double source_aspect = (double)c->width / c->height;
    if (source_aspect > target_aspect)
        crop_w = c->height * target_aspect;
    else
        crop_h = c->width / target_aspect;
    double crop_x = (c->width - crop_w) * 0.5;
    double crop_y = (c->height - crop_h) * 0.5;

    make_axis_map(x_map, dst_w, c->width, crop_x, crop_w);
    make_axis_map(y_map, dst_h, c->height, crop_y, crop_h);
    make_axis_map(x_uv_map, dst_w, uv_w, crop_x * 0.5, crop_w * 0.5);
    make_axis_map(y_uv_map, dst_h, uv_h, crop_y * 0.5, crop_h * 0.5);

    for (int py = 0; py < dst_h; py++)
    {
        uint8_t *wrow = &wire[((dst_y + py) * PANEL_W + dst_x) * 2];

        for (int px = 0; px < dst_w; px++)
        {
            int yy = sample_bilinear(yp, c->stride[0], 1,
                                     &x_map[px], &y_map[py]);
            int uu, vv;

            if (c->pixelformat == V4L2_PIX_FMT_NV12M)
            {
                uu = sample_bilinear(up, c->stride[1], 2,
                                     &x_uv_map[px], &y_uv_map[py]);
                vv = sample_bilinear(up + 1, c->stride[1], 2,
                                     &x_uv_map[px], &y_uv_map[py]);
            }
            else
            {
                uu = sample_bilinear(up, c->stride[1], 1,
                                     &x_uv_map[px], &y_uv_map[py]);
                vv = sample_bilinear(vp, c->stride[2], 1,
                                     &x_uv_map[px], &y_uv_map[py]);
            }

            int d = uu - 128 + wb_u;
            int e = vv - 128 + wb_v;
            int r, g, b;
            if (c->full_range)
            {
                r = yy + ((c->matrix_709 ? 403 : 359) * e + 128) / 256;
                g = yy - (((c->matrix_709 ? 48 : 88) * d +
                           (c->matrix_709 ? 120 : 183) * e + 128) / 256);
                b = yy + ((c->matrix_709 ? 475 : 454) * d + 128) / 256;
            }
            else
            {
                int y = yy - 16;
                if (y < 0)
                    y = 0;
                r = ((298 * y + (c->matrix_709 ? 459 : 409) * e + 128) /
                     256);
                g = ((298 * y - (c->matrix_709 ? 55 : 100) * d -
                      (c->matrix_709 ? 136 : 208) * e + 128) / 256);
                b = ((298 * y + (c->matrix_709 ? 541 : 516) * d + 128) /
                     256);
            }
            uint16_t px565 = (uint16_t)(((CLAMP8(r) >> 3) << 11) |
                                        ((CLAMP8(g) >> 2) << 5) |
                                        (CLAMP8(b) >> 3));
            wrow[px * 2]     = (uint8_t)(px565 >> 8);
            wrow[px * 2 + 1] = (uint8_t)px565;
        }
    }
}

/* ---- status band ---- */
static void band_clear(void)
{
    memset(&wire[VIEW_H * PANEL_W * 2], 0x00, BAND_H * PANEL_W * 2);
}

static void band_text(const char *s)
{
    band_clear();
    fb_string_t(s, 4, VIEW_H + 4, 0xFFE0); /* yellow */
}

static int wire_blit(void)
{
    return spi_xfer_rows(0x2C, wire, (size_t)PANEL_W * 2, PANEL_H);
}

/* ---- touch ---- */
static volatile int g_touch_ok;
static volatile int g_touch_down;

/* Touch I2C sampling occasionally blocks for hundreds of ms inside
 * the kernel (TWI bus recovery after the IC's periodic busy window).
 * Polling from a background thread keeps those stalls out of the
 * render loop - frame time no longer depends on touch bus health. */
static void *touch_thread(void *arg);
static int g_touch_thread_running;

static void touch_poll(void)
{
    static uint8_t dn_run, up_run;
    int raw, x, y;

    if (!g_touch_ok)
        return;
    raw = Touch_Sample();
    /* -1 = failed or pace-skipped sample: no state change. Treating
     * it as truthy (raw is int!) registered false touch-downs from
     * stale coordinates. */
    if (raw < 0)
        return;
    x = Touch_X();
    y = Touch_Y();
    if (raw) { dn_run = 1; up_run = 0; }
    else     { up_run = 1; dn_run = 0; }

    /* Touch_Sample() confirms down-reports with a second read, so a
     * single valid sample is trustworthy; at the app's frame cadence
     * this also debounces. */
    if (!g_touch_down && dn_run >= 1 && (x > 0 || y > 0))
    {
        g_touch_down = 1;
        log_printf(LOG_LVL_INFO, "[TOUCH] down X=%d Y=%d (%d-Y=%d)",
               x, y, PANEL_H, PANEL_H - y);
    }
    else if (g_touch_down && up_run >= 1)
    {
        g_touch_down = 0;
        log_printf(LOG_LVL_INFO, "[TOUCH] release");
    }
}

/* background touch sampler: isolates TWI bus-recovery stalls from the
 * render loop (observed 500+ ms kernel-side stalls per incident) */
static void *touch_thread(void *arg)
{
    (void)arg;
    while (!quit_req)
    {
        if (g_touch_ok)
            touch_poll();
        usleep(100000);   /* 10 Hz sampling */
    }
    return NULL;
}

/* ---- main ---- */
int main(int argc, char **argv)
{
    unsigned speed = 40000;
    unsigned bright = 0x28;
    int cam_w = CAM_W, cam_h = CAM_H;
    int mode = MODE_AUTO;
    long max_frames = 0;
    int no_touch = 0;

    log_init("csi_to_lcd");
    log_config_from_argv(argc, argv);
    /* survive SIGKILL with the log intact */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-csi") && i + 1 < argc)
        {
            const char *m = argv[++i];
            if (!strcmp(m, "auto"))       mode = MODE_AUTO;
            else if (!strcmp(m, "1"))     mode = MODE_CAM1;
            else if (!strcmp(m, "2"))     mode = MODE_CAM2;
            else if (!strcmp(m, "dual"))  mode = MODE_DUAL;
            else { log_printf(LOG_LVL_ERROR, "bad -csi value %s", m); return 1; }
        }
        else if (!strcmp(argv[i], "-no-touch"))
        {
            no_touch = 1;
        }
        else if (!strcmp(argv[i], "-wb") && i + 1 < argc)
        {
            /* chroma bias correction: neutral points for U/V (the
             * imx219 V26 profile measures U=120, V=104 on typical
             * scenes); "-wb 128,128" disables the correction */
            if (sscanf(argv[++i], "%d,%d", &wb_u, &wb_v) != 2)
            {
                log_printf(LOG_LVL_ERROR, "bad -wb value (want u,v)");
                return 1;
            }
        }
        else if (argv[i][0] == '-' && argv[i][1] && !argv[i][2])
        {
            switch (argv[i][1])
            {
            case 's': speed = (unsigned)atoi(argv[++i]); break;
            case 'b': bright = (uint8_t)strtoul(argv[++i], NULL, 0); break;
            case 'W': cam_w = atoi(argv[++i]); break;
            case 'H': cam_h = atoi(argv[++i]); break;
            case 'n': max_frames = atol(argv[++i]); break;
            default:
                log_printf(LOG_LVL_INFO,
                       "usage: csi_to_lcd [-csi auto|1|2|dual] [-s kHz]"
                       " [-b bright_hex] [-W w] [-H h] [-n frames] [-no-touch] [-wb u,v]"
                       " [-no-touch] (default -s 40000)");
                return 1;
            }
        }
        else
        {
            log_printf(LOG_LVL_INFO,
                   "usage: csi_to_lcd [-csi auto|1|2|dual] [-s kHz]"
                   " [-b bright_hex] [-W w] [-H h] [-n frames] [-no-touch] [-wb u,v]"
                   " [-no-touch] (default -s 40000)");
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    log_printf(LOG_LVL_INFO,
           "csi_to_lcd: mode %s, panel %dx%d (view %dx%d + %d band),"
           " capture %dx%d NV12/YUV420, SPI %u kHz",
           mode == MODE_DUAL ? "dual" :
           mode == MODE_CAM1 ? "CAM1" :
           mode == MODE_CAM2 ? "CAM2" : "auto",
           PANEL_W, PANEL_H, PANEL_W, VIEW_H, BAND_H,
           cam_w, cam_h, speed);
    log_printf(LOG_LVL_INFO,
           "scale: capture %dx%d -> scaled %dx%d (by height),"
           " center-crop to %dx%d per view",
           cam_w, cam_h,
           cam_w * VIEW_H / cam_h, VIEW_H, PANEL_W, VIEW_H);

    /* ---- route availability ---- */
    const cam_route_t *r1 = &ROUTES[0], *r2 = &ROUTES[1];
    cam_stream_t ca, cb;
    memset(&ca, 0, sizeof ca);
    memset(&cb, 0, sizeof cb);
    cb.fd = -1;   /* unused in single-camera modes */

    /* ---- load the VIN modules (not auto-loaded on this image) ---- */
    if (system("modprobe vin_io >/dev/null 2>&1; modprobe imx219"
               " >/dev/null 2>&1; modprobe vin_v4l2 >/dev/null 2>&1") != 0)
        log_printf(LOG_LVL_WARN,
               "[MOD] modprobe failed - are the vin modules installed?");
    sleep(1);

    if (mode == MODE_DUAL)
    {
        if (!route_available(r1) || !route_available(r2))
        {
            log_printf(LOG_LVL_ERROR,
                   "[DUAL] route missing: %s (%s) / %s (%s) -"
                   " both cameras required",
                   r1->label, r1->i2c_id, r2->label, r2->i2c_id);
            return 1;
        }
    }
    else if (mode == MODE_CAM1)
    {
        if (!route_available(r1))
        {
            log_printf(LOG_LVL_ERROR,
                   "%s not available (no i2c client %s or %s missing)",
                   r1->label, r1->i2c_id, r1->dev);
            return 1;
        }
    }
    else if (mode == MODE_CAM2)
    {
        if (!route_available(r2))
        {
            log_printf(LOG_LVL_ERROR,
                   "%s not available (no i2c client %s or %s missing)",
                   r2->label, r2->i2c_id, r2->dev);
            return 1;
        }
        r1 = r2;
    }
    else /* MODE_AUTO: first available, CAM1 preferred */
    {
        if (route_available(r1))
            ;
        else if (route_available(r2))
        {
            log_printf(LOG_LVL_INFO,
                   "[AUTO] CAM1 unavailable - using CAM2");
            r1 = r2;
        }
        else
        {
            log_printf(LOG_LVL_ERROR,
                   "[AUTO] no camera available (checked %s and %s)",
                   r1->i2c_id, r2->i2c_id);
            return 1;
        }
    }

    /* ---- touch (panel controller, TWI0) - optional ---- */
    if (!no_touch)
    {
        if (Touch_Init() == 0 && Touch_SelfTest())
            g_touch_ok = 1;
        log_printf(LOG_LVL_INFO, "[TOUCH] self-test: %s",
               g_touch_ok ? "ACK (chip present)" : "no bus / no ACK");
    }
    else
    {
        log_printf(LOG_LVL_INFO, "[TOUCH] disabled (-no-touch)");
    }

    /* touch sampling in a background thread - bus-recovery stalls
     * must not stall the render loop */
    pthread_t touch_tid;
    if (g_touch_ok && pthread_create(&touch_tid, NULL, touch_thread, NULL) == 0)
        g_touch_thread_running = 1;

    if (spi_open(speed * 1000) != 0)
        return 1;

    spi_set_speed(10000000);
    LCD_Init();
    LCD_SetBrightness(bright);
    LCD_Clear();
    spi_set_speed(speed * 1000);

    /* ---- configure the media pipeline, then the route(s) via AWISPdemo ---- */
    media_setup(r1);
    if (mode == MODE_DUAL)
    {
        /* each AWISPdemo config runs under its own exclusive mux state */
        tdm_switch(0);
        if (awisp_configure(r1, cam_w, cam_h) != 0)
            return 1;
        tdm_switch(8);
        if (awisp_configure(r2, cam_w, cam_h) != 0)
            return 1;
    }
    else
    {
        if (awisp_configure(r1, cam_w, cam_h) != 0)
            return 1;
    }

    /* ---- open the capture stream(s) ---- */
    if (mode == MODE_DUAL)
    {
        /* dual uses ONE stream, fully re-opened on each route switch:
         * the BSP will not re-route the shared ISP to the other camera
         * while a previous video node still holds buffers (verified:
         * CAM2 sensor never starts). This replicates the proven
         * AWISPdemo run-per-camera pattern. */
        tdm_switch(0);
        if (cam_open_stream(&ca, r1->dev, cam_w, cam_h) != 0)
            return 1;
        if (cam_stream_start(&ca) != 0)
            return 1;
    }
    else
    {
        if (cam_open_stream(&ca, r1->dev, cam_w, cam_h) != 0)
            return 1;
        if (cam_stream_start(&ca) != 0)
            return 1;
    }

    band_clear();
    band_text("CSI->OLED");
    if (wire_blit() != 0)
        return 1;
    /* show the band immediately - even if capture fails */

    double fps_t0 = now();
    int frames = 0, fps = 0;
    double capture_ms = 0.0, convert_ms = 0.0, display_ms = 0.0;
    double event_ms = 0.0, touch_ms = 0.0;
    long shown = 0;
    int dual_sel = 0;   /* dual: currently-streaming route, 0 = CAM1 */
    int event_errors = 0;

    while (!quit_req && (max_frames == 0 || shown < max_frames))
    {
        double capture_t0 = now();
        double frame_event_ms;
        double convert_t0;
        double frame_capture_ms, frame_convert_ms;

        /* NOTE: isp_events_fetch() (0x80885659 on isp.0/h3a.0) blocks
         * ~830 ms per call under the imx219 ISP library and is NOT
         * required for buffer completion - frames arrive without it.
         * frame_event_ms tracks the (disabled) call for reference. */
        double event_t0 = now();
        frame_event_ms = (now() - event_t0) * 1000.0;

        if (mode == MODE_DUAL)
        {
            /* sequential interleave: close the active stream, flip the
             * tdm mux, open the other one fresh - one stream at a
             * time, exactly like a sequence of AWISPdemo runs */
            int want = (int)(shown & 1);

            if (want != dual_sel)
            {
                const cam_route_t *nr = want ? r2 : r1;

                cam_streamoff_close(&ca);
                tdm_switch(want ? 8 : 0);
                if (cam_open_stream(&ca, nr->dev, cam_w, cam_h) != 0 ||
                    cam_stream_start(&ca) != 0)
                {
                    log_printf(LOG_LVL_ERROR,
                           "[DUAL] switch to %s failed", nr->label);
                    break;
                }
                dual_sel = want;
                log_printf(LOG_LVL_INFO, "[DUAL] switched to %s",
                       nr->label);
            }

            int idx;
            capture_t0 = now();
            if (cam_dq(&ca, &idx) == 0)
            {
                frame_capture_ms = (now() - capture_t0) * 1000.0;
                convert_t0 = now();
                yuv_to_wire(&ca, idx, dual_sel ? HALF_W : 0, HALF_W,
                            0, VIEW_H);
                frame_convert_ms = (now() - convert_t0) * 1000.0;
                cam_q(&ca, idx);
            }
            else
                continue;
        }
        else
        {
            int idx;
            capture_t0 = now();
            if (cam_dq(&ca, &idx) != 0)
            {
                if (quit_req)
                    break;
                continue;
            }
            frame_capture_ms = (now() - capture_t0) * 1000.0;
            convert_t0 = now();
            yuv_to_wire(&ca, idx, 0, PANEL_W, 0, VIEW_H);
            frame_convert_ms = (now() - convert_t0) * 1000.0;
            cam_q(&ca, idx);
        }

        frames++;
        shown++;

        capture_ms += frame_capture_ms;
        convert_ms += frame_convert_ms;
        event_ms += frame_event_ms;
        double display_t0 = now();
        if (wire_blit() != 0)
            break;
        double frame_display_ms = (now() - display_t0) * 1000.0;
        display_ms += frame_display_ms;

        double t = now();
        if (t - fps_t0 >= 1.0)
        {
            double elapsed = t - fps_t0;
            fps = (int)(frames / elapsed);
            char b[24];
            snprintf(b, sizeof b, "CSI %dfps", fps);
            band_text(b);
            log_printf(LOG_LVL_INFO, "[CAM] %d fps, %ld frames shown"
                       " [capture %.0fms convert %.0fms display %.0fms"
                       " ISP %.0fms touch %.0fms/frame]",
                       fps, shown, capture_ms / frames, convert_ms / frames,
                       display_ms / frames, event_ms / frames,
                       touch_ms / frames);
            frames = 0;
            capture_ms = convert_ms = display_ms = 0.0;
            event_ms = touch_ms = 0.0;
            fps_t0 = t;
        }
    }

    log_printf(LOG_LVL_INFO, "exit (%d frames shown) - restoring panel",
           (int)shown);
    cam_streamoff_close(&ca);
    cam_streamoff_close(&cb);
    spi_close();
    return 0;
}
