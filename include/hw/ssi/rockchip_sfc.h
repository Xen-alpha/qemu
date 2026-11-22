/*
 * Rockchip Serial Flash Controller (SFC) with an attached SPI NOR flash
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_ROCKCHIP_SFC_H
#define HW_SSI_ROCKCHIP_SFC_H

#include "hw/core/sysbus.h"
#include "system/block-backend.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_SFC "rockchip-sfc"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipSFCState, ROCKCHIP_SFC)

#define ROCKCHIP_SFC_FIFO_WORDS 16
#define ROCKCHIP_SFC_MAX_XFER   (16 * 1024)

struct RockchipSFCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;

    /* flash */
    BlockBackend *blk;
    uint8_t *storage;
    uint32_t flash_size;    /* property */
    uint32_t jedec_id;      /* property */
    uint8_t sr[3];
    bool wel;

    /* controller registers */
    uint32_t ctrl;
    uint32_t imr;
    uint32_t ftlr;
    uint32_t ax;
    uint32_t abit;
    uint32_t rawisr;
    uint32_t qop;
    uint32_t dma_addr;
    uint32_t cmd;
    uint32_t addr;

    /* current transfer */
    bool cmd_pending;       /* waiting for ADDR or write data */
    uint8_t buf[ROCKCHIP_SFC_MAX_XFER];
    uint32_t len;           /* datasize of the transfer */
    uint32_t pos;           /* bytes moved through the FIFO */
    bool rx_active;
    bool tx_active;
};

#endif
