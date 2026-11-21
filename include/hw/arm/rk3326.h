/*
 * Rockchip RK3326 SoC emulation
 *
 * The RK3326 is the PX30 die without VOP lite, GMAC and with a slightly
 * different OPP set: 4x Cortex-A35, GIC-400, Mali-G31.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_RK3326_H
#define HW_ARM_RK3326_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gic.h"
#include "hw/core/or-irq.h"
#include "target/arm/cpu.h"
#include "hw/misc/rockchip_grf.h"
#include "hw/misc/rk3326_cru.h"
#include "hw/misc/rk3326_pmu.h"
#include "hw/misc/rockchip_pwm.h"
#include "hw/misc/rockchip_tsadc.h"
#include "hw/misc/rockchip_iommu.h"
#include "hw/nvram/rockchip_otp.h"
#include "hw/gpio/rockchip_gpio.h"
#include "hw/i2c/rockchip_i2c.h"
#include "hw/adc/rockchip_saradc.h"
#include "hw/timer/rockchip_timer.h"
#include "hw/watchdog/dw_wdt.h"
#include "hw/sd/dw_mmc.h"
#include "hw/ssi/rockchip_sfc.h"
#include "hw/display/rockchip_vop.h"
#include "hw/display/dw_mipi_dsi.h"

#define TYPE_RK3326 "rk3326"
OBJECT_DECLARE_SIMPLE_TYPE(RK3326State, RK3326)

#define RK3326_NUM_CPUS     4
#define RK3326_NUM_UARTS    6
#define RK3326_NUM_I2C      4
#define RK3326_NUM_GPIO     4
#define RK3326_NUM_PWM      2       /* blocks of 4 channels */
#define RK3326_NUM_IOMMU    6
#define RK3326_GIC_NUM_SPI  128

#define RK3326_SDRAM_BASE   0x00000000ULL
#define RK3326_SDRAM_MAX    0xff000000ULL

/* Memory map (from rk3326.dtsi/px30.dtsi of the Hardkernel 4.4 kernel) */
enum {
    RK3326_DEV_PMU,
    RK3326_DEV_PMUGRF,
    RK3326_DEV_PMU_SRAM,
    RK3326_DEV_UART0,
    RK3326_DEV_GPIO0,
    RK3326_DEV_I2S0,
    RK3326_DEV_I2S1,
    RK3326_DEV_I2S2,
    RK3326_DEV_PDM,
    RK3326_DEV_CRYPTO,
    RK3326_DEV_SRAM,
    RK3326_DEV_GIC_DIST,
    RK3326_DEV_GIC_CPU,
    RK3326_DEV_GIC_HYP,
    RK3326_DEV_GIC_VCPU,
    RK3326_DEV_GRF,
    RK3326_DEV_CORE_GRF,
    RK3326_DEV_UART1,
    RK3326_DEV_UART2,
    RK3326_DEV_UART3,
    RK3326_DEV_UART4,
    RK3326_DEV_UART5,
    RK3326_DEV_I2C0,
    RK3326_DEV_I2C1,
    RK3326_DEV_I2C2,
    RK3326_DEV_I2C3,
    RK3326_DEV_SPI0,
    RK3326_DEV_SPI1,
    RK3326_DEV_WDT,
    RK3326_DEV_PWM0,
    RK3326_DEV_PWM1,
    RK3326_DEV_TIMER,
    RK3326_DEV_STIMER,
    RK3326_DEV_DMAC,
    RK3326_DEV_GPIO1,
    RK3326_DEV_GPIO2,
    RK3326_DEV_GPIO3,
    RK3326_DEV_TSADC,
    RK3326_DEV_SARADC,
    RK3326_DEV_OTP,
    RK3326_DEV_DDR,
    RK3326_DEV_CRU,
    RK3326_DEV_CPU_BOOST,
    RK3326_DEV_PMUCRU,
    RK3326_DEV_USB2PHY_GRF,
    RK3326_DEV_VIDEO_PHY,
    RK3326_DEV_MIPI_DPHY_RX0,
    RK3326_DEV_USB_OTG,
    RK3326_DEV_USB_EHCI,
    RK3326_DEV_USB_OHCI,
    RK3326_DEV_GMAC,
    RK3326_DEV_SDMMC,
    RK3326_DEV_SDIO,
    RK3326_DEV_EMMC,
    RK3326_DEV_SFC,
    RK3326_DEV_NANDC,
    RK3326_DEV_GPU,
    RK3326_DEV_VPU,
    RK3326_DEV_DSI,
    RK3326_DEV_VOPB,
    RK3326_DEV_VOPB_MMU,
    RK3326_DEV_VOPL,
    RK3326_DEV_RGA,
    RK3326_DEV_CIF,
    RK3326_DEV_ISP,
    RK3326_DEV_QOS,
    RK3326_DEV_DMC,
    RK3326_DEV_BOOTROM,
};

