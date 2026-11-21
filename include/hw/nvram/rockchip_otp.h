/*
 * Rockchip OTP controller (px30-otp) model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NVRAM_ROCKCHIP_OTP_H
#define HW_NVRAM_ROCKCHIP_OTP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_OTP "rockchip-otp"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipOTPState, ROCKCHIP_OTP)

#define ROCKCHIP_OTP_BYTES  512

struct RockchipOTPState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t data[ROCKCHIP_OTP_BYTES];
    char *serial;           /* property: seeds the 16 byte chip id */
    uint8_t cpu_leakage;    /* property */

    uint32_t sbpi_ctrl;
    uint32_t user_ctrl;
    uint32_t user_addr;
    uint32_t user_q;
    uint32_t int_status;
};

#endif
