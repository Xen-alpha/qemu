/*
 * Synopsys DesignWare MIPI DSI host controller (Rockchip variant) with
 * an attached DCS panel
 *
 * The host controller is a register file with the status behaviour that
 * drivers/gpu/drm/rockchip/dw-mipi-dsi.c and the PX30 inno video combo
 * PHY driver poll for (PHY lock, lane stop states, command FIFO
 * status).  Generic/DCS packets written through GEN_HDR/GEN_PLD_DATA are
 * decoded and delivered to a simple panel model which tracks the
 * MIPI DCS sleep and display on/off state (and MADCTL/COLMOD), so the
 * picture only appears once the guest has initialised the panel the same
 * way as on the real hardware.  The panel RESET line is a GPIO input
 * (active low, GPIO3_C0 on the ODROID-GO Advance).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/display/dw_mipi_dsi.h"
#include "migration/vmstate.h"

#define DSI_VERSION         0x00
#define DSI_PWR_UP          0x04
#define DSI_MODE_CFG        0x34
#define DSI_GEN_HDR         0x6c
#define DSI_GEN_PLD_DATA    0x70
#define DSI_CMD_PKT_STATUS  0x74
#define DSI_PHY_RSTZ        0xa0
#define DSI_PHY_STATUS      0xb0
#define DSI_PHY_TST_CTRL0   0xb4
#define DSI_PHY_TST_CTRL1   0xb8
#define DSI_INT_ST0         0xbc
#define DSI_INT_ST1         0xc0
#define DSI_INT_MSK0        0xc4
#define DSI_INT_MSK1        0xc8

#define GEN_CMD_EMPTY       (1u << 0)
#define GEN_PLD_W_EMPTY     (1u << 2)
#define GEN_PLD_R_EMPTY     (1u << 4)

#define PHY_STATUS_READY    ((1u << 0) |                         \
                             (1u << 2) | (1u << 3) |             \
                             (1u << 4) | (1u << 5) |             \
                             (1u << 7) | (1u << 8) |             \
                             (1u << 9) | (1u << 10) |            \
                             (1u << 11) | (1u << 12))

#define DSI_VERSION_VAL     0x3133302a      /* 1.30a */

/* MIPI DSI data types */
#define DT_GEN_SHORT_W0     0x03
#define DT_GEN_SHORT_W1     0x13
#define DT_GEN_SHORT_W2     0x23
#define DT_GEN_READ0        0x04
#define DT_GEN_READ1        0x14
#define DT_GEN_READ2        0x24
#define DT_DCS_SHORT_W0     0x05
#define DT_DCS_SHORT_W1     0x15
#define DT_DCS_READ         0x06
#define DT_SET_MAX_RET      0x37
#define DT_GEN_LONG_W       0x29
#define DT_DCS_LONG_W       0x39

/* MIPI DCS commands */
#define DCS_SOFT_RESET      0x01
#define DCS_GET_POWER_MODE  0x0a
#define DCS_ENTER_SLEEP     0x10
#define DCS_EXIT_SLEEP      0x11
#define DCS_DISPLAY_OFF     0x28
#define DCS_DISPLAY_ON      0x29
#define DCS_SET_ADDR_MODE   0x36
#define DCS_SET_PIXEL_FMT   0x3a
#define DCS_READ_ID1        0xda

bool dw_mipi_dsi_video_active(DWMipiDsiState *s)
{
    return (s->regs[DSI_PWR_UP >> 2] & 1) &&
           !(s->regs[DSI_MODE_CFG >> 2] & 1) &&     /* video mode */
           !s->panel_reset && !s->panel_sleep && s->panel_on;
}

static void dsi_panel_reset(DWMipiDsiState *s)
{
    s->panel_sleep = true;
    s->panel_on = false;
    s->panel_madctl = 0;
    s->panel_colmod = 0x66;
}

