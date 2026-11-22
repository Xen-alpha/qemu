/*
 * Rockchip RK817 PMIC I2C model
 *
 * Register map from include/linux/mfd/rk808.h, drivers/mfd/rk808.c,
 * drivers/rtc/rtc-rk808.c, drivers/power/rk817_battery.c and
 * drivers/power/rk817_charger.c of the Hardkernel odroidgoA-4.4.y
 * kernel and drivers/power/{pmic/rk8xx.c,fuel_gauge/fg_rk817.c} of
 * the Hardkernel odroidgoa-v2017.09 u-boot.
 *
 * Modelled:
 *  - I2C register file with auto-incrementing register pointer
 *  - chip id (0x8170), on/off source
 *  - POWER_EN registers with nibble write-enable mask
 *  - RTC: BCD time keeping with the RK808-family calendar in which
 *    November has 31 days (the Linux driver compensates for it),
 *    STOP/GET_TIME/READSEL handling and the alarm interrupt
 *  - interrupt status/mask registers and the INT pin (polarity from
 *    GPIO_INT_CFG), power key press/release interrupts
 *  - SYS_CFG3 DEV_OFF and the SLEEP pin power-down/reset functions
 *  - fuel gauge ADC results (battery/system/USB voltage, battery
 *    current) in the units implied by the VCALIB0/1 calibration
 *    registers, the coulomb counter (Q_INIT/Q_PRESS) and the charger
 *    status registers
 * Codec, clock outputs and regulator voltages are plain registers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bcd.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/rk817.h"
#include "migration/vmstate.h"
#include "system/rtc.h"
#include "system/system.h"
#include "system/runstate.h"

/* RTC */
#define RTC_SECONDS         0x00
#define RTC_WEEKS           0x06
#define RTC_ALARM_SECONDS   0x07
#define RTC_ALARM_YEARS     0x0c
#define RTC_CTRL            0x0d
#define RTC_STATUS          0x0e
#define RTC_INT             0x0f

#define RTC_CTRL_STOP       (1u << 0)
#define RTC_CTRL_GET_TIME   (1u << 6)
#define RTC_CTRL_READSEL    (1u << 7)
#define RTC_STATUS_ALARM    (1u << 6)
#define RTC_STATUS_POWER_UP (1u << 7)
#define RTC_INT_ALARM_EN    (1u << 3)

/* fuel gauge */
#define FG_ADC_CONFIG0      0x50
#define FG_GG_STS           0x57
#define FG_GG_STS_BAT_CON   (1u << 4)
#define FG_RELAX_VOL1_H     0x5a
#define FG_RELAX_VOL2_H     0x5c
#define FG_RELAX_CUR1_H     0x5e
#define FG_RELAX_CUR2_H     0x60
#define FG_OCV_VOL_H        0x63
#define FG_OCV_VOL0_H       0x65
#define FG_OCV_CUR_H        0x67
#define FG_OCV_CUR0_H       0x69
#define FG_PWRON_VOL_H      0x6b
#define FG_PWRON_CUR_H      0x6d
#define FG_Q_INIT_H3        0x70
#define FG_Q_INIT_L0        0x73
#define FG_Q_PRESS_H3       0x74
#define FG_Q_PRESS_L0       0x77
#define FG_BAT_VOL_H        0x78
#define FG_BAT_CUR_H        0x7a
#define FG_BAT_TS_H         0x7c
#define FG_USB_VOL_H        0x7e
#define FG_SYS_VOL_H        0x80
#define FG_VCALIB0_H        0x93
#define FG_VCALIB1_H        0x95
#define FG_LAST             0xb0

/* power */
#define POWER_EN_REG0       0xb1
#define POWER_EN_REG3       0xb4
#define BUCK1_ON_VSEL       0xbb
#define BUCK2_ON_VSEL       0xbe
#define BUCK3_ON_VSEL       0xc1
#define BUCK4_ON_VSEL       0xc4
#define LDO_ON_VSEL(i)      (0xcc + (i) * 2)

