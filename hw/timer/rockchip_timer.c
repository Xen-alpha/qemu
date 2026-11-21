/*
 * Rockchip timer block (rk3288-timer style) model
 *
 * Each channel occupies 0x20 bytes:
 *   0x00 LOAD_COUNT0   load value [31:0]
 *   0x04 LOAD_COUNT1   load value [63:32]
 *   0x08 CURRENT_VALUE0
 *   0x0c CURRENT_VALUE1
 *   0x10 CONTROLREG    [0] enable, [1] mode (0 free running/periodic,
 *                      1 user defined count/one-shot), [2] int unmask
 *   0x18 INTSTATUS     [0] write 1 to clear
 *
 * The 64-bit counter counts down from LOAD_COUNT at the timer clock
 * (24 MHz xin24m on PX30/RK3326).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/rockchip_timer.h"
#include "migration/vmstate.h"

#define TIMER_LOAD_COUNT0   0x00
#define TIMER_LOAD_COUNT1   0x04
#define TIMER_CUR_VALUE0    0x08
#define TIMER_CUR_VALUE1    0x0c
#define TIMER_CONTROL       0x10
#define TIMER_INT_STATUS    0x18

#define CTRL_ENABLE         (1u << 0)
#define CTRL_USER_MODE      (1u << 1)
#define CTRL_INT_UNMASK     (1u << 2)

#define PTIMER_POLICY_RK    (PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD | \
                             PTIMER_POLICY_CONTINUOUS_TRIGGER | \
                             PTIMER_POLICY_NO_IMMEDIATE_TRIGGER | \
                             PTIMER_POLICY_NO_COUNTER_ROUND_DOWN)

static void rk_timer_update_irq(RockchipTimerChannel *c)
{
    qemu_set_irq(c->irq, (c->control & CTRL_INT_UNMASK) && c->int_status);
}

static void rk_timer_expired(void *opaque)
{
    RockchipTimerChannel *c = opaque;

    /* in user defined count (one-shot) mode the ptimer stops by itself */
    c->int_status = 1;
    rk_timer_update_irq(c);
}

static void rk_timer_control(RockchipTimerChannel *c, uint32_t value)
{
    uint32_t old = c->control;

    c->control = value & 7;
    ptimer_transaction_begin(c->ptimer);
    if ((c->control & CTRL_ENABLE) && !(old & CTRL_ENABLE)) {
        ptimer_set_limit(c->ptimer, c->load, 1);
        ptimer_run(c->ptimer, (c->control & CTRL_USER_MODE) ? 1 : 0);
    } else if (!(c->control & CTRL_ENABLE) && (old & CTRL_ENABLE)) {
        ptimer_stop(c->ptimer);
    }
    ptimer_transaction_commit(c->ptimer);
    rk_timer_update_irq(c);
}

static uint64_t rk_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipTimerState *s = opaque;
    unsigned n = offset / 0x20;
    hwaddr reg = offset % 0x20;
    RockchipTimerChannel *c;
    uint64_t cur;

    if (n >= s->num_channels) {
        return 0;
    }
    c = &s->ch[n];
    switch (reg) {
    case TIMER_LOAD_COUNT0:
        return (uint32_t)c->load;
    case TIMER_LOAD_COUNT1:
        return c->load >> 32;
    case TIMER_CUR_VALUE0:
    case TIMER_CUR_VALUE1:
        cur = ptimer_get_count(c->ptimer);
        return reg == TIMER_CUR_VALUE0 ? (uint32_t)cur : cur >> 32;
    case TIMER_CONTROL:
        return c->control;
    case TIMER_INT_STATUS:
        return c->int_status;
    }
    return 0;
}

static void rk_timer_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    RockchipTimerState *s = opaque;
    unsigned n = offset / 0x20;
    hwaddr reg = offset % 0x20;
    RockchipTimerChannel *c;

    if (n >= s->num_channels) {
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-timer: bad offset 0x%"
                      HWADDR_PRIx "\n", offset);
        return;
    }
    c = &s->ch[n];
    switch (reg) {
    case TIMER_LOAD_COUNT0:
        c->load = (c->load & 0xffffffff00000000ull) | (uint32_t)value;
        break;
    case TIMER_LOAD_COUNT1:
        c->load = (c->load & 0xffffffffull) | ((uint64_t)value << 32);
        break;
    case TIMER_CONTROL:
        rk_timer_control(c, value);
        break;
    case TIMER_INT_STATUS:
        if (value & 1) {
            c->int_status = 0;
            rk_timer_update_irq(c);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-timer: write to RO/bad "
                      "offset 0x%" HWADDR_PRIx "\n", offset);
    }
}

static const MemoryRegionOps rk_timer_ops = {
    .read = rk_timer_read,
    .write = rk_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk_timer_reset_hold(Object *obj, ResetType type)
{
    RockchipTimerState *s = ROCKCHIP_TIMER(obj);

    for (unsigned i = 0; i < s->num_channels; i++) {
        RockchipTimerChannel *c = &s->ch[i];

        ptimer_transaction_begin(c->ptimer);
        ptimer_stop(c->ptimer);
        ptimer_set_limit(c->ptimer, 0, 1);
        ptimer_transaction_commit(c->ptimer);
        c->load = 0;
        c->control = 0;
        c->int_status = 0;
    }
}

static void rk_timer_realize(DeviceState *dev, Error **errp)
{
    RockchipTimerState *s = ROCKCHIP_TIMER(dev);

    if (!s->num_channels || s->num_channels > ROCKCHIP_TIMER_MAX_CHANNELS) {
        error_setg(errp, "rockchip-timer: invalid num-channels");
        return;
    }
    memory_region_init_io(&s->iomem, OBJECT(s), &rk_timer_ops, s,
                          TYPE_ROCKCHIP_TIMER, 0x20 * s->num_channels);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (unsigned i = 0; i < s->num_channels; i++) {
        RockchipTimerChannel *c = &s->ch[i];

        c->parent = s;
        c->index = i;
        c->ptimer = ptimer_init(rk_timer_expired, c, PTIMER_POLICY_RK);
        ptimer_transaction_begin(c->ptimer);
        ptimer_set_freq(c->ptimer, s->freq);
        ptimer_transaction_commit(c->ptimer);
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &c->irq);
    }
}

static const Property rk_timer_props[] = {
    DEFINE_PROP_UINT32("num-channels", RockchipTimerState, num_channels, 6),
    DEFINE_PROP_UINT32("clock-frequency", RockchipTimerState, freq,
                       24000000),
};

static const VMStateDescription vmstate_rk_timer_ch = {
    .name = "rockchip-timer-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(ptimer, RockchipTimerChannel),
        VMSTATE_UINT64(load, RockchipTimerChannel),
        VMSTATE_UINT32(control, RockchipTimerChannel),
        VMSTATE_UINT32(int_status, RockchipTimerChannel),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_rk_timer = {
    .name = TYPE_ROCKCHIP_TIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, RockchipTimerState,
                             ROCKCHIP_TIMER_MAX_CHANNELS, 1,
                             vmstate_rk_timer_ch, RockchipTimerChannel),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = rk_timer_realize;
    dc->vmsd = &vmstate_rk_timer;
    device_class_set_props(dc, rk_timer_props);
    rc->phases.hold = rk_timer_reset_hold;
}

static const TypeInfo rk_timer_info = {
    .name = TYPE_ROCKCHIP_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipTimerState),
    .class_init = rk_timer_class_init,
};

static void rk_timer_register_types(void)
{
    type_register_static(&rk_timer_info);
}

type_init(rk_timer_register_types)