static void dsi_rd_push(DWMipiDsiState *s, const uint8_t *data, int len)
{
    s->rd_count = 0;
    memset(s->rd_fifo, 0, sizeof(s->rd_fifo));
    for (int i = 0; i < len && i < 16; i++) {
        s->rd_fifo[i / 4] |= (uint32_t)data[i] << (8 * (i % 4));
        s->rd_count = i / 4 + 1;
    }
}

static void dsi_panel_dcs(DWMipiDsiState *s, const uint8_t *data, int len,
                          bool read)
{
    uint8_t cmd = data[0];
    uint8_t rsp[4] = { 0 };

    if (s->panel_reset) {
        return;
    }
    s->panel_cmds++;
    switch (cmd) {
    case DCS_SOFT_RESET:
        dsi_panel_reset(s);
        break;
    case DCS_ENTER_SLEEP:
        s->panel_sleep = true;
        break;
    case DCS_EXIT_SLEEP:
        s->panel_sleep = false;
        break;
    case DCS_DISPLAY_OFF:
        s->panel_on = false;
        break;
    case DCS_DISPLAY_ON:
        s->panel_on = true;
        break;
    case DCS_SET_ADDR_MODE:
        if (len > 1) {
            s->panel_madctl = data[1];
        }
        break;
    case DCS_SET_PIXEL_FMT:
        if (len > 1) {
            s->panel_colmod = data[1];
        }
        break;
    }
    if (read) {
        switch (cmd) {
        case DCS_GET_POWER_MODE:
            rsp[0] = 0x08 | (s->panel_sleep ? 0 : 0x10) |
                     (s->panel_on ? 0x04 : 0) | 0x80;
            break;
        case DCS_READ_ID1:
            rsp[0] = 0x00;
            break;
        }
        dsi_rd_push(s, rsp, 1);
    }
}

static void dsi_gen_hdr(DWMipiDsiState *s, uint32_t hdr)
{
    uint8_t dt = hdr & 0x3f;
    uint8_t d0 = (hdr >> 8) & 0xff;
    uint8_t d1 = (hdr >> 16) & 0xff;
    uint8_t buf[2] = { d0, d1 };
    uint32_t wc = d0 | (d1 << 8);

    switch (dt) {
    case DT_DCS_SHORT_W0:
        dsi_panel_dcs(s, buf, 1, false);
        break;
    case DT_DCS_SHORT_W1:
        dsi_panel_dcs(s, buf, 2, false);
        break;
    case DT_DCS_READ:
        dsi_panel_dcs(s, buf, 1, true);
        break;
    case DT_GEN_SHORT_W0:
    case DT_GEN_SHORT_W1:
    case DT_GEN_SHORT_W2:
        /* manufacturer commands sent as generic writes */
        if (dt != DT_GEN_SHORT_W0) {
            dsi_panel_dcs(s, buf, dt == DT_GEN_SHORT_W1 ? 1 : 2, false);
        }
        break;
    case DT_GEN_READ0:
    case DT_GEN_READ1:
    case DT_GEN_READ2:
        dsi_panel_dcs(s, buf, 1, true);
        break;
    case DT_GEN_LONG_W:
    case DT_DCS_LONG_W:
        if (wc > s->pld_len) {
            qemu_log_mask(LOG_GUEST_ERROR, "dw-mipi-dsi: long packet wc %u "
                          "> payload %u\n", wc, s->pld_len);
            wc = s->pld_len;
        }
        if (wc) {
            dsi_panel_dcs(s, s->pld, wc, false);
        }
        s->pld_len = 0;
        break;
    case DT_SET_MAX_RET:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "dw-mipi-dsi: data type 0x%02x\n", dt);
    }
}

