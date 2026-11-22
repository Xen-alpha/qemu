/*
 * Rockchip RK3x I2C controller model (rk3399-i2c / px30)
 *
 * The controller works on 32-byte chunks.  Transfers are started by
 * writing MTXCNT (transmit) or MRXCNT (receive) and complete instantly.
 * Modes (CON[2:1]):
 *   0 TX     raw bytes from TXDATA; the first byte after START is the
 *            address byte
 *   1 TRX    "register read": addresses the slave in MRXADDR, sends the
 *            register bytes from MRXRADDR (if any are valid), issues a
 *            repeated START with the read bit and receives MRXCNT bytes
 *   2 RX     receives MRXCNT more bytes of an ongoing read
 *   3 RRX    like TRX, but without repeated START (treated as TRX)
 *
 * Matches drivers/i2c/busses/i2c-rk3x.c (Linux) and
 * drivers/i2c/rk_i2c.c (u-boot).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/i2c/rockchip_i2c.h"
#include "migration/vmstate.h"

#define REG_CON         0x000
#define REG_CLKDIV      0x004
#define REG_MRXADDR     0x008
#define REG_MRXRADDR    0x00c
#define REG_MTXCNT      0x010
#define REG_MRXCNT      0x014
#define REG_IEN         0x018
#define REG_IPD         0x01c
#define REG_FCNT        0x020
#define REG_SCL_OE_DB   0x024
#define REG_TXDATA      0x100
#define REG_RXDATA      0x200

#define CON_EN          (1u << 0)
#define CON_MOD_SHIFT   1
#define CON_MOD_MASK    (3u << CON_MOD_SHIFT)
#define CON_MOD_TX      0
#define CON_MOD_TRX     1
#define CON_MOD_RX      2
#define CON_MOD_RRX     3
#define CON_START       (1u << 3)
#define CON_STOP        (1u << 4)
#define CON_LASTACK     (1u << 5)
#define CON_ACTACK      (1u << 6)

#define INT_BTF         (1u << 0)
#define INT_BRF         (1u << 1)
#define INT_MBTF        (1u << 2)
#define INT_MBRF        (1u << 3)
#define INT_START       (1u << 4)
#define INT_STOP        (1u << 5)
#define INT_NAKRCV      (1u << 6)
#define INT_ALL         0x7f

static void rk_i2c_update_irq(RockchipI2CState *s)
{
    qemu_set_irq(s->irq, !!(s->ipd & s->ien));
}

static void rk_i2c_nak(RockchipI2CState *s)
{
    s->nak = true;
    s->ipd |= INT_NAKRCV;
}

static bool rk_i2c_start(RockchipI2CState *s, uint8_t addr7, bool recv)
{
    if (i2c_start_transfer(s->bus, addr7, recv)) {
        s->active = false;
        rk_i2c_nak(s);
        return false;
    }
    s->active = true;
    return true;
}

static void rk_i2c_do_tx(RockchipI2CState *s)
{
    unsigned cnt = s->mtxcnt & 0x3f;

    if (cnt > 32) {
        cnt = 32;
    }
    for (unsigned i = 0; i < cnt && !s->nak; i++) {
        uint8_t byte = s->txdata[i / 4] >> ((i % 4) * 8);

        if (s->started) {
            s->started = false;
            if (s->active) {
                /* repeated START */
                i2c_end_transfer(s->bus);
                s->active = false;
            }
            if (!rk_i2c_start(s, byte >> 1, byte & 1)) {
                break;
            }
            continue;
        }
        if (!s->active) {
            rk_i2c_nak(s);
            break;
        }
        if (i2c_send(s->bus, byte)) {
            rk_i2c_nak(s);
            break;
        }
    }
    if (!s->nak) {
        s->ipd |= INT_MBTF;
    }
    rk_i2c_update_irq(s);
}

static void rk_i2c_do_rx(RockchipI2CState *s)
{
    unsigned mode = (s->con & CON_MOD_MASK) >> CON_MOD_SHIFT;
    unsigned cnt = s->mrxcnt & 0x3f;

    if (cnt > 32) {
        cnt = 32;
    }

    if (mode == CON_MOD_TRX || mode == CON_MOD_RRX || s->started) {
        uint8_t addr = s->mrxaddr & 0xff;
        bool have_reg = s->mrxraddr & (7u << 24);

        s->started = false;
        if (s->active) {
            i2c_end_transfer(s->bus);
            s->active = false;
        }
        if (have_reg) {
            if (!rk_i2c_start(s, addr >> 1, false)) {
                goto done;
            }
            for (int i = 0; i < 3; i++) {
                if (s->mrxraddr & (1u << (24 + i))) {
                    if (i2c_send(s->bus, s->mrxraddr >> (8 * i))) {
                        rk_i2c_nak(s);
                        goto done;
                    }
                }
            }
            i2c_end_transfer(s->bus);
            s->active = false;
        }
        if (!rk_i2c_start(s, addr >> 1, true)) {
            goto done;
        }
    }

    if (!s->active) {
        rk_i2c_nak(s);
        goto done;
    }
    memset(s->rxdata, 0, sizeof(s->rxdata));
    for (unsigned i = 0; i < cnt; i++) {
        if (i == cnt - 1 && (s->con & CON_LASTACK)) {
            i2c_nack(s->bus);
        }
        s->rxdata[i / 4] |= (uint32_t)i2c_recv(s->bus) << ((i % 4) * 8);
    }
done:
    if (!s->nak) {
        s->ipd |= INT_MBRF;
    }
    rk_i2c_update_irq(s);
}

