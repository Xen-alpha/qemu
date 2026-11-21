/*
 * Rockchip General Register File (GRF / PMUGRF / syscon) model
 *
 * Most Rockchip "syscon" style register blocks use the so called
 * HIWORD_UPDATE convention: bits [31:16] of a write are a write-enable
 * mask for bits [15:0].  A handful of registers are plain 32-bit
 * registers (e.g. PMUGRF OS_REGx), some are read-only status registers
 * and some survive a warm (software/watchdog) reset.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_ROCKCHIP_GRF_H
#define HW_MISC_ROCKCHIP_GRF_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ROCKCHIP_GRF "rockchip-grf"
OBJECT_DECLARE_SIMPLE_TYPE(RockchipGRFState, ROCKCHIP_GRF)

/* Per-register attributes */
#define RK_GRF_F_PLAIN    (1u << 0)   /* full 32-bit write, no hiword mask */
#define RK_GRF_F_RO       (1u << 1)   /* writes ignored */
#define RK_GRF_F_KEEP     (1u << 2)   /* preserved across system reset */

struct RockchipGRFState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t size;          /* in bytes, property */
    char *name;             /* region name, property */

    uint32_t nregs;
    uint32_t *regs;
    uint32_t *reset_val;
    uint8_t *flags;
    bool powered_on;        /* cold boot seen once (for RK_GRF_F_KEEP) */

    /* optional PVTM (process/voltage/temperature monitor) channel */
    bool has_pvtm;
    uint32_t pvtm_con;      /* [0] start, [1] enable (hiword) */
    uint32_t pvtm_sta;      /* [0] freq done; sta + 4: frequency count */
    uint32_t pvtm_rate;     /* ring oscillator counts per 24 MHz cycle *1000 */
};

/*
 * Configure a register.  Must be called after realize and before the
 * first system reset (i.e. from the SoC realize function).
 */
void rockchip_grf_set_reg(RockchipGRFState *s, hwaddr offset,
                          uint32_t reset_value, unsigned flags);
void rockchip_grf_set_range_flags(RockchipGRFState *s, hwaddr start,
                                  hwaddr end, unsigned flags);
uint32_t rockchip_grf_read_reg(RockchipGRFState *s, hwaddr offset);

/*
 * Add a PVTM channel: CON at @con (start/enable, CAL at con + 4),
 * STA at @sta (done flag, FREQ count at sta + 4).  @counts_per_ms is the
 * ring oscillator count for a 1 ms measurement window.
 */
void rockchip_grf_add_pvtm(RockchipGRFState *s, hwaddr con, hwaddr sta,
                           uint32_t counts_per_ms);

#endif