#define CHRG_STS            0xeb
#define ID_MSB              0xed
#define ID_LSB              0xee
#define SYS_STS             0xf0
#define SYS_STS_PLUG_IN     (1u << 6)
#define SYS_CFG3            0xf4
#define SYS_CFG3_DEV_OFF    (1u << 0)
#define SYS_CFG3_SLPPIN_SHIFT 3
#define SYS_CFG3_SLPPOL_H   (1u << 5)
#define ON_SOURCE           0xf5
#define OFF_SOURCE          0xf6
#define INT_STS0            0xf8
#define INT_MSK0            0xf9
#define INT_STS1            0xfa
#define INT_MSK1            0xfb
#define INT_STS2            0xfc
#define INT_MSK2            0xfd
#define GPIO_INT_CFG        0xfe
#define GPIO_INT_POL_H      (1u << 1)

#define IRQ0_PWRON_FALL     (1u << 0)
#define IRQ0_PWRON_RISE     (1u << 1)
#define IRQ0_RTC_ALARM      (1u << 5)
#define IRQ1_PLUG_IN        (1u << 0)
#define IRQ1_PLUG_OUT       (1u << 1)

#define CHRG_STS_BAT_EXS    (1u << 7)
#define CHG_STS_CC_OR_CV    3

/*
 * ADC scaling.  The voltage ADC is calibrated by VCALIB0 (2300 mV) and
 * VCALIB1 (4025 mV); we pick 4 codes per mV.  Currents use the
 * 172 uA/LSB (at 10 mOhm sense resistor) scale of the driver.
 */
#define VCALIB0_CODE        (2300 * 4)
#define VCALIB1_CODE        (4025 * 4)

/* --- RTC calendar -------------------------------------------------- */

