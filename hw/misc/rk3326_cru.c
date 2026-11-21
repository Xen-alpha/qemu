/*
 * Rockchip RK3326/PX30 Clock and Reset Unit (CRU and PMUCRU)
 *
 * Register layout follows the PX30 CRU as used by the Hardkernel
 * u-boot (arch/arm/include/asm/arch-rockchip/cru_px30.h) and Linux
 * (drivers/clk/rockchip/clk-px30.c) for the ODROID-GO Advance.
 *
 * Clocks are not modelled: the CRU is a register file with the PX30
 * HIWORD write-mask semantics, PLL lock reporting and the global
 * software reset registers.
 *
 * Reset values describe the state left behind by the Rockchip DDR init
 * blob + miniloader (which the QEMU board replaces): all PLLs locked in
 * normal mode, APLL 600 MHz, DPLL 666 MHz (DDR 333 MHz), CPLL 1200 MHz,
 * NPLL 1188 MHz, GPLL 1200 MHz.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/rk3326_cru.h"
#include "migration/vmstate.h"
#include "system/runstate.h"

/* CRU */
#define CRU_PLL_CON(pll, n)     ((pll) * 0x20 + (n) * 4)
#define CRU_MODE_CON            0xa0
#define CRU_MISC_CON            0xa4
#define CRU_GLB_CNT_TH          0xb0
#define CRU_GLB_RST_ST          0xb4
#define CRU_GLB_SRST_FST        0xb8
#define CRU_GLB_SRST_SND        0xbc
#define CRU_GLB_RST_CON         0xc0
#define CRU_SSGTBL_START        0x280
#define CRU_SSGTBL_END          0x300

/* PMUCRU */
#define PMUCRU_MODE             0x20

#define PLL_CON1_LOCK           (1u << 10)
#define PLL_CON1_PWRDOWN        (1u << 13)

#define GLB_SRST_FST_VAL        0xfdb9
#define GLB_SRST_SND_VAL        0xeca8

static bool rk_cru_is_pll_reg(RK3326CRUState *s, hwaddr off, int *n)
{
    hwaddr end = s->pmucru ? 0x20 : 0x80;

    if (off >= end) {
        return false;
    }
    *n = (off & 0x1f) >> 2;
    return *n <= 4;
}

static bool rk_cru_is_plain(RK3326CRUState *s, hwaddr off)
{
    int n;

    if (rk_cru_is_pll_reg(s, off, &n)) {
        return n == 2;          /* CON2: 24-bit fractional divider */
    }
    if (s->pmucru) {
        return false;
    }
    switch (off) {
    case CRU_GLB_CNT_TH:
    case CRU_GLB_RST_CON:
        return true;
    }
    return off >= CRU_SSGTBL_START && off < CRU_SSGTBL_END;
}

static uint64_t rk_cru_read(void *opaque, hwaddr offset, unsigned size)
{
    RK3326CRUState *s = opaque;
    uint32_t val = s->regs[offset >> 2];
    int n;

    if (rk_cru_is_pll_reg(s, offset, &n) && n == 1) {
        /* PLLs lock instantly once powered up */
        val &= ~PLL_CON1_LOCK;
        if (!(val & PLL_CON1_PWRDOWN)) {
            val |= PLL_CON1_LOCK;
        }
    }
    if (!s->pmucru && offset == CRU_GLB_RST_ST) {
        val = s->glb_rst_st;
    }
    return val;
}

