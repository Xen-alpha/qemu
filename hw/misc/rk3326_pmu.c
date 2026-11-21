/*
 * Rockchip RK3326/PX30 Power Management Unit
 *
 * Models the power domain and NIU (bus idle) handshake registers used by
 * drivers/soc/rockchip/pm_domains.c (px30_pmu):
 *
 *   0x18 PMU_PWRDN_CON   hiword, bit n = power domain n off request
 *   0x20 PMU_PWRDN_ST    read-only, bit n = power domain n is off
 *   0x64 PMU_BUS_IDLE_REQ hiword, bit n = idle request for NIU n
 *   0x6c PMU_BUS_IDLE_ST read-only, [15:0] ack, [31:16] idle
 *
 * Power domains switch instantly.  Everything else is plain storage;
 * system suspend (handled by EL3 firmware on real hardware) is not
 * modelled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/rk3326_pmu.h"
#include "migration/vmstate.h"

#define PMU_WAKEUP_CFG0         0x00
#define PMU_PWRDN_CON           0x18
#define PMU_PWRDN_ST            0x20
#define PMU_PWRMODE_CORE_CON    0x24
#define PMU_PWRMODE_COMMON_CON  0x28
#define PMU_BUS_IDLE_REQ        0x64
#define PMU_BUS_IDLE_ST         0x6c

static bool rk_pmu_is_hiword(hwaddr offset)
{
    switch (offset) {
    case PMU_PWRDN_CON:
    case PMU_BUS_IDLE_REQ:
    case PMU_PWRMODE_CORE_CON:
    case PMU_PWRMODE_COMMON_CON:
        return true;
    }
    return false;
}

static uint64_t rk_pmu_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3326PMUState *s = opaque;
    uint32_t req;

    switch (offset) {
    case PMU_PWRDN_ST:
        return s->regs[PMU_PWRDN_CON >> 2] & 0xffff;
    case PMU_BUS_IDLE_ST:
        req = s->regs[PMU_BUS_IDLE_REQ >> 2] & 0xffff;
        return req | (req << 16);
    }
    return s->regs[offset >> 2];
}

static void rk_pmu_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RK3326PMUState *s = opaque;
    unsigned idx = offset >> 2;
    uint32_t mask;

    switch (offset) {
    case PMU_PWRDN_ST:
    case PMU_BUS_IDLE_ST:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rk3326-pmu: write to read-only reg 0x%" HWADDR_PRIx
                      "\n", offset);
        return;
    }
    if (rk_pmu_is_hiword(offset)) {
        mask = (value >> 16) & 0xffff;
        s->regs[idx] = (s->regs[idx] & ~mask) | (value & mask);
    } else {
        s->regs[idx] = value;
    }
}

static const MemoryRegionOps rk_pmu_ops = {
    .read = rk_pmu_read,
    .write = rk_pmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk_pmu_reset_hold(Object *obj, ResetType type)
{
    RK3326PMUState *s = RK3326_PMU(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rk_pmu_init(Object *obj)
{
    RK3326PMUState *s = RK3326_PMU(obj);

    memory_region_init_io(&s->iomem, obj, &rk_pmu_ops, s, TYPE_RK3326_PMU,
                          RK3326_PMU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_rk_pmu = {
    .name = TYPE_RK3326_PMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3326PMUState, RK3326_PMU_NREGS),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_pmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rk_pmu;
    rc->phases.hold = rk_pmu_reset_hold;
}

static const TypeInfo rk_pmu_info = {
    .name = TYPE_RK3326_PMU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3326PMUState),
    .instance_init = rk_pmu_init,
    .class_init = rk_pmu_class_init,
};

static void rk_pmu_register_types(void)
{
    type_register_static(&rk_pmu_info);
}

type_init(rk_pmu_register_types)
