/*
 * Rockchip VOP (video output processor), PX30/RK3326 "VOP big" model
 *
 * Register layout is the RK3366 "VOP lite" one used for px30_vop_big in
 * drivers/gpu/drm/rockchip/rockchip_vop_reg.c (Linux) and the Rockchip
 * u-boot DRM driver:
 *   0x000 REG_CFG_DONE  0x018 SYS_CTRL2 (standby)  0x020 DSP_CTRL0
 *   0x028 DSP_CTRL2     0x030 LINE_FLAG  0x034 INTR_EN  0x038 INTR_CLEAR
 *   0x03c INTR_STATUS   0x050.. WIN0     0x090.. WIN1
 *   0x100 DSP_HTOTAL_HS_END  0x104 DSP_HACT_ST_END
 *   0x108 DSP_VTOTAL_VS_END  0x10c DSP_VACT_ST_END
 *   0x190.. WIN2 (RK3368 win2 layout, first area only)
 *
 * Writes go to shadow registers which are latched at the next frame
 * start after REG_CFG_DONE.  Each frame raises FS/FS_NEW/LINE_FLAG,
 * entering standby raises DSP_HOLD_VALID.  Frames are scanned out from
 * guest memory, through the VOP IOMMU when paging is enabled, and
 * composed into the QEMU console (window formats ARGB8888, RGB888,
 * RGB565 and, for WIN0, YUV420/422/444 semi-planar; WIN0 scaling is
 * nearest neighbour).  Nothing is shown unless the DSI link/panel is
 * active.  The "rotation" property rotates the console output to match
 * how the panel is mounted.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/rockchip_vop.h"
#include "hw/display/dw_mipi_dsi.h"
#include "hw/misc/rockchip_iommu.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "ui/pixel_ops.h"

#define VOP_REG_CFG_DONE        0x000
#define VOP_VERSION             0x004
#define VOP_DSP_BG              0x008
#define VOP_SYS_CTRL2           0x018
#define VOP_DSP_CTRL0           0x020
#define VOP_DSP_CTRL2           0x028
#define VOP_STATUS              0x02c
#define VOP_LINE_FLAG           0x030
#define VOP_INTR_EN             0x034
#define VOP_INTR_CLEAR          0x038
#define VOP_INTR_STATUS         0x03c
#define VOP_WIN0_CTRL0          0x050
#define VOP_WIN0_VIR            0x05c
#define VOP_WIN0_YRGB_MST       0x060
#define VOP_WIN0_CBR_MST        0x064
#define VOP_WIN0_ACT_INFO       0x068
#define VOP_WIN0_DSP_INFO       0x06c
#define VOP_WIN0_DSP_ST         0x070
#define VOP_WIN1_CTRL0          0x090
#define VOP_WIN1_VIR            0x098
#define VOP_WIN1_MST            0x0a0
#define VOP_WIN1_DSP_INFO       0x0a4
#define VOP_WIN1_DSP_ST         0x0a8
#define VOP_DSP_HTOTAL_HS_END   0x100
#define VOP_DSP_HACT_ST_END     0x104
#define VOP_DSP_VTOTAL_VS_END   0x108
#define VOP_DSP_VACT_ST_END     0x10c
#define VOP_WIN2_CTRL0          0x190
#define VOP_WIN2_VIR0_1         0x198
#define VOP_WIN2_MST0           0x1a0
#define VOP_WIN2_DSP_INFO0      0x1a4
#define VOP_WIN2_DSP_ST0        0x1a8

#define SYS_CTRL2_STANDBY       (1u << 1)
#define DSP_CTRL0_RGB_EN        (1u << 0)
#define DSP_CTRL0_HDMI_EN       (1u << 8)
#define DSP_CTRL0_LVDS_EN       (1u << 16)
#define DSP_CTRL0_MIPI_EN       (1u << 24)
#define DSP_CTRL2_BLANK         (1u << 14)
#define DSP_CTRL2_BLACK         (1u << 15)

#define INTR_FS                 (1u << 0)
#define INTR_FS_NEW             (1u << 1)
#define INTR_LINE_FLAG0         (1u << 3)
#define INTR_LINE_FLAG1         (1u << 4)
#define INTR_DSP_HOLD_VALID     (1u << 8)

#define VOP_VERSION_VAL         0x02060000  /* VOP 2.6 */
#define VOP_FRAME_NS            (NANOSECONDS_PER_SECOND / 60)