static void rk_i2c_write_con(RockchipI2CState *s, uint32_t value)
{
    uint32_t old = s->con;

    s->con = value;
    if (!(value & CON_EN)) {
        if (s->active) {
            i2c_end_transfer(s->bus);
            s->active = false;
        }
        s->started = false;
        return;
    }
    if ((value & CON_START) && !(old & CON_START)) {
        s->started = true;
        s->nak = false;
        s->ipd |= INT_START;
    }
    if ((value & CON_STOP) && !(old & CON_STOP)) {
        if (s->active) {
            i2c_end_transfer(s->bus);
            s->active = false;
        }
        s->started = false;
        s->nak = false;
        s->ipd |= INT_STOP;
    }
    rk_i2c_update_irq(s);
}

static uint64_t rk_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipI2CState *s = opaque;

    if (offset >= REG_TXDATA && offset < REG_TXDATA + 0x20) {
        return s->txdata[(offset - REG_TXDATA) >> 2];
    }
    if (offset >= REG_RXDATA && offset < REG_RXDATA + 0x20) {
        return s->rxdata[(offset - REG_RXDATA) >> 2];
    }
    switch (offset) {
    case REG_CON:
        return s->con;
    case REG_CLKDIV:
        return s->clkdiv;
    case REG_MRXADDR:
        return s->mrxaddr;
    case REG_MRXRADDR:
        return s->mrxraddr;
    case REG_MTXCNT:
        return s->mtxcnt;
    case REG_MRXCNT:
        return s->mrxcnt;
    case REG_IEN:
        return s->ien;
    case REG_IPD:
        return s->ipd;
    case REG_FCNT:
        return 0;
    case REG_SCL_OE_DB:
        return s->scl_oe_db;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rockchip-i2c: bad read 0x%" HWADDR_PRIx
                  "\n", offset);
    return 0;
}

static void rk_i2c_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RockchipI2CState *s = opaque;

    if (offset >= REG_TXDATA && offset < REG_TXDATA + 0x20) {
        s->txdata[(offset - REG_TXDATA) >> 2] = value;
        return;
    }
    if (offset >= REG_RXDATA && offset < REG_RXDATA + 0x20) {
        return;
    }
    switch (offset) {
    case REG_CON:
        rk_i2c_write_con(s, value);
        break;
    case REG_CLKDIV:
        s->clkdiv = value;
        break;
    case REG_MRXADDR:
        s->mrxaddr = value & 0x07ffffff;
        break;
    case REG_MRXRADDR:
        s->mrxraddr = value & 0x07ffffff;
        break;
    case REG_MTXCNT:
        s->mtxcnt = value & 0x3f;
        if ((s->con & CON_EN) && s->mtxcnt) {
            rk_i2c_do_tx(s);
        }
        break;
    case REG_MRXCNT:
        s->mrxcnt = value & 0x3f;
        if ((s->con & CON_EN) && s->mrxcnt) {
            rk_i2c_do_rx(s);
        }
        break;
    case REG_IEN:
        s->ien = value & INT_ALL;
        rk_i2c_update_irq(s);
        break;
    case REG_IPD:
        s->ipd &= ~(value & INT_ALL);
        rk_i2c_update_irq(s);
        break;
    case REG_SCL_OE_DB:
        s->scl_oe_db = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-i2c: bad write 0x%"
                      HWADDR_PRIx "\n", offset);
    }
}

static const MemoryRegionOps rk_i2c_ops = {
    .read = rk_i2c_read,
    .write = rk_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rk_i2c_reset_hold(Object *obj, ResetType type)
{
    RockchipI2CState *s = ROCKCHIP_I2C(obj);

    if (s->active) {
        i2c_end_transfer(s->bus);
    }
    s->con = 0;
    s->clkdiv = 0x00060006;
    s->mrxaddr = 0;
    s->mrxraddr = 0;
    s->mtxcnt = 0;
    s->mrxcnt = 0;
    s->ien = 0;
    s->ipd = 0;
    s->scl_oe_db = 0;
    memset(s->txdata, 0, sizeof(s->txdata));
    memset(s->rxdata, 0, sizeof(s->rxdata));
    s->started = false;
    s->active = false;
    s->nak = false;
}

static void rk_i2c_init(Object *obj)
{
    RockchipI2CState *s = ROCKCHIP_I2C(obj);

    memory_region_init_io(&s->iomem, obj, &rk_i2c_ops, s, TYPE_ROCKCHIP_I2C,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static const VMStateDescription vmstate_rk_i2c = {
    .name = TYPE_ROCKCHIP_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(con, RockchipI2CState),
        VMSTATE_UINT32(clkdiv, RockchipI2CState),
        VMSTATE_UINT32(mrxaddr, RockchipI2CState),
        VMSTATE_UINT32(mrxraddr, RockchipI2CState),
        VMSTATE_UINT32(mtxcnt, RockchipI2CState),
        VMSTATE_UINT32(mrxcnt, RockchipI2CState),
        VMSTATE_UINT32(ien, RockchipI2CState),
        VMSTATE_UINT32(ipd, RockchipI2CState),
        VMSTATE_UINT32(scl_oe_db, RockchipI2CState),
        VMSTATE_UINT32_ARRAY(txdata, RockchipI2CState, 8),
        VMSTATE_UINT32_ARRAY(rxdata, RockchipI2CState, 8),
        VMSTATE_BOOL(started, RockchipI2CState),
        VMSTATE_BOOL(active, RockchipI2CState),
        VMSTATE_BOOL(nak, RockchipI2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rk_i2c;
    rc->phases.hold = rk_i2c_reset_hold;
}

static const TypeInfo rk_i2c_info = {
    .name = TYPE_ROCKCHIP_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipI2CState),
    .instance_init = rk_i2c_init,
    .class_init = rk_i2c_class_init,
};

static void rk_i2c_register_types(void)
{
    type_register_static(&rk_i2c_info);
}

type_init(rk_i2c_register_types)
