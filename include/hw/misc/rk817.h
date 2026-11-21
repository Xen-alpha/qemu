/*
 * Rockchip RK817 PMIC (regulators, RTC, power key, fuel gauge, charger,
 * audio codec) I2C model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK817_H
#define HW_MISC_RK817_H

#include "hw/i2c/i2c.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RK817 "rk817"
OBJECT_DECLARE_SIMPLE_TYPE(RK817State, RK817)

typedef struct RK817Date {
    int sec, min, hour, mday, mon, year, wday;   /* mon 1..12, year full */
} RK817Date;

struct RK817State {
    I2CSlave parent_obj;

    qemu_irq irq;           /* INT pin, physical level (honours INT_POL) */
    QEMUTimer *rtc_timer;

    uint8_t regs[256];
    uint8_t ptr;
    bool ptr_set;
    bool sleep_pin;         /* SLEEP pin level from the SoC */
    bool pwrkey;            /* power button pressed */
    bool irq_level;
    bool powered;           /* cold power-on done */

    /* RTC */
    RK817Date rtc_base;
    int64_t rtc_base_ns;
    bool rtc_stopped;
    RK817Date rtc_latch;

    /* fuel gauge coulomb counter */
    int64_t q_press;
    bool q_valid;
    int64_t q_last_ns;

    /* board configuration (properties) */
    uint32_t bat_mv;
    int32_t bat_ma;         /* positive: charging */
    uint32_t usb_mv;
    bool dc_in;
    bool battery_present;
};

/* Board helpers */
void rk817_set_pwrkey(RK817State *s, bool pressed);
void rk817_set_dc_in(RK817State *s, bool present);

#endif