enum {
    FMT_ARGB8888 = 0,
    FMT_RGB888 = 1,
    FMT_RGB565 = 2,
    FMT_YUV420SP = 4,
    FMT_YUV422SP = 5,
    FMT_YUV444SP = 6,
};

typedef struct VOPWin {
    bool enable;
    int format;
    bool rb_swap;
    uint32_t mst;
    uint32_t cbr_mst;
    uint32_t vir;           /* stride in bytes */
    int act_w, act_h;
    int dsp_w, dsp_h;
    int dsp_x, dsp_y;       /* relative to the active area */
} VOPWin;

static void vop_update_irq(RockchipVOPState *s)
{
    qemu_set_irq(s->irq, !!(s->intr_raw & s->intr_en & 0xffff));
}

static void vop_raise(RockchipVOPState *s, uint32_t bits)
{
    s->intr_raw |= bits;
    vop_update_irq(s);
}

static bool vop_output_enabled(RockchipVOPState *s, const uint32_t *r)
{
    return !(r[VOP_SYS_CTRL2 >> 2] & SYS_CTRL2_STANDBY) &&
           (r[VOP_DSP_CTRL0 >> 2] & (DSP_CTRL0_RGB_EN | DSP_CTRL0_HDMI_EN |
                                     DSP_CTRL0_LVDS_EN | DSP_CTRL0_MIPI_EN));
}

static void vop_get_active_area(const uint32_t *r, int *x0, int *y0,
                                int *w, int *h)
{
    uint32_t hact = r[VOP_DSP_HACT_ST_END >> 2];
    uint32_t vact = r[VOP_DSP_VACT_ST_END >> 2];
    int hst = (hact >> 16) & 0xfff, hend = hact & 0xfff;
    int vst = (vact >> 16) & 0xfff, vend = vact & 0xfff;

    *x0 = hst;
    *y0 = vst;
    *w = hend > hst ? hend - hst : 0;
    *h = vend > vst ? vend - vst : 0;
}

static void vop_get_win(const uint32_t *r, int n, int x0, int y0, VOPWin *w)
{
    uint32_t ctrl, info, st, act;

    memset(w, 0, sizeof(*w));
    switch (n) {
    case 0:
        ctrl = r[VOP_WIN0_CTRL0 >> 2];
        w->enable = ctrl & 1;
        w->format = (ctrl >> 1) & 7;
        w->rb_swap = (ctrl >> 12) & 1;
        w->mst = r[VOP_WIN0_YRGB_MST >> 2];
        w->cbr_mst = r[VOP_WIN0_CBR_MST >> 2];
        w->vir = (r[VOP_WIN0_VIR >> 2] & 0x1fff) * 4;
        act = r[VOP_WIN0_ACT_INFO >> 2];
        info = r[VOP_WIN0_DSP_INFO >> 2];
        st = r[VOP_WIN0_DSP_ST >> 2];
        w->act_w = (act & 0x1fff) + 1;
        w->act_h = ((act >> 16) & 0x1fff) + 1;
        break;
    case 1:
        ctrl = r[VOP_WIN1_CTRL0 >> 2];
        w->enable = ctrl & 1;
        w->format = (ctrl >> 4) & 7;
        w->rb_swap = (ctrl >> 12) & 1;
        w->mst = r[VOP_WIN1_MST >> 2];
        w->vir = (r[VOP_WIN1_VIR >> 2] & 0x1fff) * 4;
        info = r[VOP_WIN1_DSP_INFO >> 2];
        st = r[VOP_WIN1_DSP_ST >> 2];
        w->act_w = (info & 0xfff) + 1;
        w->act_h = ((info >> 16) & 0xfff) + 1;
        break;
    default:
        ctrl = r[VOP_WIN2_CTRL0 >> 2];
        w->enable = (ctrl >> 4) & 1;
        w->format = (ctrl >> 5) & 3;
        w->rb_swap = (ctrl >> 20) & 1;
        w->mst = r[VOP_WIN2_MST0 >> 2];
        w->vir = (r[VOP_WIN2_VIR0_1 >> 2] & 0x1fff) * 4;
        info = r[VOP_WIN2_DSP_INFO0 >> 2];
        st = r[VOP_WIN2_DSP_ST0 >> 2];
        w->act_w = (info & 0xfff) + 1;
        w->act_h = ((info >> 16) & 0xfff) + 1;
        break;
    }
    w->dsp_w = (info & 0xfff) + 1;
    w->dsp_h = ((info >> 16) & 0xfff) + 1;
    w->dsp_x = (int)(st & 0x1fff) - x0;
    w->dsp_y = (int)((st >> 16) & 0x1fff) - y0;
}

