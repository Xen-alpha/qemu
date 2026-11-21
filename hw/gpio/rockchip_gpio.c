/*
 * Rockchip GPIO bank (v1, DesignWare APB GPIO derived) model
 *
 * Used by RK3288/RK3328/RK3368/PX30/RK3326.  Register layout matches
 * drivers/pinctrl/pinctrl-rockchip.c (Linux 4.4) and
 * drivers/gpio/rk_gpio.c (u-boot).
 *
 * GPIO lines: input gpio n drives external pin n, output gpio n reflects
 * pin n while it is configured as an output.  The bank IRQ is an
 * active-high level output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/gpio/rockchip_gpio.h"
#include "migration/vmstate.h"

#define GPIO_SWPORT_DR          0x00
#define GPIO_SWPORT_DDR         0x04
#define GPIO_INTEN              0x30
#define GPIO_INTMASK            0x34
#define GPIO_INTTYPE_LEVEL      0x38
#define GPIO_INT_POLARITY       0x3c
#define GPIO_INT_STATUS         0x40
#define GPIO_INT_RAWSTATUS      0x44
#define GPIO_DEBOUNCE           0x48
#define GPIO_PORTS_EOI          0x4c
#define GPIO_EXT_PORT           0x50
#define GPIO_LS_SYNC            0x60

static uint32_t rk_gpio_pin_level(RockchipGPIOState *s)
{
    return (s->dr & s->ddr) | (s->in_level & ~s->ddr);
}

static uint32_t rk_gpio_raw(RockchipGPIOState *s)
{
    uint32_t level = rk_gpio_pin_level(s);
    uint32_t lvl_active = ~(level ^ s->intpol);

    return (lvl_active & ~s->inttype) | (s->raw_edge & s->inttype);
}

static void rk_gpio_update(RockchipGPIOState *s)
{
    uint32_t level = rk_gpio_pin_level(s);
    uint32_t changed = level ^ s->last_level;
    uint32_t edges;
    uint32_t outs;

    /* edge detection on all enabled edge-type interrupts */
    edges = changed & s->inten & s->inttype &
            ~(level ^ s->intpol);          /* new level matches polarity */
    s->raw_edge |= edges;
    s->last_level = level;

    qemu_set_irq(s->irq, !!(rk_gpio_raw(s) & s->inten & ~s->intmask));

    outs = s->ddr;
    for (int i = 0; i < ROCKCHIP_GPIO_PINS; i++) {
        uint32_t bit = 1u << i;
        if ((outs & bit) && ((s->out_level ^ level) & bit)) {
            qemu_set_irq(s->out[i], !!(level & bit));
        }
    }
    s->out_level = (s->out_level & ~outs) | (level & outs);
}

static uint64_t rk_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipGPIOState *s = opaque;

    switch (offset) {
    case GPIO_SWPORT_DR:
        return s->dr;
    case GPIO_SWPORT_DDR:
        return s->ddr;
    case GPIO_INTEN:
        return s->inten;
    case GPIO_INTMASK:
        return s->intmask;
    case GPIO_INTTYPE_LEVEL:
        return s->inttype;
    case GPIO_INT_POLARITY:
        return s->intpol;
    case GPIO_INT_STATUS:
        return rk_gpio_raw(s) & s->inten & ~s->intmask;
    case GPIO_INT_RAWSTATUS:
        return rk_gpio_raw(s) & s->inten;
    case GPIO_DEBOUNCE:
        return s->debounce;
    case GPIO_EXT_PORT:
        return rk_gpio_pin_level(s);
    case GPIO_LS_SYNC:
        return s->ls_sync;
    case GPIO_PORTS_EOI:
        return 0;
    case 0x78:
        /* GPIO_VER_ID on v2 banks; v1 banks read as zero */
        return 0;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rockchip-gpio: bad read offset 0x%"
                  HWADDR_PRIx "\n", offset);
    return 0;
}

