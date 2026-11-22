/*
 * Synopsys DesignWare APB watchdog timer model ("snps,dw-wdt")
 *
 *   0x00 WDT_CR    [0] enable (sticky), [1] response mode
 *                  (0: reset on timeout, 1: interrupt first, reset on
 *                  the second timeout)
 *   0x04 WDT_TORR  [3:0] timeout period: 2^(16 + TOP) clock cycles
 *   0x08 WDT_CCVR  current counter value
 *   0x0c WDT_CRR   write 0x76 to restart the counter
 *   0x10 WDT_STAT  interrupt status
 *   0x14 WDT_EOI   read to clear the interrupt
 *   0xf4 COMP_PARAMS_1, 0xf8 COMP_VERSION, 0xfc COMP_TYPE
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/watchdog/dw_wdt.h"
#include "migration/vmstate.h"
#include "system/watchdog.h"

#define WDT_CR          0x00
#define WDT_TORR        0x04
#define WDT_CCVR        0x08
#define WDT_CRR         0x0c
#define WDT_STAT        0x10
#define WDT_EOI         0x14
#define WDT_COMP_PARAMS_1 0xf4
#define WDT_COMP_VERSION  0xf8
#define WDT_COMP_TYPE     0xfc

#define CR_EN           (1u << 0)
#define CR_RMOD         (1u << 1)
#define CRR_RESTART     0x76

/* APB 32-bit, fixed timeout ranges, TOP_INIT/TOP reset values 0 */
#define COMP_PARAMS_1   ((2u << 8) | (1u << 6))
#define COMP_VERSION    0x3131302a      /* "110*" */
#define COMP_TYPE       0x44570120

static void dw_wdt_reload(DWWDTState *s)
{
    uint64_t count = 1ull << (16 + (s->torr & 0xf));

    ptimer_transaction_begin(s->ptimer);
    ptimer_set_limit(s->ptimer, count, 1);
    if (s->cr & CR_EN) {
        ptimer_run(s->ptimer, 0);
    }
    ptimer_transaction_commit(s->ptimer);
}

static void dw_wdt_expired(void *opaque)
{
    DWWDTState *s = opaque;

    if ((s->cr & CR_RMOD) && !s->stat) {
        s->stat = 1;
        qemu_set_irq(s->irq, 1);
        return;
    }
    qemu_log_mask(CPU_LOG_RESET, "dw-wdt: watchdog timeout, resetting\n");
    watchdog_perform_action();
}

static uint64_t dw_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    DWWDTState *s = opaque;

    switch (offset) {
    case WDT_CR:
        return s->cr;
    case WDT_TORR:
        return s->torr;
    case WDT_CCVR:
        return (s->cr & CR_EN) ? ptimer_get_count(s->ptimer) : 0;
    case WDT_STAT:
        return s->stat;
    case WDT_EOI:
        s->stat = 0;
        qemu_set_irq(s->irq, 0);
        return 0;
    case WDT_COMP_PARAMS_1:
        return COMP_PARAMS_1;
    case WDT_COMP_VERSION:
        return COMP_VERSION;
    case WDT_COMP_TYPE:
        return COMP_TYPE;
    }
    return 0;
}

static void dw_wdt_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    DWWDTState *s = opaque;

    switch (offset) {
    case WDT_CR:
        /* the enable bit can only be cleared by a system reset */
        s->cr = (value & 0x1f) | (s->cr & CR_EN);
        if ((s->cr & CR_EN) && !s->started) {
            s->started = true;
            dw_wdt_reload(s);
        }
        break;
    case WDT_TORR:
        s->torr = value & 0xff;
        break;
    case WDT_CRR:
        if ((value & 0xff) == CRR_RESTART) {
            s->stat = 0;
            qemu_set_irq(s->irq, 0);
            dw_wdt_reload(s);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dw-wdt: write to RO/bad offset 0x%"
                      HWADDR_PRIx "\n", offset);
    }
}

static const MemoryRegionOps dw_wdt_ops = {
    .read = dw_wdt_read,
    .write = dw_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_wdt_reset_hold(Object *obj, ResetType type)
{
    DWWDTState *s = DW_WDT(obj);

    ptimer_transaction_begin(s->ptimer);
    ptimer_stop(s->ptimer);
    ptimer_transaction_commit(s->ptimer);
    s->cr = 0;
    s->torr = 0;
    s->stat = 0;
    s->started = false;
    qemu_set_irq(s->irq, 0);
}

static void dw_wdt_realize(DeviceState *dev, Error **errp)
{
    DWWDTState *s = DW_WDT(dev);

    s->ptimer = ptimer_init(dw_wdt_expired, s,
                            PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                            PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
    ptimer_transaction_begin(s->ptimer);
    ptimer_set_freq(s->ptimer, s->freq);
    ptimer_transaction_commit(s->ptimer);
}

static void dw_wdt_init(Object *obj)
{
    DWWDTState *s = DW_WDT(obj);

    memory_region_init_io(&s->iomem, obj, &dw_wdt_ops, s, TYPE_DW_WDT, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property dw_wdt_props[] = {
    DEFINE_PROP_UINT32("clock-frequency", DWWDTState, freq, 100000000),
};

static const VMStateDescription vmstate_dw_wdt = {
    .name = TYPE_DW_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(ptimer, DWWDTState),
        VMSTATE_UINT32(cr, DWWDTState),
        VMSTATE_UINT32(torr, DWWDTState),
        VMSTATE_UINT32(stat, DWWDTState),
        VMSTATE_BOOL(started, DWWDTState),
        VMSTATE_END_OF_LIST()
    }
};

static void dw_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dw_wdt_realize;
    dc->vmsd = &vmstate_dw_wdt;
    device_class_set_props(dc, dw_wdt_props);
    rc->phases.hold = dw_wdt_reset_hold;
}

static const TypeInfo dw_wdt_info = {
    .name = TYPE_DW_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DWWDTState),
    .instance_init = dw_wdt_init,
    .class_init = dw_wdt_class_init,
};

static void dw_wdt_register_types(void)
{
    type_register_static(&dw_wdt_info);
}

type_init(dw_wdt_register_types)
