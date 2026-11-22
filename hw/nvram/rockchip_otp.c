/*
 * Rockchip OTP controller (px30-otp) model
 *
 * Implements the user-mode read state machine used by
 * drivers/nvmem/rockchip-otp.c:
 *   0x0020 SBPI_CTRL      [0] start SBPI command sequence -> SBPI_DONE
 *   0x0100 USER_CTRL      hiword, [0] use user interface
 *   0x0104 USER_ADDR      hiword, [15:0] byte address
 *   0x0108 USER_ENABLE    hiword, [0] start read -> USER_DONE
 *   0x0124 USER_Q         read data
 *   0x0304 INT_STATUS     [1] SBPI_DONE, [2] USER_DONE (write 1 clear)
 * SBPI commands (ECC config) are accepted and ignored.
 *
 * Contents: the 16-byte chip id cell at 0x07 (used as serial number by
 * rockchip-cpuinfo), cpu leakage at 0x17 and performance bits at 0x1e.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/nvram/rockchip_otp.h"
#include "migration/vmstate.h"

#define OTPC_SBPI_CTRL      0x0020
#define OTPC_USER_CTRL      0x0100
#define OTPC_USER_ADDR      0x0104
#define OTPC_USER_ENABLE    0x0108
#define OTPC_USER_Q         0x0124
#define OTPC_INT_STATUS     0x0304

#define OTPC_SBPI_DONE      (1u << 1)
#define OTPC_USER_DONE      (1u << 2)

#define OTP_ID_OFFSET       0x07
#define OTP_ID_LEN          0x10
#define OTP_LEAKAGE_OFFSET  0x17

static uint32_t hiword(uint32_t old, uint32_t val)
{
    uint32_t mask = val >> 16;

    return (old & ~mask) | (val & mask);
}

static uint64_t rk_otp_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipOTPState *s = opaque;

    switch (offset) {
    case OTPC_SBPI_CTRL:
        return s->sbpi_ctrl;
    case OTPC_USER_CTRL:
        return s->user_ctrl;
    case OTPC_USER_ADDR:
        return s->user_addr;
    case OTPC_USER_Q:
        return s->user_q;
    case OTPC_INT_STATUS:
        return s->int_status;
    }
    return 0;
}

static void rk_otp_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RockchipOTPState *s = opaque;

    switch (offset) {
    case OTPC_SBPI_CTRL:
        s->sbpi_ctrl = hiword(s->sbpi_ctrl, value);
        if (s->sbpi_ctrl & 1) {
            s->sbpi_ctrl &= ~1;
            s->int_status |= OTPC_SBPI_DONE;
        }
        break;
    case OTPC_USER_CTRL:
        s->user_ctrl = hiword(s->user_ctrl, value);
        break;
    case OTPC_USER_ADDR:
        s->user_addr = hiword(s->user_addr, value);
        break;
    case OTPC_USER_ENABLE:
        if ((value & (1u << 16)) && (value & 1)) {
            unsigned addr = s->user_addr & 0xffff;

            s->user_q = addr < ROCKCHIP_OTP_BYTES ? s->data[addr] : 0;
            s->int_status |= OTPC_USER_DONE;
        }
        break;
    case OTPC_INT_STATUS:
        s->int_status &= ~value;
        break;
    default:
        /* SBPI command buffer and timing registers */
        break;
    }
}

static const MemoryRegionOps rk_otp_ops = {
    .read = rk_otp_read,
    .write = rk_otp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void rk_otp_reset_hold(Object *obj, ResetType type)
{
    RockchipOTPState *s = ROCKCHIP_OTP(obj);

    s->sbpi_ctrl = 0;
    s->user_ctrl = 0;
    s->user_addr = 0;
    s->user_q = 0;
    s->int_status = 0;
}

static void rk_otp_realize(DeviceState *dev, Error **errp)
{
    RockchipOTPState *s = ROCKCHIP_OTP(dev);
    const char *serial = s->serial ? s->serial : "QEMU-RK3326";
    size_t len = strlen(serial);

    memset(s->data, 0, sizeof(s->data));
    /* chip id cell: serial string, zero padded */
    for (int i = 0; i < OTP_ID_LEN; i++) {
        s->data[OTP_ID_OFFSET + i] = i < len ? serial[i] : 0x00;
    }
    s->data[OTP_LEAKAGE_OFFSET] = s->cpu_leakage;
}

static void rk_otp_init(Object *obj)
{
    RockchipOTPState *s = ROCKCHIP_OTP(obj);

    memory_region_init_io(&s->iomem, obj, &rk_otp_ops, s, TYPE_ROCKCHIP_OTP,
                          0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property rk_otp_props[] = {
    DEFINE_PROP_STRING("serial", RockchipOTPState, serial),
    DEFINE_PROP_UINT8("cpu-leakage", RockchipOTPState, cpu_leakage, 0x0f),
};

static const VMStateDescription vmstate_rk_otp = {
    .name = TYPE_ROCKCHIP_OTP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(sbpi_ctrl, RockchipOTPState),
        VMSTATE_UINT32(user_ctrl, RockchipOTPState),
        VMSTATE_UINT32(user_addr, RockchipOTPState),
        VMSTATE_UINT32(user_q, RockchipOTPState),
        VMSTATE_UINT32(int_status, RockchipOTPState),
        VMSTATE_END_OF_LIST()
    }
};

static void rk_otp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = rk_otp_realize;
    dc->vmsd = &vmstate_rk_otp;
    device_class_set_props(dc, rk_otp_props);
    rc->phases.hold = rk_otp_reset_hold;
}

static const TypeInfo rk_otp_info = {
    .name = TYPE_ROCKCHIP_OTP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipOTPState),
    .instance_init = rk_otp_init,
    .class_init = rk_otp_class_init,
};

static void rk_otp_register_types(void)
{
    type_register_static(&rk_otp_info);
}

type_init(rk_otp_register_types)