static void rk_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    RockchipGPIOState *s = opaque;

    switch (offset) {
    case GPIO_SWPORT_DR:
        s->dr = value;
        break;
    case GPIO_SWPORT_DDR:
        s->ddr = value;
        break;
    case GPIO_INTEN:
        s->inten = value;
        s->raw_edge &= value;
        break;
    case GPIO_INTMASK:
        s->intmask = value;
        break;
    case GPIO_INTTYPE_LEVEL:
        s->inttype = value;
        break;
    case GPIO_INT_POLARITY:
        s->intpol = value;
        break;
    case GPIO_DEBOUNCE:
        s->debounce = value;
        break;
    case GPIO_PORTS_EOI:
        s->raw_edge &= ~value;
        break;
    case GPIO_LS_SYNC:
        s->ls_sync = value & 1;
        break;
    case GPIO_INT_STATUS:
    case GPIO_INT_RAWSTATUS:
    case GPIO_EXT_PORT:
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-gpio: write to RO reg 0x%"
                      HWADDR_PRIx "\n", offset);
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-gpio: bad write offset 0x%"
                      HWADDR_PRIx "\n", offset);
        return;
    }
    rk_gpio_update(s);
}

static const MemoryRegionOps rk_gpio_ops = {
    .read = rk_gpio_read,
    .write = rk_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk_gpio_set_input(void *opaque, int line, int level)
{
    RockchipGPIOState *s = opaque;

    if (level) {
        s->in_level |= 1u << line;
    } else {
        s->in_level &= ~(1u << line);
    }
    rk_gpio_update(s);
}

static void rk_gpio_reset_hold(Object *obj, ResetType type)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(obj);

    s->dr = 0;
    s->ddr = 0;
    s->inten = 0;
    s->intmask = 0;
    s->inttype = 0;
    s->intpol = 0;
    s->raw_edge = 0;
    s->debounce = 0;
    s->ls_sync = 0;
    s->last_level = rk_gpio_pin_level(s);
    s->out_level = 0;
}

static void rk_gpio_reset_exit(Object *obj, ResetType type)
{
    rk_gpio_update(ROCKCHIP_GPIO(obj));
}

static void rk_gpio_realize(DeviceState *dev, Error **errp)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(dev);

    s->in_level = s->in_default;
}

static void rk_gpio_init(Object *obj)
{
    RockchipGPIOState *s = ROCKCHIP_GPIO(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rk_gpio_ops, s,
                          TYPE_ROCKCHIP_GPIO, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in(dev, rk_gpio_set_input, ROCKCHIP_GPIO_PINS);
    qdev_init_gpio_out(dev, s->out, ROCKCHIP_GPIO_PINS);
}

static const Property rk_gpio_props[] = {
    DEFINE_PROP_UINT32("input-default", RockchipGPIOState, in_default, 0),
};

static const VMStateDescription vmstate_rk_gpio = {
    .name = TYPE_ROCKCHIP_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(in_level, RockchipGPIOState),
        VMSTATE_UINT32(dr, RockchipGPIOState),
        VMSTATE_UINT32(ddr, RockchipGPIOState),
        VMSTATE_UINT32(inten, RockchipGPIOState),
        VMSTATE_UINT32(intmask, RockchipGPIOState),
        VMSTATE_UINT32(inttype, RockchipGPIOState),
        VMSTATE_UINT32(intpol, RockchipGPIOState),
        VMSTATE_UINT32(raw_edge, RockchipGPIOState),
        VMSTATE_UINT32(debounce, RockchipGPIOState),
        VMSTATE_UINT32(ls_sync, RockchipGPIOState),
        VMSTATE_UINT32(last_level, RockchipGPIOState),
        VMSTATE_UINT32(out_level, RockchipGPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = rk_gpio_realize;
    dc->vmsd = &vmstate_rk_gpio;
    device_class_set_props(dc, rk_gpio_props);
    rc->phases.hold = rk_gpio_reset_hold;
    rc->phases.exit = rk_gpio_reset_exit;
}

static const TypeInfo rk_gpio_info = {
    .name = TYPE_ROCKCHIP_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipGPIOState),
    .instance_init = rk_gpio_init,
    .class_init = rk_gpio_class_init,
};

static void rk_gpio_register_types(void)
{
    type_register_static(&rk_gpio_info);
}

type_init(rk_gpio_register_types)
