/*
 * Synopsys DesignWare MIPI DSI host controller (Rockchip variant) with
 * an attached DCS command mode panel (ODROID-GO Advance: Elida KD35T133,
 * ILI9486 class controller)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_DW_MIPI_DSI_H
#define HW_DISPLAY_DW_MIPI_DSI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_DW_MIPI_DSI "dw-mipi-dsi"
OBJECT_DECLARE_SIMPLE_TYPE(DWMipiDsiState, DW_MIPI_DSI)

#define DW_MIPI_DSI_NREGS   (0x100 / 4)
#define DW_MIPI_DSI_PLD_MAX 64

struct DWMipiDsiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[DW_MIPI_DSI_NREGS];

    /* generic payload FIFO (write) */
    uint8_t pld[DW_MIPI_DSI_PLD_MAX * 4];
    uint32_t pld_len;
    /* read response FIFO */
    uint32_t rd_fifo[4];
    uint32_t rd_count;

    /* panel state */
    bool panel_reset;       /* reset line asserted */
    bool panel_sleep;       /* in sleep mode */
    bool panel_on;          /* display on */
    uint8_t panel_madctl;
    uint8_t panel_colmod;
    uint32_t panel_cmds;    /* number of DCS commands received */
};

/* true when the panel is showing the video stream from the VOP */
bool dw_mipi_dsi_video_active(DWMipiDsiState *s);

#endif