/* Read guest memory through the IOMMU, splitting at page boundaries */
static bool vop_dma_read(RockchipVOPState *s, uint32_t addr, void *buf,
                         uint32_t len)
{
    uint8_t *p = buf;

    while (len) {
        uint32_t chunk = MIN(len, 0x1000 - (addr & 0xfff));
        hwaddr pa;

        if (s->iommu) {
            if (!rockchip_iommu_translate(s->iommu, addr, false, &pa)) {
                memset(p, 0, len);
                return false;
            }
        } else {
            pa = addr;
        }
        if (dma_memory_read(&s->dma_as, pa, p, chunk,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            memset(p, 0, chunk);
        }
        p += chunk;
        addr += chunk;
        len -= chunk;
    }
    return true;
}

static int vop_bpp(int format)
{
    switch (format) {
    case FMT_ARGB8888:
        return 4;
    case FMT_RGB888:
        return 3;
    case FMT_RGB565:
        return 2;
    default:
        return 1;   /* luma plane of the YUV formats */
    }
}

static inline uint32_t vop_yuv_to_rgb(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;

    r = MAX(0, MIN(255, r));
    g = MAX(0, MIN(255, g));
    b = MAX(0, MIN(255, b));
    return (r << 16) | (g << 8) | b;
}

/* Convert one source line of a window to xRGB8888 */
static void vop_convert_line(RockchipVOPState *s, const VOPWin *w, int sy,
                             uint32_t *out, uint8_t *line, uint8_t *cline)
{
    int bpp = vop_bpp(w->format);
    bool yuv = w->format >= FMT_YUV420SP;

    vop_dma_read(s, w->mst + sy * w->vir, line, w->act_w * bpp);
    if (yuv) {
        int cy = (w->format == FMT_YUV420SP) ? sy / 2 : sy;
        int cbytes = (w->format == FMT_YUV444SP) ? w->act_w * 2 :
                                                   ((w->act_w + 1) & ~1);
        uint32_t cstride = (w->format == FMT_YUV444SP) ? w->vir * 2 : w->vir;

        vop_dma_read(s, w->cbr_mst + cy * cstride, cline, cbytes);
    }

    for (int x = 0; x < w->act_w; x++) {
        uint32_t px;
        uint8_t *p = line + x * bpp;
        uint8_t r, g, b;

        switch (w->format) {
        case FMT_ARGB8888:
            px = ldl_le_p(p) & 0xffffff;
            break;
        case FMT_RGB888:
            px = p[0] | (p[1] << 8) | (p[2] << 16);
            break;
        case FMT_RGB565: {
            uint16_t v = lduw_le_p(p);

            r = (v >> 11) << 3;
            g = ((v >> 5) & 0x3f) << 2;
            b = (v & 0x1f) << 3;
            px = (r << 16) | (g << 8) | b;
            break;
        }
        case FMT_YUV420SP:
        case FMT_YUV422SP:
        case FMT_YUV444SP: {
            int ci = (w->format == FMT_YUV444SP) ? x * 2 : (x & ~1);

            px = vop_yuv_to_rgb(p[0], cline[ci], cline[ci + 1]);
            break;
        }
        default:
            px = 0;
        }
        if (w->rb_swap) {
            px = (px & 0x00ff00) | ((px >> 16) & 0xff) | ((px & 0xff) << 16);
        }
        out[x] = px;
    }
}

static void vop_draw_win(RockchipVOPState *s, const VOPWin *w,
                         uint32_t *fb, int fb_w, int fb_h)
{
    g_autofree uint32_t *src = NULL;
    g_autofree uint8_t *line = NULL;
    g_autofree uint8_t *cline = NULL;
    int last_sy = -1;

    if (!w->enable || w->act_w <= 0 || w->act_h <= 0 || !w->vir ||
        w->act_w > 4096 || w->act_h > 4096) {
        return;
    }
    if (w->format == 3 || w->format > FMT_YUV444SP) {
        qemu_log_mask(LOG_UNIMP, "rockchip-vop: window format %d\n",
                      w->format);
        return;
    }
    src = g_new(uint32_t, w->act_w);
    line = g_malloc(w->act_w * 4);
    cline = g_malloc(w->act_w * 2 + 2);

    for (int dy = 0; dy < w->dsp_h; dy++) {
        int y = w->dsp_y + dy;
        int sy = (int)((int64_t)dy * w->act_h / w->dsp_h);
        uint32_t *row;

        if (y < 0 || y >= fb_h) {
            continue;
        }
        if (sy != last_sy) {
            vop_convert_line(s, w, sy, src, line, cline);
            last_sy = sy;
        }
        row = fb + y * fb_w;
        for (int dx = 0; dx < w->dsp_w; dx++) {
            int x = w->dsp_x + dx;
            int sx = (int)((int64_t)dx * w->act_w / w->dsp_w);

            if (x >= 0 && x < fb_w) {
                row[x] = src[sx];
            }
        }
    }
}

static bool vop_update_display(void *opaque)
{
    RockchipVOPState *s = opaque;
    const uint32_t *r = s->active;
    DisplaySurface *surface;
    g_autofree uint32_t *fb = NULL;
    int x0, y0, w, h, ow, oh;
    bool visible;

    vop_get_active_area(r, &x0, &y0, &w, &h);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        /* not configured yet: show the native panel size, blank */
        w = 320;
        h = 480;
    }
    ow = (s->rotation == 90 || s->rotation == 270) ? h : w;
    oh = (s->rotation == 90 || s->rotation == 270) ? w : h;

    surface = qemu_console_surface(s->con);
    if (!surface || surface_width(surface) != ow ||
        surface_height(surface) != oh) {
        qemu_console_resize(s->con, ow, oh);
        surface = qemu_console_surface(s->con);
    }

    fb = g_new0(uint32_t, w * h);
    visible = vop_output_enabled(s, r) &&
              !(r[VOP_DSP_CTRL2 >> 2] & (DSP_CTRL2_BLANK | DSP_CTRL2_BLACK)) &&
              (!s->dsi || dw_mipi_dsi_video_active(s->dsi));
    if (visible) {
        uint32_t bg = r[VOP_DSP_BG >> 2] & 0xffffff;
        VOPWin win;

        for (int i = 0; i < w * h; i++) {
            fb[i] = bg;
        }
        /* bottom to top: primary (win1), overlay (win0), cursor (win2) */
        vop_get_win(r, 1, x0, y0, &win);
        vop_draw_win(s, &win, fb, w, h);
        vop_get_win(r, 0, x0, y0, &win);
        vop_draw_win(s, &win, fb, w, h);
        vop_get_win(r, 2, x0, y0, &win);
        vop_draw_win(s, &win, fb, w, h);
    }

    /* copy to the console surface (32 bpp xRGB) with rotation */
    {
        uint8_t *dst = surface_data(surface);
        int stride = surface_stride(surface);

        for (int y = 0; y < oh; y++) {
            uint32_t *drow = (uint32_t *)(dst + y * stride);

            for (int x = 0; x < ow; x++) {
                int sx, sy;

                switch (s->rotation) {
                case 90:        /* clockwise */
                    sx = y;
                    sy = h - 1 - x;
                    break;
                case 180:
                    sx = w - 1 - x;
                    sy = h - 1 - y;
                    break;
                case 270:
                    sx = w - 1 - y;
                    sy = x;
                    break;
                default:
                    sx = x;
                    sy = y;
                }
                drow[x] = fb[sy * w + sx];
            }
        }
    }
    qemu_console_update(s->con, 0, 0, ow, oh);
    return true;
}