static uint64_t dsi_read(void *opaque, hwaddr offset, unsigned size)
{
    DWMipiDsiState *s = opaque;
    uint32_t val;

    switch (offset) {
    case DSI_VERSION:
        return DSI_VERSION_VAL;
    case DSI_CMD_PKT_STATUS:
        val = GEN_CMD_EMPTY | GEN_PLD_W_EMPTY;
        if (!s->rd_count) {
            val |= GEN_PLD_R_EMPTY;
        }
        return val;
    case DSI_GEN_PLD_DATA:
        if (s->rd_count) {
            val = s->rd_fifo[0];
            memmove(&s->rd_fifo[0], &s->rd_fifo[1], 3 * sizeof(uint32_t));
            s->rd_fifo[3] = 0;
            s->rd_count--;
            return val;
        }
        return 0;
    case DSI_PHY_STATUS:
        return PHY_STATUS_READY;
    case DSI_INT_ST0:
    case DSI_INT_ST1:
        /* clear on read, no errors are ever reported */
        return 0;
    }
    return s->regs[offset >> 2];
}

static void dsi_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    DWMipiDsiState *s = opaque;

    switch (offset) {
    case DSI_VERSION:
    case DSI_CMD_PKT_STATUS:
    case DSI_PHY_STATUS:
    case DSI_INT_ST0:
    case DSI_INT_ST1:
        return;
    case DSI_GEN_PLD_DATA:
        if (s->pld_len + 4 <= sizeof(s->pld)) {
            stl_le_p(&s->pld[s->pld_len], value);
            s->pld_len += 4;
        }
        return;
    case DSI_GEN_HDR:
        s->regs[offset >> 2] = value;
        dsi_gen_hdr(s, value);
        return;
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps dsi_ops = {
    .read = dsi_read,
    .write = dsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dsi_set_panel_reset(void *opaque, int n, int level)
{
    DWMipiDsiState *s = opaque;

    /* active low */
    s->panel_reset = !level;
    if (s->panel_reset) {
        dsi_panel_reset(s);
    }
}

static void dsi_reset_hold(Object *obj, ResetType type)
{
    DWMipiDsiState *s = DW_MIPI_DSI(obj);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[DSI_MODE_CFG >> 2] = 1;     /* command mode */
    s->pld_len = 0;
    s->rd_count = 0;
    dsi_panel_reset(s);
}

static void dsi_init(Object *obj)
{
    DWMipiDsiState *s = DW_MIPI_DSI(obj);

    memory_region_init_io(&s->iomem, obj, &dsi_ops, s, TYPE_DW_MIPI_DSI,
                          0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), dsi_set_panel_reset,
                            "panel-reset", 1);
    s->panel_reset = false;
}

static const VMStateDescription vmstate_dsi = {
    .name = TYPE_DW_MIPI_DSI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, DWMipiDsiState, DW_MIPI_DSI_NREGS),
        VMSTATE_UINT8_ARRAY(pld, DWMipiDsiState, DW_MIPI_DSI_PLD_MAX * 4),
        VMSTATE_UINT32(pld_len, DWMipiDsiState),
        VMSTATE_UINT32_ARRAY(rd_fifo, DWMipiDsiState, 4),
        VMSTATE_UINT32(rd_count, DWMipiDsiState),
        VMSTATE_BOOL(panel_reset, DWMipiDsiState),
        VMSTATE_BOOL(panel_sleep, DWMipiDsiState),
        VMSTATE_BOOL(panel_on, DWMipiDsiState),
        VMSTATE_UINT8(panel_madctl, DWMipiDsiState),
        VMSTATE_UINT8(panel_colmod, DWMipiDsiState),
        VMSTATE_END_OF_LIST()
    }
};

static void dsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_dsi;
    rc->phases.hold = dsi_reset_hold;
}

static const TypeInfo dsi_info = {
    .name = TYPE_DW_MIPI_DSI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWMipiDsiState),
    .instance_init = dsi_init,
    .class_init = dsi_class_init,
};

static void dsi_register_types(void)
{
    type_register_static(&dsi_info);
}

type_init(dsi_register_types)
