/*
 * Rockchip General Register File (GRF / PMUGRF / syscon) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/rockchip_grf.h"
#include "migration/vmstate.h"

static uint64_t rk_grf_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipGRFState *s = opaque;

    return s->regs[offset >> 2];
}

static void rk_grf_pvtm_update(RockchipGRFState *s)
{
    uint32_t con = s->regs[s->pvtm_con >> 2];
    uint32_t cal = s->regs[(s->pvtm_con + 4) >> 2];

    if ((con & 3) == 3) {
        /* measurement over CAL cycles of the 24 MHz clock */
        s->regs[s->pvtm_sta >> 2] |= 1;
        s->regs[(s->pvtm_sta + 4) >> 2] =
            (uint64_t)cal * s->pvtm_rate / 24000;
    } else {
        s->regs[s->pvtm_sta >> 2] &= ~1u;
    }
}

void rockchip_grf_add_pvtm(RockchipGRFState *s, hwaddr con, hwaddr sta,
                           uint32_t counts_per_ms)
{
    s->has_pvtm = true;
    s->pvtm_con = con;
    s->pvtm_sta = sta;
    s->pvtm_rate = counts_per_ms;
    rockchip_grf_set_reg(s, con + 4, 0, RK_GRF_F_PLAIN);
    rockchip_grf_set_reg(s, sta, 0, RK_GRF_F_RO);
    rockchip_grf_set_reg(s, sta + 4, 0, RK_GRF_F_RO);
}

static void rk_grf_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RockchipGRFState *s = opaque;
    unsigned idx = offset >> 2;
    uint32_t mask;

    if (s->flags[idx] & RK_GRF_F_RO) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register 0x%" HWADDR_PRIx "\n",
                      s->name, offset);
        return;
    }
    if (s->flags[idx] & RK_GRF_F_PLAIN) {
        s->regs[idx] = value;
        if (s->has_pvtm && offset == s->pvtm_con + 4) {
            rk_grf_pvtm_update(s);
        }
        return;
    }
    mask = (value >> 16) & 0xffff;
    s->regs[idx] = (s->regs[idx] & ~mask) | (value & mask);
    if (s->has_pvtm && offset == s->pvtm_con) {
        rk_grf_pvtm_update(s);
    }
}

static const MemoryRegionOps rk_grf_ops = {
    .read = rk_grf_read,
    .write = rk_grf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

void rockchip_grf_set_reg(RockchipGRFState *s, hwaddr offset,
                          uint32_t reset_value, unsigned flags)
{
    unsigned idx = offset >> 2;

    assert(offset < s->size);
    s->reset_val[idx] = reset_value;
    s->flags[idx] = flags;
    s->regs[idx] = reset_value;
}

void rockchip_grf_set_range_flags(RockchipGRFState *s, hwaddr start,
                                  hwaddr end, unsigned flags)
{
    for (hwaddr o = start; o < end && o < s->size; o += 4) {
        s->flags[o >> 2] = flags;
    }
}

uint32_t rockchip_grf_read_reg(RockchipGRFState *s, hwaddr offset)
{
    return s->regs[offset >> 2];
}

static void rk_grf_reset_hold(Object *obj, ResetType type)
{
    RockchipGRFState *s = ROCKCHIP_GRF(obj);

    for (unsigned i = 0; i < s->size / 4; i++) {
        if ((s->flags[i] & RK_GRF_F_KEEP) && s->powered_on) {
            continue;
        }
        s->regs[i] = s->reset_val[i];
    }
    s->powered_on = true;
}

static void rk_grf_realize(DeviceState *dev, Error **errp)
{
    RockchipGRFState *s = ROCKCHIP_GRF(dev);

    if (!s->size || (s->size & 3)) {
        error_setg(errp, "rockchip-grf: invalid size 0x%x", s->size);
        return;
    }
    s->nregs = s->size / 4;
    s->regs = g_new0(uint32_t, s->nregs);
    s->reset_val = g_new0(uint32_t, s->size / 4);
    s->flags = g_new0(uint8_t, s->size / 4);
    memory_region_init_io(&s->iomem, OBJECT(s), &rk_grf_ops, s,
                          s->name ? s->name : TYPE_ROCKCHIP_GRF, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property rk_grf_props[] = {
    DEFINE_PROP_UINT32("size", RockchipGRFState, size, 0x1000),
    DEFINE_PROP_STRING("name", RockchipGRFState, name),
};

static const VMStateDescription vmstate_rk_grf = {
    .name = TYPE_ROCKCHIP_GRF,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(powered_on, RockchipGRFState),
        VMSTATE_VARRAY_UINT32(regs, RockchipGRFState, nregs, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_grf_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = rk_grf_realize;
    dc->vmsd = &vmstate_rk_grf;
    device_class_set_props(dc, rk_grf_props);
    rc->phases.hold = rk_grf_reset_hold;
}

static const TypeInfo rk_grf_info = {
    .name = TYPE_ROCKCHIP_GRF,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipGRFState),
    .class_init = rk_grf_class_init,
};

static void rk_grf_register_types(void)
{
    type_register_static(&rk_grf_info);
}

type_init(rk_grf_register_types)
