/*
 * Rockchip SARADC (v1, rk3399-saradc compatible) model
 *
 * 10-bit successive approximation ADC, 6 channels, single-shot.
 *   0x00 DATA        conversion result
 *   0x04 STAS        [0] busy
 *   0x08 CTRL        [2:0] channel, [3] power, [5] irq enable,
 *                    [6] irq status (write 0 to clear)
 *   0x0c DLY_PU_SOC  power-up delay
 *
 * Setting CTRL.power starts a conversion of the selected channel which
 * completes after ~13 cycles of the converter clock.
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
#include "hw/core/qdev-properties.h"
#include "hw/adc/rockchip_saradc.h"
#include "migration/vmstate.h"

#define SARADC_DATA         0x00
#define SARADC_STAS         0x04
#define SARADC_CTRL         0x08
#define SARADC_DLY_PU_SOC   0x0c

#define CTRL_CHN_MASK       0x7
#define CTRL_POWER          (1u << 3)
#define CTRL_IRQ_ENABLE     (1u << 5)
#define CTRL_IRQ_STATUS     (1u << 6)

#define SARADC_CONV_NS      (2 * SCALE_US)

static void rk_saradc_update_irq(RockchipSARADCState *s)
{
    qemu_set_irq(s->irq, (s->ctrl & CTRL_IRQ_ENABLE) &&
                         (s->ctrl & CTRL_IRQ_STATUS));
}

static void rk_saradc_done(void *opaque)
{
    RockchipSARADCState *s = opaque;
    unsigned ch = s->ctrl & CTRL_CHN_MASK;

    s->busy = false;
    s->data = ch < ROCKCHIP_SARADC_CHANNELS ? (s->input[ch] & 0x3ff) : 0;
    s->ctrl |= CTRL_IRQ_STATUS;
    rk_saradc_update_irq(s);
}

static uint64_t rk_saradc_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipSARADCState *s = opaque;

    switch (offset) {
    case SARADC_DATA:
        return s->data;
    case SARADC_STAS:
        return s->busy;
    case SARADC_CTRL:
        return s->ctrl;
    case SARADC_DLY_PU_SOC:
        return s->dly_pu_soc;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rockchip-saradc: bad read 0x%"
                  HWADDR_PRIx "\n", offset);
    return 0;
}

static void rk_saradc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    RockchipSARADCState *s = opaque;
    uint32_t old;

    switch (offset) {
    case SARADC_CTRL:
        old = s->ctrl;
        s->ctrl = value & (CTRL_CHN_MASK | CTRL_POWER | CTRL_IRQ_ENABLE |
                           CTRL_IRQ_STATUS);
        /* the status bit can only be cleared by software */
        if (!(old & CTRL_IRQ_STATUS)) {
            s->ctrl &= ~CTRL_IRQ_STATUS;
        }
        if (!(s->ctrl & CTRL_POWER)) {
            timer_del(s->timer);
            s->busy = false;
        } else if (!(old & CTRL_POWER)) {
            s->busy = true;
            timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      SARADC_CONV_NS);
        }
        rk_saradc_update_irq(s);
        break;
    case SARADC_DLY_PU_SOC:
        s->dly_pu_soc = value & 0x3f;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-saradc: bad write 0x%"
                      HWADDR_PRIx "\n", offset);
    }
}

static const MemoryRegionOps rk_saradc_ops = {
    .read = rk_saradc_read,
    .write = rk_saradc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

void rockchip_saradc_set_input(RockchipSARADCState *s, int ch, uint16_t val)
{
    assert(ch >= 0 && ch < ROCKCHIP_SARADC_CHANNELS);
    s->input[ch] = MIN(val, 0x3ff);
}

static void rk_saradc_get_ch(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    uint16_t *p = opaque;

    visit_type_uint16(v, name, p, errp);
}

static void rk_saradc_set_ch(Object *obj, Visitor *v, const char *name,
                             void *opaque, Error **errp)
{
    uint16_t *p = opaque;
    uint16_t val;

    if (!visit_type_uint16(v, name, &val, errp)) {
        return;
    }
    if (val > 0x3ff) {
        error_setg(errp, "ADC code %u out of range (0..1023)", val);
        return;
    }
    *p = val;
}

static void rk_saradc_reset_hold(Object *obj, ResetType type)
{
    RockchipSARADCState *s = ROCKCHIP_SARADC(obj);

    timer_del(s->timer);
    s->data = 0;
    s->ctrl = 0;
    s->dly_pu_soc = 0;
    s->busy = false;
}

static void rk_saradc_init(Object *obj)
{
    RockchipSARADCState *s = ROCKCHIP_SARADC(obj);

    memory_region_init_io(&s->iomem, obj, &rk_saradc_ops, s,
                          TYPE_ROCKCHIP_SARADC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rk_saradc_done, s);

    for (int i = 0; i < ROCKCHIP_SARADC_CHANNELS; i++) {
        g_autofree char *name = g_strdup_printf("channel%d", i);
        object_property_add(obj, name, "uint16", &uint16_type_info,
                            rk_saradc_get_ch,
                            rk_saradc_set_ch, NULL, &s->input[i]);
    }
}

static const VMStateDescription vmstate_rk_saradc = {
    .name = TYPE_ROCKCHIP_SARADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(input, RockchipSARADCState,
                             ROCKCHIP_SARADC_CHANNELS),
        VMSTATE_UINT32(data, RockchipSARADCState),
        VMSTATE_UINT32(ctrl, RockchipSARADCState),
        VMSTATE_UINT32(dly_pu_soc, RockchipSARADCState),
        VMSTATE_BOOL(busy, RockchipSARADCState),
        VMSTATE_TIMER_PTR(timer, RockchipSARADCState),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_saradc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rk_saradc;
    rc->phases.hold = rk_saradc_reset_hold;
}

static const TypeInfo rk_saradc_info = {
    .name = TYPE_ROCKCHIP_SARADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipSARADCState),
    .instance_init = rk_saradc_init,
    .class_init = rk_saradc_class_init,
};

static void rk_saradc_register_types(void)
{
    type_register_static(&rk_saradc_info);
}

type_init(rk_saradc_register_types)
