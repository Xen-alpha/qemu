/*
 * Rockchip RK3x I2C controller model (rk3399-i2c / px30)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_ROCKCHIP_I2C_H
#define HW_I2C_ROCKCHIP_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_I2C "rockchip-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipI2CState, ROCKCHIP_I2C)

struct RockchipI2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    I2CBus *bus;

    uint32_t con;
    uint32_t clkdiv;
    uint32_t mrxaddr;
    uint32_t mrxraddr;
    uint32_t mtxcnt;
    uint32_t mrxcnt;
    uint32_t ien;
    uint32_t ipd;
    uint32_t fcnt;
    uint32_t scl_oe_db;
    uint32_t txdata[8];
    uint32_t rxdata[8];

    bool started;           /* START condition issued, no address yet */
    bool active;            /* a transfer is open on the bus */
    bool nak;               /* slave NAKed, waiting for STOP */
};

#endif
