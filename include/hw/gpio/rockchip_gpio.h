/*
 * Rockchip GPIO bank (v1, DesignWare APB GPIO derived) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_ROCKCHIP_GPIO_H
#define HW_GPIO_ROCKCHIP_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_GPIO "rockchip-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipGPIOState, ROCKCHIP_GPIO)

#define ROCKCHIP_GPIO_PINS 32

struct RockchipGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq out[ROCKCHIP_GPIO_PINS];

    /* external pin state, not affected by reset */
    uint32_t in_level;
    uint32_t in_default;    /* property: pin level when nothing drives it */

    uint32_t dr;
    uint32_t ddr;
    uint32_t inten;
    uint32_t intmask;
    uint32_t inttype;       /* 1 = edge */
    uint32_t intpol;        /* 1 = high / rising */
    uint32_t raw_edge;      /* latched edge interrupts */
    uint32_t debounce;
    uint32_t ls_sync;
    uint32_t last_level;    /* for edge detection */
    uint32_t out_level;     /* last value driven on out[] */
};

#endif
