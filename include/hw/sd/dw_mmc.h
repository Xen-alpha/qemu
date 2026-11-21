/*
 * Synopsys DesignWare Mobile Storage Host Controller (dw-mshc) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_DW_MMC_H
#define HW_SD_DW_MMC_H

#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_DW_MMC "dw-mshc"
OBJECT_DECLARE_SIMPLE_TYPE(DWMMCState, DW_MMC)

#define TYPE_DW_MMC_BUS "dw-mshc-bus"

#define DW_MMC_REG_SIZE 0x200   /* registers below the FIFO window */

struct DWMMCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SDBus sdbus;
    qemu_irq irq;
    QEMUTimer *cmd_timer;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    /* properties */
    uint32_t fifo_depth;    /* in 32-bit words */
    uint32_t verid;

    uint32_t ctrl;
    uint32_t pwren;
    uint32_t clkdiv;
    uint32_t clksrc;
    uint32_t clkena;
    uint32_t tmout;
    uint32_t ctype;
    uint32_t blksiz;
    uint32_t bytcnt;
    uint32_t intmask;
    uint32_t cmdarg;
    uint32_t cmd;
    uint32_t resp[4];
    uint32_t rintsts;
    uint32_t fifoth;
    uint32_t debnce;
    uint32_t usrid;
    uint32_t uhs_reg;
    uint32_t rst_n;
    uint32_t bmod;
    uint32_t pldmnd;
    uint32_t dbaddr;
    uint32_t idsts;
    uint32_t idinten;
    uint32_t dscaddr;
    uint32_t bufaddr;
    uint32_t cardthrctl;
    uint32_t emmc_ddr;
    uint32_t enable_shift;
    uint32_t tcbcnt;        /* bytes transferred to the card */
    uint32_t tbbcnt;        /* bytes transferred via the FIFO */

    /* data transfer state (PIO) */
    bool data_active;
    bool data_write;
    uint32_t data_remaining;
};

#endif
