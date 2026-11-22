/*
 * Synopsys DesignWare APB watchdog timer model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_WATCHDOG_DW_WDT_H
#define HW_WATCHDOG_DW_WDT_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_DW_WDT "dw-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(DWWDTState, DW_WDT)

struct DWWDTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *ptimer;
    uint32_t freq;          /* property */

    uint32_t cr;
    uint32_t torr;
    uint32_t stat;
    bool started;
};

#endif
