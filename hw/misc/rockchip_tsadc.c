/*
 * Rockchip TSADC (px30-tsadc) model
 *
 * Two sensors (0: CPU, 1: GPU).  The conversion result follows the
 * rk3328/px30 code table (12-bit, code increases with temperature) of
 * drivers/thermal/rockchip_thermal.c.  Sensor temperatures are QOM
 * properties ("temperature0", "temperature1", milli-degree Celsius) and
 * can be changed at run time with qom-set to exercise thermal code.
 *
 * In auto mode the high temperature alarm interrupt and the thermal
 * shutdown (TSHUT to CRU = global reset) are evaluated whenever the
 * configuration or a temperature changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/misc/rockchip_tsadc.h"
#include "migration/vmstate.h"
#include "system/runstate.h"

#define TSADC_USER_CON          0x00
#define TSADC_AUTO_CON          0x04
#define TSADC_INT_EN            0x08
#define TSADC_INT_PD            0x0c
#define TSADC_DATA(ch)          (0x20 + (ch) * 4)
#define TSADC_COMP_INT(ch)      (0x30 + (ch) * 4)
#define TSADC_COMP_SHUT(ch)     (0x40 + (ch) * 4)

#define AUTO_EN                 (1u << 0)
#define AUTO_SRC_EN(ch)         (1u << (4 + (ch)))
#define INT_SRC_EN(ch)          (1u << (ch))
#define SHUT_2CRU_SRC_EN(ch)    (1u << (8 + (ch)))

static const struct {
    uint32_t code;
    int32_t temp;
} rk_tsadc_table[] = {
    { 296, -40000 }, { 304, -35000 }, { 313, -30000 }, { 331, -20000 },
    { 340, -15000 }, { 349, -10000 }, { 359, -5000 }, { 368, 0 },
    { 378, 5000 }, { 388, 10000 }, { 398, 15000 }, { 408, 20000 },
    { 418, 25000 }, { 429, 30000 }, { 440, 35000 }, { 451, 40000 },
    { 462, 45000 }, { 473, 50000 }, { 485, 55000 }, { 496, 60000 },
    { 508, 65000 }, { 521, 70000 }, { 533, 75000 }, { 546, 80000 },
    { 559, 85000 }, { 572, 90000 }, { 586, 95000 }, { 600, 100000 },
    { 614, 105000 }, { 629, 110000 }, { 644, 115000 }, { 659, 120000 },
    { 675, 125000 },
};

static uint32_t rk_tsadc_temp_to_code(int32_t temp)
{
    int n = ARRAY_SIZE(rk_tsadc_table);

    if (temp <= rk_tsadc_table[0].temp) {
        return rk_tsadc_table[0].code;
    }
    for (int i = 1; i < n; i++) {
        if (temp <= rk_tsadc_table[i].temp) {
            int32_t t0 = rk_tsadc_table[i - 1].temp;
            int32_t t1 = rk_tsadc_table[i].temp;
            uint32_t c0 = rk_tsadc_table[i - 1].code;
            uint32_t c1 = rk_tsadc_table[i].code;

            return c0 + (uint32_t)((int64_t)(temp - t0) * (c1 - c0) /
                                   (t1 - t0));
        }
    }
    return rk_tsadc_table[n - 1].code;
}

static void rk_tsadc_update(RockchipTSADCState *s)
{
    uint32_t auto_con = s->regs[TSADC_AUTO_CON >> 2];
    uint32_t int_en = s->regs[TSADC_INT_EN >> 2];
    bool shut = false;

    if (auto_con & AUTO_EN) {
        for (int ch = 0; ch < ROCKCHIP_TSADC_CHANNELS; ch++) {
            uint32_t code = rk_tsadc_temp_to_code(s->temp[ch]);

            if (!(auto_con & AUTO_SRC_EN(ch))) {
                continue;
            }
            if ((int_en & INT_SRC_EN(ch)) &&
                s->regs[TSADC_COMP_INT(ch) >> 2] &&
                code >= s->regs[TSADC_COMP_INT(ch) >> 2]) {
                s->regs[TSADC_INT_PD >> 2] |= INT_SRC_EN(ch);
            }
            if ((int_en & SHUT_2CRU_SRC_EN(ch)) &&
                s->regs[TSADC_COMP_SHUT(ch) >> 2] &&
                code >= s->regs[TSADC_COMP_SHUT(ch) >> 2]) {
                shut = true;
            }
        }
    }
    qemu_set_irq(s->irq, !!(s->regs[TSADC_INT_PD >> 2] & int_en & 0xf));
    if (shut) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rockchip-tsadc: over-temperature shutdown\n");
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t rk_tsadc_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipTSADCState *s = opaque;

    if (offset == TSADC_DATA(0) || offset == TSADC_DATA(1)) {
        return rk_tsadc_temp_to_code(s->temp[(offset - TSADC_DATA(0)) / 4]);
    }
    return s->regs[offset >> 2];
}

static void rk_tsadc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    RockchipTSADCState *s = opaque;

    switch (offset) {
    case TSADC_INT_PD:
        /* write 1 to clear the alarm status bits */
        s->regs[offset >> 2] &= ~(value & 0xff);
        break;
    case TSADC_DATA(0):
    case TSADC_DATA(1):
        return;
    default:
        s->regs[offset >> 2] = value;
    }
    rk_tsadc_update(s);
}

static const MemoryRegionOps rk_tsadc_ops = {
    .read = rk_tsadc_read,
    .write = rk_tsadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk_tsadc_get_temp(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    visit_type_int32(v, name, opaque, errp);
}

static void rk_tsadc_set_temp(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    RockchipTSADCState *s = ROCKCHIP_TSADC(obj);
    int32_t *p = opaque;
    int32_t val;

    if (!visit_type_int32(v, name, &val, errp)) {
        return;
    }
    *p = val;
    if (DEVICE(obj)->realized) {
        rk_tsadc_update(s);
    }
}

static void rk_tsadc_reset_hold(Object *obj, ResetType type)
{
    RockchipTSADCState *s = ROCKCHIP_TSADC(obj);

    memset(s->regs, 0, sizeof(s->regs));
}

static void rk_tsadc_init(Object *obj)
{
    RockchipTSADCState *s = ROCKCHIP_TSADC(obj);

    memory_region_init_io(&s->iomem, obj, &rk_tsadc_ops, s,
                          TYPE_ROCKCHIP_TSADC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    for (int i = 0; i < ROCKCHIP_TSADC_CHANNELS; i++) {
        g_autofree char *name = g_strdup_printf("temperature%d", i);

        s->temp[i] = 40000;
        object_property_add(obj, name, "int32", &int32_type_info,
                            rk_tsadc_get_temp,
                            rk_tsadc_set_temp, NULL, &s->temp[i]);
    }
}

static const VMStateDescription vmstate_rk_tsadc = {
    .name = TYPE_ROCKCHIP_TSADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32_ARRAY(temp, RockchipTSADCState,
                            ROCKCHIP_TSADC_CHANNELS),
        VMSTATE_UINT32_ARRAY(regs, RockchipTSADCState, ROCKCHIP_TSADC_NREGS),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_tsadc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rk_tsadc;
    rc->phases.hold = rk_tsadc_reset_hold;
}

static const TypeInfo rk_tsadc_info = {
    .name = TYPE_ROCKCHIP_TSADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipTSADCState),
    .instance_init = rk_tsadc_init,
    .class_init = rk_tsadc_class_init,
};

static void rk_tsadc_register_types(void)
{
    type_register_static(&rk_tsadc_info);
}

type_init(rk_tsadc_register_types)