static void rk_cru_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RK3326CRUState *s = opaque;
    unsigned idx = offset >> 2;
    uint32_t mask;

    if (!s->pmucru) {
        switch (offset) {
        case CRU_GLB_RST_ST:
            /* write 1 to clear */
            s->glb_rst_st &= ~value;
            return;
        case CRU_GLB_SRST_FST:
        case CRU_GLB_SRST_SND:
            s->regs[idx] = value;
            if ((offset == CRU_GLB_SRST_FST && value == GLB_SRST_FST_VAL) ||
                (offset == CRU_GLB_SRST_SND && value == GLB_SRST_SND_VAL)) {
                s->glb_rst_st = (offset == CRU_GLB_SRST_FST) ? 1 : 2;
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
            return;
        }
    }

    if (rk_cru_is_plain(s, offset)) {
        s->regs[idx] = value;
        return;
    }
    mask = (value >> 16) & 0xffff;
    s->regs[idx] = (s->regs[idx] & ~mask) | (value & mask);
}

static const MemoryRegionOps rk_cru_ops = {
    .read = rk_cru_read,
    .write = rk_cru_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/*
 * PLL register encoding (rk3036/rk3328 style integer PLL):
 *  CON0: [15] bypass, [14:12] postdiv1, [11:0] fbdiv
 *  CON1: [13] pwrdown, [12] dsmpd, [10] lock, [8:6] postdiv2, [5:0] refdiv
 */
#define PLL_CON0(fb, pd1)       (((pd1) << 12) | (fb))
#define PLL_CON1(ref, pd2)      ((1u << 12) | ((pd2) << 6) | (ref))

static void rk_cru_set_pll(RK3326CRUState *s, int pll, uint32_t fb,
                           uint32_t ref, uint32_t pd1, uint32_t pd2)
{
    s->regs[CRU_PLL_CON(pll, 0) >> 2] = PLL_CON0(fb, pd1);
    s->regs[CRU_PLL_CON(pll, 1) >> 2] = PLL_CON1(ref, pd2);
    s->regs[CRU_PLL_CON(pll, 2) >> 2] = 0;
}

static void rk_cru_reset_hold(Object *obj, ResetType type)
{
    RK3326CRUState *s = RK3326_CRU(obj);

    memset(s->regs, 0, sizeof(s->regs));
    if (s->pmucru) {
        rk_cru_set_pll(s, 0, 100, 2, 1, 1);         /* GPLL 1200 MHz */
        s->regs[PMUCRU_MODE >> 2] = 0x1;            /* GPLL normal */
    } else {
        rk_cru_set_pll(s, 0, 75, 1, 3, 1);          /* APLL 600 MHz */
        rk_cru_set_pll(s, 1, 111, 1, 4, 1);         /* DPLL 666 MHz */
        rk_cru_set_pll(s, 2, 100, 2, 1, 1);         /* CPLL 1200 MHz */
        rk_cru_set_pll(s, 3, 99, 2, 1, 1);          /* NPLL 1188 MHz */
        /* MODE_CON: apll[1:0] cpll[3:2] dpll[5:4] npll[7:6], 1 = normal */
        s->regs[CRU_MODE_CON >> 2] = 0x55;
        s->regs[CRU_GLB_CNT_TH >> 2] = 0x3a980064;
    }
}

static void rk_cru_init(Object *obj)
{
    RK3326CRUState *s = RK3326_CRU(obj);

    memory_region_init_io(&s->iomem, obj, &rk_cru_ops, s, TYPE_RK3326_CRU,
                          RK3326_CRU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property rk_cru_props[] = {
    DEFINE_PROP_BOOL("pmucru", RK3326CRUState, pmucru, false),
};

static const VMStateDescription vmstate_rk_cru = {
    .name = TYPE_RK3326_CRU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RK3326CRUState, RK3326_CRU_NREGS),
        VMSTATE_UINT32(glb_rst_st, RK3326CRUState),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_cru_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rk_cru;
    device_class_set_props(dc, rk_cru_props);
    rc->phases.hold = rk_cru_reset_hold;
}

static const TypeInfo rk_cru_info = {
    .name = TYPE_RK3326_CRU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3326CRUState),
    .instance_init = rk_cru_init,
    .class_init = rk_cru_class_init,
};

static void rk_cru_register_types(void)
{
    type_register_static(&rk_cru_info);
}

type_init(rk_cru_register_types)
