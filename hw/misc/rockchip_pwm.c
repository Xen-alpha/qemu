/*
 * Rockchip PWM v2 (rk3328-pwm / px30-pwm) model
 *
 * One instance covers a block of four channels (PX30: pwm0-3 at
 * 0xff200000, pwm4-7 at 0xff208000).  Per channel (stride 0x10):
 *   0x0 CNT          counter (read-only)
 *   0x4 PERIOD_HPR   period / high period (capture)
 *   0x8 DUTY_LPR     duty / low period (capture)
 *   0xc CTRL         [0] enable, [2:1] mode, [3] duty polarity,
 *                    [4] inactive polarity, [5] output mode, [6] conf lock
 * Shared: 0x40 INTSTS, 0x44 INT_EN.
 *
 * The output waveform is not generated; the configured level is
 * available to the board (backlight).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/misc/rockchip_pwm.h"
#include "migration/vmstate.h"

#define PWM_CNT         0x0
#define PWM_PERIOD      0x4
#define PWM_DUTY        0x8
#define PWM_CTRL        0xc
#define PWM_INTSTS      0x40
#define PWM_INT_EN      0x44

#define CTRL_ENABLE     (1u << 0)
#define CTRL_MODE_MASK  (3u << 1)
#define CTRL_MODE_CONT  (1u << 1)
#define CTRL_DUTY_POS   (1u << 3)
#define CTRL_LOCK       (1u << 6)

unsigned rockchip_pwm_get_level(RockchipPWMState *s, int ch)
{
    RockchipPWMChannel *c = &s->ch[ch];
    unsigned level;

    if (!(c->ctrl & CTRL_ENABLE) || !c->period) {
        /* inactive polarity bit set means the output idles high */
        return (c->ctrl & (1u << 4)) ? 1000 : 0;
    }
    level = MIN((uint64_t)c->duty * 1000 / c->period, 1000);
    return (c->ctrl & CTRL_DUTY_POS) ? level : 1000 - level;
}

static uint64_t rk_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipPWMState *s = opaque;

    if (offset < 0x40) {
        RockchipPWMChannel *c = &s->ch[offset >> 4];

        switch (offset & 0xf) {
        case PWM_CNT:
            return 0;
        case PWM_PERIOD:
            return c->period;
        case PWM_DUTY:
            return c->duty;
        case PWM_CTRL:
            return c->ctrl;
        }
    }
    switch (offset) {
    case PWM_INTSTS:
        return s->intsts;
    case PWM_INT_EN:
        return s->int_en;
    }
    return 0;
}

static void rk_pwm_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RockchipPWMState *s = opaque;

    if (offset < 0x40) {
        RockchipPWMChannel *c = &s->ch[offset >> 4];

        switch (offset & 0xf) {
        case PWM_PERIOD:
            c->period = value;
            break;
        case PWM_DUTY:
            c->duty = value;
            break;
        case PWM_CTRL:
            c->ctrl = value;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "rockchip-pwm: write to RO reg 0x%"
                          HWADDR_PRIx "\n", offset);
        }
        return;
    }
    switch (offset) {
    case PWM_INTSTS:
        s->intsts &= ~value;
        break;
    case PWM_INT_EN:
        s->int_en = value;
        break;
    }
    qemu_set_irq(s->irq, !!(s->intsts & s->int_en));
}

static const MemoryRegionOps rk_pwm_ops = {
    .read = rk_pwm_read,
    .write = rk_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk_pwm_reset_hold(Object *obj, ResetType type)
{
    RockchipPWMState *s = ROCKCHIP_PWM(obj);

    for (int i = 0; i < ROCKCHIP_PWM_CHANNELS; i++) {
        s->ch[i].period = 0;
        s->ch[i].duty = 0;
        s->ch[i].ctrl = 0;
    }
    s->intsts = 0;
    s->int_en = 0;
}

static void rk_pwm_init(Object *obj)
{
    RockchipPWMState *s = ROCKCHIP_PWM(obj);

    memory_region_init_io(&s->iomem, obj, &rk_pwm_ops, s, TYPE_ROCKCHIP_PWM,
                          0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_rk_pwm_ch = {
    .name = "rockchip-pwm-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(period, RockchipPWMChannel),
        VMSTATE_UINT32(duty, RockchipPWMChannel),
        VMSTATE_UINT32(ctrl, RockchipPWMChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_rk_pwm = {
    .name = TYPE_ROCKCHIP_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, RockchipPWMState, ROCKCHIP_PWM_CHANNELS, 1,
                             vmstate_rk_pwm_ch, RockchipPWMChannel),
        VMSTATE_UINT32(intsts, RockchipPWMState),
        VMSTATE_UINT32(int_en, RockchipPWMState),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rk_pwm;
    rc->phases.hold = rk_pwm_reset_hold;
}

static const TypeInfo rk_pwm_info = {
    .name = TYPE_ROCKCHIP_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipPWMState),
    .instance_init = rk_pwm_init,
    .class_init = rk_pwm_class_init,
};

static void rk_pwm_register_types(void)
{
    type_register_static(&rk_pwm_info);
}

type_init(rk_pwm_register_types)