/* Shared Peripheral Interrupt numbers */
enum {
    RK3326_SPI_DMAC0        = 1,
    RK3326_SPI_DMAC1        = 2,
    RK3326_SPI_GPIO0        = 3,
    RK3326_SPI_GPIO1        = 4,
    RK3326_SPI_GPIO2        = 5,
    RK3326_SPI_GPIO3        = 6,
    RK3326_SPI_I2C0         = 7,
    RK3326_SPI_I2C1         = 8,
    RK3326_SPI_I2C2         = 9,
    RK3326_SPI_I2C3         = 10,
    RK3326_SPI_I2S0         = 12,
    RK3326_SPI_I2S1         = 13,
    RK3326_SPI_I2S2         = 14,
    RK3326_SPI_UART0        = 15,
    RK3326_SPI_UART1        = 16,
    RK3326_SPI_UART2        = 17,
    RK3326_SPI_UART3        = 18,
    RK3326_SPI_UART4        = 19,
    RK3326_SPI_UART5        = 20,
    RK3326_SPI_SPI0         = 26,
    RK3326_SPI_SPI1         = 27,
    RK3326_SPI_TIMER0       = 30,
    RK3326_SPI_TSADC        = 36,
    RK3326_SPI_WDT          = 37,
    RK3326_SPI_GPU_JOB      = 45,
    RK3326_SPI_HEVC_MMU     = 50,
    RK3326_SPI_EMMC         = 53,
    RK3326_SPI_SDMMC        = 54,
    RK3326_SPI_SDIO         = 55,
    RK3326_SPI_SFC          = 56,
    RK3326_SPI_EHCI         = 60,
    RK3326_SPI_OHCI         = 61,
    RK3326_SPI_OTG          = 62,
    RK3326_SPI_VIP          = 69,
    RK3326_SPI_ISP          = 70,
    RK3326_SPI_DSI          = 75,
    RK3326_SPI_VOPB         = 77,
    RK3326_SPI_VPU_MMU      = 81,
    RK3326_SPI_SARADC       = 84,
    RK3326_SPI_PMU_CORE0    = 100,
};

struct RK3326State {
    SysBusDevice parent_obj;

    ARMCPU cpus[RK3326_NUM_CPUS];
    GICState gic;

    RockchipGRFState grf;
    RockchipGRFState pmugrf;
    RockchipGRFState core_grf;
    RockchipGRFState cpu_boost;
    RockchipGRFState usb2phy_grf;
    RockchipGRFState video_phy;
    RK3326CRUState cru;
    RK3326CRUState pmucru;
    RK3326PMUState pmu;
    RockchipGPIOState gpio[RK3326_NUM_GPIO];
    RockchipI2CState i2c[RK3326_NUM_I2C];
    RockchipSARADCState saradc;
    RockchipTimerState timer;
    RockchipTimerState stimer;
    DWWDTState wdt;
    RockchipPWMState pwm[RK3326_NUM_PWM];
    RockchipTSADCState tsadc;
    RockchipOTPState otp;
    DWMMCState sdmmc;
    DWMMCState sdio;
    DWMMCState emmc;
    RockchipSFCState sfc;
    RockchipVOPState vopb;
    RockchipIOMMUState iommu[RK3326_NUM_IOMMU];
    DWMipiDsiState dsi;
    OrIRQState dmac_irq;

    MemoryRegion uart_container[RK3326_NUM_UARTS];
    MemoryRegion uart_dw[RK3326_NUM_UARTS];
    struct SerialMM *uart[RK3326_NUM_UARTS];

    MemoryRegion sram;
    MemoryRegion pmu_sram;

    /* properties */
    uint64_t ram_size;      /* for the DRAM info in PMUGRF OS_REG2/3 */
    char *otp_serial;
    uint32_t display_rotation;
    bool has_el3;
};

#endif