static int rk817_days_in_month(int mon, int year)
{
    static const int days[12] = {31, 28, 31, 30, 31, 30,
                                 31, 31, 30, 31, 31 /* Nov! */, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

    if (mon == 2 && leap) {
        return 29;
    }
    return days[mon - 1];
}

static void rk817_date_add(RK817Date *d, int64_t secs)
{
    int64_t days;
    int64_t tod = d->hour * 3600 + d->min * 60 + d->sec + secs;

    days = tod / 86400;
    tod %= 86400;
    if (tod < 0) {
        tod += 86400;
        days--;
    }
    d->hour = tod / 3600;
    d->min = (tod / 60) % 60;
    d->sec = tod % 60;
    d->wday = (int)(((d->wday + days) % 7 + 7) % 7);
    while (days > 0) {
        int dim = rk817_days_in_month(d->mon, d->year);
        int left = dim - d->mday + 1;

        if (days < left) {
            d->mday += days;
            break;
        }
        days -= left;
        d->mday = 1;
        if (++d->mon > 12) {
            d->mon = 1;
            d->year++;
        }
    }
}

static void rk817_rtc_now(RK817State *s, RK817Date *d)
{
    *d = s->rtc_base;
    if (!s->rtc_stopped) {
        int64_t now = qemu_clock_get_ns(rtc_clock);

        rk817_date_add(d, (now - s->rtc_base_ns) / NANOSECONDS_PER_SECOND);
    }
}

static void rk817_rtc_rebase(RK817State *s)
{
    int64_t now = qemu_clock_get_ns(rtc_clock);
    int64_t secs;

    if (!s->rtc_stopped) {
        secs = (now - s->rtc_base_ns) / NANOSECONDS_PER_SECOND;
        rk817_date_add(&s->rtc_base, secs);
        s->rtc_base_ns += secs * NANOSECONDS_PER_SECOND;
    }
}

static void rk817_rtc_init_time(RK817State *s)
{
    struct tm tm;
    struct tm ref = { .tm_year = 116, .tm_mon = 0, .tm_mday = 1 };
    int64_t host, ref_secs;

    qemu_get_timedate(&tm, 0);
    host = mktimegm(&tm);
    ref_secs = mktimegm(&ref);

    /* the RTC is assumed to have been set by an OS at 2016-01-01 */
    s->rtc_base = (RK817Date) {
        .sec = 0, .min = 0, .hour = 0, .mday = 1, .mon = 1, .year = 2016,
        .wday = 5,      /* Friday */
    };
    if (host > ref_secs) {
        rk817_date_add(&s->rtc_base, host - ref_secs);
    }
    s->rtc_base_ns = qemu_clock_get_ns(rtc_clock);
    s->rtc_stopped = false;
}

static uint8_t rk817_rtc_field(const RK817Date *d, int reg)
{
    switch (reg) {
    case 0:
        return to_bcd(d->sec);
    case 1:
        return to_bcd(d->min);
    case 2:
        return to_bcd(d->hour);
    case 3:
        return to_bcd(d->mday);
    case 4:
        return to_bcd(d->mon);
    case 5:
        return to_bcd(d->year % 100);
    case 6:
        return to_bcd(d->wday);
    }
    return 0;
}

static void rk817_rtc_set_field(RK817Date *d, int reg, uint8_t v)
{
    int val = from_bcd(v);

    switch (reg) {
    case 0:
        d->sec = MIN(val, 59);
        break;
    case 1:
        d->min = MIN(val, 59);
        break;
    case 2:
        d->hour = MIN(from_bcd(v & 0x3f), 23);
        break;
    case 3:
        d->mday = MAX(MIN(val, 31), 1);
        break;
    case 4:
        d->mon = MAX(MIN(val, 12), 1);
        break;
    case 5:
        d->year = 2000 + val;
        break;
    case 6:
        d->wday = val % 7;
        break;
    }
}

/* --- interrupts ------------------------------------------------------ */

static void rk817_update_irq(RK817State *s)
{
    bool active = (s->regs[INT_STS0] & ~s->regs[INT_MSK0]) ||
                  (s->regs[INT_STS1] & ~s->regs[INT_MSK1]) ||
                  (s->regs[INT_STS2] & ~s->regs[INT_MSK2]);
    bool pol_high = s->regs[GPIO_INT_CFG] & GPIO_INT_POL_H;
    bool level = pol_high ? active : !active;

    s->irq_level = level;
    qemu_set_irq(s->irq, level);
}

static void rk817_rtc_check_alarm(RK817State *s)
{
    RK817Date now;
    bool match = true;

    if (!(s->regs[RTC_INT] & RTC_INT_ALARM_EN)) {
        return;
    }
    rk817_rtc_now(s, &now);
    for (int i = 0; i < 6; i++) {
        if (s->regs[RTC_ALARM_SECONDS + i] != rk817_rtc_field(&now, i)) {
            match = false;
            break;
        }
    }
    if (match) {
        s->regs[RTC_STATUS] |= RTC_STATUS_ALARM;
        s->regs[INT_STS0] |= IRQ0_RTC_ALARM;
        rk817_update_irq(s);
    }
}

static void rk817_rtc_tick(void *opaque)
{
    RK817State *s = opaque;

    rk817_rtc_check_alarm(s);
    if (s->regs[RTC_INT] & RTC_INT_ALARM_EN) {
        timer_mod(s->rtc_timer, qemu_clock_get_ms(rtc_clock) + 500);
    }
}

/* --- fuel gauge ------------------------------------------------------ */

static void rk817_put16(RK817State *s, int reg, uint16_t v)
{
    s->regs[reg] = v >> 8;
    s->regs[reg + 1] = v & 0xff;
}

static int32_t rk817_cur_to_adc(int32_t ma)
{
    /* ADC_TO_CURRENT(adc) = adc * 172 / 1000 (10 mOhm, res_div 1) */
    return (int32_t)((int64_t)ma * 1000 / 172);
}

static void rk817_fg_update_q(RK817State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = now - s->q_last_ns;
    int64_t secs = elapsed / NANOSECONDS_PER_SECOND;

    if (secs > 0) {
        s->q_press += (int64_t)rk817_cur_to_adc(s->bat_ma) * secs;
        if (s->q_press < 0) {
            s->q_press = 0;
        }
        if (s->q_press > 0x7fffffff) {
            s->q_press = 0x7fffffff;
        }
        s->q_last_ns += secs * NANOSECONDS_PER_SECOND;
    }
}

static void rk817_fg_refresh(RK817State *s)
{
    uint32_t sys_mv = s->dc_in ? 5000 : s->bat_mv;
    uint16_t vcode = s->battery_present ? s->bat_mv * 4 : 0;
    uint16_t ccode = (uint16_t)rk817_cur_to_adc(s->bat_ma);

    rk817_put16(s, FG_BAT_VOL_H, vcode);
    rk817_put16(s, FG_BAT_CUR_H, ccode);
    rk817_put16(s, FG_SYS_VOL_H, sys_mv * 4 * 46 / 60);
    rk817_put16(s, FG_USB_VOL_H, s->dc_in ? s->usb_mv * 4 * 46 / 60 : 0);
    rk817_put16(s, FG_BAT_TS_H, 0);

    s->regs[CHRG_STS] = (s->regs[CHRG_STS] & 0x0f) |
                        (s->battery_present ? CHRG_STS_BAT_EXS : 0) |
                        ((s->dc_in ? CHG_STS_CC_OR_CV : 0) << 4);
    if (s->dc_in) {
        s->regs[SYS_STS] |= SYS_STS_PLUG_IN;
    } else {
        s->regs[SYS_STS] &= ~SYS_STS_PLUG_IN;
    }

    rk817_fg_update_q(s);
    s->regs[FG_Q_PRESS_H3] = (s->q_press >> 24) | (s->q_valid ? 0 : 0x80);
    s->regs[FG_Q_PRESS_H3 + 1] = s->q_press >> 16;
    s->regs[FG_Q_PRESS_H3 + 2] = s->q_press >> 8;
    s->regs[FG_Q_PRESS_L0] = s->q_press;
}

static void rk817_fg_power_on(RK817State *s)
{
    uint16_t vcode = s->bat_mv * 4;
    uint16_t ccode = (uint16_t)rk817_cur_to_adc(s->bat_ma);

    rk817_put16(s, FG_VCALIB0_H, VCALIB0_CODE);
    rk817_put16(s, FG_VCALIB1_H, VCALIB1_CODE);
    /* sampled values captured by the gauge at power on / relax */
    rk817_put16(s, FG_PWRON_VOL_H, vcode);
    rk817_put16(s, FG_PWRON_CUR_H, ccode);
    rk817_put16(s, FG_OCV_VOL_H, vcode);
    rk817_put16(s, FG_OCV_VOL0_H, vcode);
    rk817_put16(s, FG_OCV_CUR_H, ccode);
    rk817_put16(s, FG_OCV_CUR0_H, ccode);
    rk817_put16(s, FG_RELAX_VOL1_H, vcode);
    rk817_put16(s, FG_RELAX_VOL2_H, vcode);
    rk817_put16(s, FG_RELAX_CUR1_H, ccode);
    rk817_put16(s, FG_RELAX_CUR2_H, ccode);
    /* battery freshly connected: invalid capacity, BAT_CON set */
    s->regs[FG_GG_STS] = FG_GG_STS_BAT_CON;
    s->q_press = 0;
    s->q_valid = false;                 /* Q_PRESS_H3 reads CAP_INVALID */
    s->q_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* --- register access ------------------------------------------------ */

static void rk817_power_defaults(RK817State *s)
{
    /*
     * Power-on defaults of the RK817 OTP as used on the ODROID-GO Advance:
     * DCDC1-4 on; LDO2/3/4 (SoC 1.8V/1.0V, PMU 3.3V) on; LDO5/6 (SD) on;
     * LDO9 (WiFi) and BOOST on.
     */
    s->regs[POWER_EN_REG0 + 0] = 0x0f;
    s->regs[POWER_EN_REG0 + 1] = 0x0e;
    s->regs[POWER_EN_REG0 + 2] = 0x03;
    s->regs[POWER_EN_REG0 + 3] = 0x03;
    s->regs[0xb5] = 0x0f;               /* POWER_SLP_EN */
    s->regs[BUCK1_ON_VSEL] = 0x28;      /* 1.0 V */
    s->regs[BUCK2_ON_VSEL] = 0x28;      /* 1.0 V */
    s->regs[BUCK3_ON_VSEL] = 0x28;
    s->regs[BUCK4_ON_VSEL] = 0x62;      /* 3.3 V */
    s->regs[LDO_ON_VSEL(0)] = 0x10;     /* 1.0 V */
    s->regs[LDO_ON_VSEL(1)] = 0x30;     /* 1.8 V */
    s->regs[LDO_ON_VSEL(2)] = 0x10;     /* 1.0 V */
    s->regs[LDO_ON_VSEL(3)] = 0x6c;     /* 3.3 V */
    s->regs[LDO_ON_VSEL(4)] = 0x6c;     /* 3.3 V */
    s->regs[LDO_ON_VSEL(5)] = 0x6c;     /* 3.3 V */
    s->regs[LDO_ON_VSEL(6)] = 0x6c;     /* 3.3 V */
    s->regs[LDO_ON_VSEL(7)] = 0x58;     /* 2.8 V */
    s->regs[LDO_ON_VSEL(8)] = 0x6c;     /* 3.3 V */
}

static void rk817_regs_reset(RK817State *s, bool cold)
{
    uint8_t keep[256];

    memcpy(keep, s->regs, sizeof(keep));
    memset(s->regs, 0, sizeof(s->regs));
    if (!cold) {
        /* RTC and gauge are always powered */
        memcpy(&s->regs[0x00], &keep[0x00], 0x12);
        memcpy(&s->regs[FG_ADC_CONFIG0], &keep[FG_ADC_CONFIG0],
               FG_LAST - FG_ADC_CONFIG0 + 1);
    }
    rk817_power_defaults(s);
    s->regs[ID_MSB] = 0x81;
    s->regs[ID_LSB] = 0x70;
    s->regs[ON_SOURCE] = 0x80;          /* power on by PWRON key */
    s->regs[OFF_SOURCE] = 0x00;
    s->regs[INT_MSK0] = 0x00;
    s->regs[INT_MSK1] = 0x00;
    s->regs[INT_MSK2] = 0x00;
    s->regs[GPIO_INT_CFG] = 0x00;       /* INT active low */
    if (cold) {
        s->regs[RTC_STATUS] = RTC_STATUS_POWER_UP;
        rk817_rtc_init_time(s);
        rk817_rtc_now(s, &s->rtc_latch);
        rk817_fg_power_on(s);
    }
    s->ptr = 0;
    s->ptr_set = false;
}

static uint8_t rk817_reg_read(RK817State *s, uint8_t reg)
{
    if (reg <= RTC_WEEKS) {
        RK817Date d;

        if (s->regs[RTC_CTRL] & RTC_CTRL_READSEL) {
            d = s->rtc_latch;
        } else {
            rk817_rtc_now(s, &d);
        }
        return rk817_rtc_field(&d, reg);
    }
    if ((reg >= FG_Q_PRESS_H3 && reg <= FG_SYS_VOL_H + 1) ||
        reg == CHRG_STS || reg == SYS_STS) {
        rk817_fg_refresh(s);
    }
    return s->regs[reg];
}

static void rk817_sleep_pin_action(RK817State *s)
{
    unsigned func = (s->regs[SYS_CFG3] >> SYS_CFG3_SLPPIN_SHIFT) & 3;

    switch (func) {
    case 2:     /* power down */
        qemu_log_mask(CPU_LOG_RESET, "rk817: SLEEP pin power down\n");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        break;
    case 3:     /* reset */
        qemu_log_mask(CPU_LOG_RESET, "rk817: SLEEP pin reset\n");
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        break;
    default:    /* none / sleep: regulators switch to sleep config */
        break;
    }
}

static void rk817_reg_write(RK817State *s, uint8_t reg, uint8_t val)
{
    uint8_t old = s->regs[reg];

    if (reg <= RTC_WEEKS) {
        rk817_rtc_rebase(s);
        rk817_rtc_set_field(&s->rtc_base, reg, val);
        return;
    }
    switch (reg) {
    case RTC_CTRL:
        if ((val & RTC_CTRL_STOP) && !(old & RTC_CTRL_STOP)) {
            rk817_rtc_now(s, &s->rtc_base);
            s->rtc_stopped = true;
        } else if (!(val & RTC_CTRL_STOP) && (old & RTC_CTRL_STOP)) {
            s->rtc_base_ns = qemu_clock_get_ns(rtc_clock);
            s->rtc_stopped = false;
        }
        if ((val & RTC_CTRL_GET_TIME) && !(old & RTC_CTRL_GET_TIME)) {
            rk817_rtc_now(s, &s->rtc_latch);
        }
        s->regs[reg] = val;
        return;
    case RTC_STATUS:
        s->regs[reg] &= ~(val & 0xfe);
        return;
    case RTC_INT:
        s->regs[reg] = val;
        if (val & RTC_INT_ALARM_EN) {
            timer_mod(s->rtc_timer, qemu_clock_get_ms(rtc_clock) + 500);
        } else {
            timer_del(s->rtc_timer);
        }
        return;
    case FG_Q_INIT_L0:
        s->regs[reg] = val;
        s->q_press = ((uint32_t)s->regs[FG_Q_INIT_H3] << 24) |
                     ((uint32_t)s->regs[FG_Q_INIT_H3 + 1] << 16) |
                     ((uint32_t)s->regs[FG_Q_INIT_H3 + 2] << 8) | val;
        s->q_press &= 0x7fffffff;
        s->q_valid = true;
        s->q_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        return;
    case POWER_EN_REG0 ... POWER_EN_REG3: {
        uint8_t mask = val >> 4;

        s->regs[reg] = (old & ~mask & 0x0f) | (val & mask & 0x0f);
        return;
    }
    case SYS_CFG3:
        s->regs[reg] = val;
        if (val & SYS_CFG3_DEV_OFF) {
            qemu_log_mask(CPU_LOG_RESET, "rk817: DEV_OFF, powering off\n");
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
        return;
    case INT_STS0:
    case INT_STS1:
    case INT_STS2:
        s->regs[reg] &= ~val;
        rk817_update_irq(s);
        return;
    case INT_MSK0:
    case INT_MSK1:
    case INT_MSK2:
    case GPIO_INT_CFG:
        s->regs[reg] = val;
        rk817_update_irq(s);
        return;
    case ID_MSB:
    case ID_LSB:
    case CHRG_STS:
    case ON_SOURCE:
    case OFF_SOURCE:
        return;
    case FG_Q_PRESS_H3 ... FG_Q_PRESS_L0:
    case FG_BAT_VOL_H ... FG_SYS_VOL_H + 1:
        return;
    case SYS_STS:
        s->regs[reg] = (val & ~SYS_STS_PLUG_IN) | (old & SYS_STS_PLUG_IN);
        return;
    }
    s->regs[reg] = val;
}

static int rk817_event(I2CSlave *i2c, enum i2c_event event)
{
    RK817State *s = RK817(i2c);

    if (event == I2C_START_SEND) {
        s->ptr_set = false;
    }
    return 0;
}

static uint8_t rk817_recv(I2CSlave *i2c)
{
    RK817State *s = RK817(i2c);

    return rk817_reg_read(s, s->ptr++);
}

static int rk817_send(I2CSlave *i2c, uint8_t data)
{
    RK817State *s = RK817(i2c);

    if (!s->ptr_set) {
        s->ptr = data;
        s->ptr_set = true;
        return 0;
    }
    rk817_reg_write(s, s->ptr++, data);
    return 0;
}

void rk817_set_pwrkey(RK817State *s, bool pressed)
{
    if (pressed == s->pwrkey) {
        return;
    }
    s->pwrkey = pressed;
    s->regs[INT_STS0] |= pressed ? IRQ0_PWRON_FALL : IRQ0_PWRON_RISE;
    rk817_update_irq(s);
}

void rk817_set_dc_in(RK817State *s, bool present)
{
    if (present == s->dc_in) {
        return;
    }
    s->dc_in = present;
    s->regs[INT_STS1] |= present ? IRQ1_PLUG_IN : IRQ1_PLUG_OUT;
    rk817_fg_refresh(s);
    rk817_update_irq(s);
}

static void rk817_set_sleep_pin(void *opaque, int n, int level)
{
    RK817State *s = opaque;
    bool pol_high = s->regs[SYS_CFG3] & SYS_CFG3_SLPPOL_H;
    bool was_active = s->sleep_pin == pol_high;

    s->sleep_pin = level;
    if (!was_active && (s->sleep_pin == pol_high)) {
        rk817_sleep_pin_action(s);
    }
}

static void rk817_reset_hold(Object *obj, ResetType type)
{
    RK817State *s = RK817(obj);

    rk817_regs_reset(s, !s->powered);
    s->powered = true;
}

static void rk817_reset_exit(Object *obj, ResetType type)
{
    RK817State *s = RK817(obj);

    rk817_update_irq(s);
}

static void rk817_realize(DeviceState *dev, Error **errp)
{
    RK817State *s = RK817(dev);

    s->rtc_timer = timer_new_ms(rtc_clock, rk817_rtc_tick, s);
    qdev_init_gpio_out_named(dev, &s->irq, "irq", 1);
    qdev_init_gpio_in_named(dev, rk817_set_sleep_pin, "sleep", 1);
}

static const Property rk817_props[] = {
    DEFINE_PROP_UINT32("battery-mv", RK817State, bat_mv, 3900),
    DEFINE_PROP_INT32("battery-ma", RK817State, bat_ma, 0),
    DEFINE_PROP_UINT32("usb-mv", RK817State, usb_mv, 5000),
    DEFINE_PROP_BOOL("dc-in", RK817State, dc_in, false),
    DEFINE_PROP_BOOL("battery-present", RK817State, battery_present, true),
};

static const VMStateDescription vmstate_rk817_date = {
    .name = "rk817-date",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(sec, RK817Date),
        VMSTATE_INT32(min, RK817Date),
        VMSTATE_INT32(hour, RK817Date),
        VMSTATE_INT32(mday, RK817Date),
        VMSTATE_INT32(mon, RK817Date),
        VMSTATE_INT32(year, RK817Date),
        VMSTATE_INT32(wday, RK817Date),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_rk817 = {
    .name = TYPE_RK817,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, RK817State),
        VMSTATE_UINT8_ARRAY(regs, RK817State, 256),
        VMSTATE_UINT8(ptr, RK817State),
        VMSTATE_BOOL(ptr_set, RK817State),
        VMSTATE_BOOL(sleep_pin, RK817State),
        VMSTATE_BOOL(pwrkey, RK817State),
        VMSTATE_STRUCT(rtc_base, RK817State, 1, vmstate_rk817_date, RK817Date),
        VMSTATE_INT64(rtc_base_ns, RK817State),
        VMSTATE_BOOL(rtc_stopped, RK817State),
        VMSTATE_STRUCT(rtc_latch, RK817State, 1, vmstate_rk817_date,
                       RK817Date),
        VMSTATE_INT64(q_press, RK817State),
        VMSTATE_BOOL(q_valid, RK817State),
        VMSTATE_BOOL(powered, RK817State),
        VMSTATE_INT64(q_last_ns, RK817State),
        VMSTATE_BOOL(dc_in, RK817State),
        VMSTATE_TIMER_PTR(rtc_timer, RK817State),
        VMSTATE_END_OF_LIST()
    }
};

static void rk817_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *isc = I2C_SLAVE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = rk817_realize;
    dc->vmsd = &vmstate_rk817;
    device_class_set_props(dc, rk817_props);
    rc->phases.hold = rk817_reset_hold;
    rc->phases.exit = rk817_reset_exit;
    isc->event = rk817_event;
    isc->recv = rk817_recv;
    isc->send = rk817_send;
}

static const TypeInfo rk817_info = {
    .name = TYPE_RK817,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(RK817State),
    .class_init = rk817_class_init,
};

static void rk817_register_types(void)
{
    type_register_static(&rk817_info);
}

type_init(rk817_register_types)