static void vop_invalidate_display(void *opaque)
{
}

static const GraphicHwOps vop_gfx_ops = {
    .invalidate = vop_invalidate_display,
    .gfx_update = vop_update_display,
};

static void vop_frame(void *opaque)
{
    RockchipVOPState *s = opaque;
    bool standby = s->regs[VOP_SYS_CTRL2 >> 2] & SYS_CTRL2_STANDBY;

    if (s->cfg_pending) {
        memcpy(s->active, s->regs, sizeof(s->active));
        s->cfg_pending = false;
    }
    /* standby takes effect immediately in the active copy */
    s->active[VOP_SYS_CTRL2 >> 2] = s->regs[VOP_SYS_CTRL2 >> 2];

    if (standby) {
        if (!s->was_standby) {
            vop_raise(s, INTR_DSP_HOLD_VALID);
        }
        s->was_standby = true;
    } else {
        s->was_standby = false;
        if (vop_output_enabled(s, s->active)) {
            s->frame_count++;
            vop_raise(s, INTR_FS | INTR_FS_NEW | INTR_LINE_FLAG0 |
                         INTR_LINE_FLAG1);
        }
    }
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VOP_FRAME_NS);
}

static uint64_t vop_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipVOPState *s = opaque;

    switch (offset) {
    case VOP_VERSION:
        return VOP_VERSION_VAL;
    case VOP_INTR_EN:
        return s->intr_en;
    case VOP_INTR_CLEAR:
        return 0;
    case VOP_INTR_STATUS:
        return (s->intr_raw & s->intr_en & 0xffff) |
               ((s->intr_raw & 0xffff) << 16);
    case VOP_REG_CFG_DONE:
        return 0;
    }
    return s->regs[offset >> 2];
}

static void vop_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RockchipVOPState *s = opaque;
    uint32_t mask;

    switch (offset) {
    case VOP_VERSION:
    case VOP_INTR_STATUS:
        return;
    case VOP_INTR_EN:
        mask = value >> 16;
        s->intr_en = (s->intr_en & ~mask) | (value & mask);
        vop_update_irq(s);
        return;
    case VOP_INTR_CLEAR:
        mask = value >> 16;
        s->intr_raw &= ~(value & mask);
        vop_update_irq(s);
        return;
    case VOP_REG_CFG_DONE:
        if (value & 1) {
            s->cfg_pending = true;
            /* when no frame is running the update is immediate */
            if (!vop_output_enabled(s, s->active)) {
                memcpy(s->active, s->regs, sizeof(s->active));
                s->cfg_pending = false;
            }
        }
        return;
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps vop_ops = {
    .read = vop_read,
    .write = vop_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* AXI slave: wider accesses (e.g. memcpy of the register file) work */
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void vop_reset_hold(Object *obj, ResetType type)
{
    RockchipVOPState *s = ROCKCHIP_VOP(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[VOP_SYS_CTRL2 >> 2] = SYS_CTRL2_STANDBY;
    memcpy(s->active, s->regs, sizeof(s->active));
    s->intr_raw = 0;
    s->intr_en = 0;
    s->cfg_pending = false;
    s->was_standby = true;
    s->frame_count = 0;
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VOP_FRAME_NS);
}

static void vop_realize(DeviceState *dev, Error **errp)
{
    RockchipVOPState *s = ROCKCHIP_VOP(dev);

    if (s->rotation % 90 || s->rotation >= 360) {
        error_setg(errp, "rockchip-vop: rotation must be 0, 90, 180 or 270");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr ? s->dma_mr : get_system_memory(),
                       "rockchip-vop-dma");
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vop_frame, s);
    s->con = qemu_graphic_console_create(dev, 0, &vop_gfx_ops, s);
    qemu_console_resize(s->con,
                        (s->rotation % 180) ? 480 : 320,
                        (s->rotation % 180) ? 320 : 480);
}

static void vop_init(Object *obj)
{
    RockchipVOPState *s = ROCKCHIP_VOP(obj);

    memory_region_init_io(&s->iomem, obj, &vop_ops, s, TYPE_ROCKCHIP_VOP,
                          ROCKCHIP_VOP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property vop_props[] = {
    DEFINE_PROP_LINK("dma-memory", RockchipVOPState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_LINK("iommu", RockchipVOPState, iommu, TYPE_ROCKCHIP_IOMMU,
                     struct RockchipIOMMUState *),
    DEFINE_PROP_LINK("dsi", RockchipVOPState, dsi, TYPE_DW_MIPI_DSI,
                     struct DWMipiDsiState *),
    DEFINE_PROP_UINT32("rotation", RockchipVOPState, rotation, 0),
};

static const VMStateDescription vmstate_vop = {
    .name = TYPE_ROCKCHIP_VOP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RockchipVOPState, ROCKCHIP_VOP_NREGS),
        VMSTATE_UINT32_ARRAY(active, RockchipVOPState, ROCKCHIP_VOP_NREGS),
        VMSTATE_UINT32(intr_raw, RockchipVOPState),
        VMSTATE_UINT32(intr_en, RockchipVOPState),
        VMSTATE_BOOL(cfg_pending, RockchipVOPState),
        VMSTATE_BOOL(was_standby, RockchipVOPState),
        VMSTATE_UINT64(frame_count, RockchipVOPState),
        VMSTATE_TIMER_PTR(frame_timer, RockchipVOPState),
        VMSTATE_END_OF_LIST()
    }
};

static void vop_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = vop_realize;
    dc->vmsd = &vmstate_vop;
    device_class_set_props(dc, vop_props);
    rc->phases.hold = vop_reset_hold;
}

static const TypeInfo vop_info = {
    .name = TYPE_ROCKCHIP_VOP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipVOPState),
    .instance_init = vop_init,
    .class_init = vop_class_init,
};

static void vop_register_types(void)
{
    type_register_static(&vop_info);
}

type_init(vop_register_types)
