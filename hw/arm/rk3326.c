/*
 * Rockchip RK3326 SoC emulation
 *
 * Memory map, interrupt numbers and peripheral set follow
 * arch/arm64/boot/dts/rockchip/{px30,rk3326}.dtsi of the Hardkernel
 * odroidgoA-4.4.y kernel and the PX30 support of the Hardkernel
 * odroidgoa-v2017.09 u-boot.
 *
 * Blocks without a functional model are mapped as "unimplemented
 * devices" (RAZ/WI, logged with -d unimp) so that probing guests do not
 * take external aborts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/arm/rk3326.h"
#include "hw/arm/linux-boot-if.h"
#include "hw/char/serial-mm.h"
#include "hw/char/serial.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/unimp.h"
#include "hw/usb/hcd-ehci.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"

static const hwaddr rk3326_memmap[] = {
    [RK3326_DEV_PMU]            = 0xff000000,
    [RK3326_DEV_PMUGRF]         = 0xff010000,
    [RK3326_DEV_PMU_SRAM]       = 0xff020000,
    [RK3326_DEV_UART0]          = 0xff030000,
    [RK3326_DEV_GPIO0]          = 0xff040000,
    [RK3326_DEV_I2S0]           = 0xff060000,
    [RK3326_DEV_I2S1]           = 0xff070000,
    [RK3326_DEV_I2S2]           = 0xff080000,
    [RK3326_DEV_PDM]            = 0xff0a0000,
    [RK3326_DEV_CRYPTO]         = 0xff0b0000,
    [RK3326_DEV_SRAM]           = 0xff0e0000,
    [RK3326_DEV_GIC_DIST]       = 0xff131000,
    [RK3326_DEV_GIC_CPU]        = 0xff132000,
    [RK3326_DEV_GIC_HYP]        = 0xff134000,
    [RK3326_DEV_GIC_VCPU]       = 0xff136000,
    [RK3326_DEV_GRF]            = 0xff140000,
    [RK3326_DEV_CORE_GRF]       = 0xff148000,
    [RK3326_DEV_UART1]          = 0xff158000,
    [RK3326_DEV_UART2]          = 0xff160000,
    [RK3326_DEV_UART3]          = 0xff168000,
    [RK3326_DEV_UART4]          = 0xff170000,
    [RK3326_DEV_UART5]          = 0xff178000,
    [RK3326_DEV_I2C0]           = 0xff180000,
    [RK3326_DEV_I2C1]           = 0xff190000,
    [RK3326_DEV_I2C2]           = 0xff1a0000,
    [RK3326_DEV_I2C3]           = 0xff1b0000,
    [RK3326_DEV_SPI0]           = 0xff1d0000,
    [RK3326_DEV_SPI1]           = 0xff1d8000,
    [RK3326_DEV_WDT]            = 0xff1e0000,
    [RK3326_DEV_PWM0]           = 0xff200000,
    [RK3326_DEV_PWM1]           = 0xff208000,
    [RK3326_DEV_TIMER]          = 0xff210000,
    [RK3326_DEV_STIMER]         = 0xff220000,
    [RK3326_DEV_DMAC]           = 0xff240000,
    [RK3326_DEV_GPIO1]          = 0xff250000,
    [RK3326_DEV_GPIO2]          = 0xff260000,
    [RK3326_DEV_GPIO3]          = 0xff270000,
    [RK3326_DEV_TSADC]          = 0xff280000,
    [RK3326_DEV_SARADC]         = 0xff288000,
    [RK3326_DEV_OTP]            = 0xff290000,
    [RK3326_DEV_DDR]            = 0xff2a0000,
    [RK3326_DEV_CRU]            = 0xff2b0000,
    [RK3326_DEV_CPU_BOOST]      = 0xff2b8000,
    [RK3326_DEV_PMUCRU]         = 0xff2bc000,
    [RK3326_DEV_USB2PHY_GRF]    = 0xff2c0000,
    [RK3326_DEV_VIDEO_PHY]      = 0xff2e0000,
    [RK3326_DEV_MIPI_DPHY_RX0]  = 0xff2f0000,
    [RK3326_DEV_USB_OTG]        = 0xff300000,
    [RK3326_DEV_USB_EHCI]       = 0xff340000,
    [RK3326_DEV_USB_OHCI]       = 0xff350000,
    [RK3326_DEV_GMAC]           = 0xff360000,
    [RK3326_DEV_SDMMC]          = 0xff370000,
    [RK3326_DEV_SDIO]           = 0xff380000,
    [RK3326_DEV_EMMC]           = 0xff390000,
    [RK3326_DEV_SFC]            = 0xff3a0000,
    [RK3326_DEV_NANDC]          = 0xff3b0000,
    [RK3326_DEV_GPU]            = 0xff400000,
    [RK3326_DEV_VPU]            = 0xff440000,
    [RK3326_DEV_DSI]            = 0xff450000,
    [RK3326_DEV_VOPB]           = 0xff460000,
    [RK3326_DEV_VOPB_MMU]       = 0xff460f00,
    [RK3326_DEV_VOPL]           = 0xff470000,
    [RK3326_DEV_RGA]            = 0xff480000,
    [RK3326_DEV_CIF]            = 0xff490000,
    [RK3326_DEV_ISP]            = 0xff4a0000,
    [RK3326_DEV_QOS]            = 0xff518000,
    [RK3326_DEV_DMC]            = 0xff600000,
    [RK3326_DEV_BOOTROM]        = 0xffff0000,
};

static const struct {
    const char *name;
    int dev;
    hwaddr size;
} rk3326_unimp[] = {
    { "i2s0-8ch",       RK3326_DEV_I2S0,            0x1000 },
    { "i2s1-2ch",       RK3326_DEV_I2S1,            0x1000 },
    { "i2s2-2ch",       RK3326_DEV_I2S2,            0x1000 },
    { "pdm",            RK3326_DEV_PDM,             0x1000 },
    { "crypto",         RK3326_DEV_CRYPTO,          0x4000 },
    { "spi0",           RK3326_DEV_SPI0,            0x1000 },
    { "spi1",           RK3326_DEV_SPI1,            0x1000 },
    { "ddr-phy",        RK3326_DEV_DDR,             0x10000 },
    { "mipi-dphy-rx0",  RK3326_DEV_MIPI_DPHY_RX0,   0x4000 },
    { "gmac",           RK3326_DEV_GMAC,            0x10000 },
    { "nandc",          RK3326_DEV_NANDC,           0x4000 },
    { "gpu",            RK3326_DEV_GPU,             0x4000 },
    { "vpu",            RK3326_DEV_VPU,             0x10000 },
    { "vopl",           RK3326_DEV_VOPL,            0x1000 },
    { "rga",            RK3326_DEV_RGA,             0x1000 },
    { "cif",            RK3326_DEV_CIF,             0x1000 },
    { "isp",            RK3326_DEV_ISP,             0x10000 },
    { "qos",            RK3326_DEV_QOS,             0x48000 },
    { "dmc",            RK3326_DEV_DMC,             0x20000 },
    { "bootrom",        RK3326_DEV_BOOTROM,         0x8000 },
};

/* IOMMUs; the VOP one is iommu[0], the two HEVC banks share an IRQ */
static const struct {
    hwaddr base;
    hwaddr size;
    int irq;
} rk3326_iommus[RK3326_NUM_IOMMU] = {
    { 0xff460f00, 0x100, RK3326_SPI_VOPB },        /* vopb_mmu */
    { 0xff442800, 0x100, RK3326_SPI_VPU_MMU },     /* vpu_mmu */
    { 0xff440440, 0x40,  RK3326_SPI_HEVC_MMU },    /* hevc_mmu bank 0 */
    { 0xff440480, 0x40,  RK3326_SPI_HEVC_MMU },    /* hevc_mmu bank 1 */
    { 0xff490800, 0x100, RK3326_SPI_VIP },         /* vip_mmu */
    { 0xff4a8000, 0x100, RK3326_SPI_ISP },         /* isp_mmu */
};

static const hwaddr rk3326_uart_base[RK3326_NUM_UARTS] = {
    0xff030000, 0xff158000, 0xff160000, 0xff168000, 0xff170000, 0xff178000,
};

/*
 * QEMU serial port index -> UART.  serial0 is UART2, the Rockchip debug
 * UART used by the loader, u-boot and the kernel console.
 */
static const int rk3326_serial_map[RK3326_NUM_UARTS] = { 2, 1, 0, 3, 4, 5 };

static qemu_irq rk3326_spi(RK3326State *s, int n)
{
    return qdev_get_gpio_in(DEVICE(&s->gic), n);
}

/* --- DesignWare APB UART extension registers -------------------------- */

#define DW_UART_USR     0x7c
#define DW_UART_TFL     0x80
#define DW_UART_RFL     0x84
#define DW_UART_CPR     0xf4
#define DW_UART_UCV     0xf8
#define DW_UART_CTR     0xfc

static uint64_t dw_uart_ext_read(void *opaque, hwaddr offset, unsigned size)
{
    SerialMM *smm = opaque;
    bool rx = smm->serial.lsr & 0x01;     /* LSR.DR */

    switch (offset) {
    case DW_UART_USR:
        /* TX FIFO not full + empty, RX FIFO not empty */
        return 0x06 | (rx ? 0x08 : 0);
    case DW_UART_TFL:
        return 0;
    case DW_UART_RFL:
        return rx ? 1 : 0;
    case DW_UART_CPR:
        /* 32-bit APB, AFCE, THRE, FIFO access/stat, shadow, 16 byte FIFO */
        return 0x2 | (1 << 4) | (1 << 5) | (1 << 9) | (1 << 10) |
               (1 << 11) | (1 << 12) | (1 << 13) | (1 << 16);
    case DW_UART_UCV:
        return 0x3430312a;
    case DW_UART_CTR:
        return 0x44570110;
    }
    return 0;
}

static void dw_uart_ext_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    /* SRR, shadow registers etc. are accepted and ignored */
}

static const MemoryRegionOps dw_uart_ext_ops = {
    .read = dw_uart_ext_read,
    .write = dw_uart_ext_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* --- DRAM information for the loader/u-boot -------------------------- */

/*
 * PMUGRF OS_REG2/OS_REG3 normally get the DRAM geometry from the
 * Rockchip DDR init blob; u-boot (rockchip_sdram_size()) and the Linux
 * DFI/DMC drivers read it.  Encode a DDR3, single channel, 32-bit,
 * 8 bank, 10 column device of the requested size.
 */
static uint32_t rk3326_dram_os_reg2(uint64_t size)
{
    uint64_t mb = size >> 20;
    int rank = 1, row = 13, row_3_4 = 0;
    uint32_t v;

    if (mb >= 4096) {
        rank = 2;
        row = 16;
    } else {
        uint64_t p = mb;

        if (!is_power_of_2(p)) {
            /* 3/4 sizes: 768M, 1536M, 3072M */
            row_3_4 = 1;
            p = p * 4 / 3;
        }
        while (row < 16 && (1ull << (row - 5)) < p) {
            row++;
        }
    }
    v = (3u << 13) |                /* DDR3 */
        (0u << 12) |                /* 1 channel */
        (1u << 28) |                /* channel 0 info valid */
        ((uint32_t)row_3_4 << 30) |
        ((uint32_t)(rank - 1) << 11) |
        (1u << 9) |                 /* 10 columns */
        (0u << 8) |                 /* 8 banks */
        ((uint32_t)(row - 13) << 6) |
        ((uint32_t)(row - 13) << 4) |
        (0u << 2);                  /* 32-bit bus */
    return v;
}

/* --- SoC object ----------------------------------------------------- */

static void rk3326_init(Object *obj)
{
    RK3326State *s = RK3326(obj);

    for (int i = 0; i < RK3326_NUM_CPUS; i++) {
        object_initialize_child(obj, "cpu[*]", &s->cpus[i],
                                ARM_CPU_TYPE_NAME("cortex-a35"));
    }
    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GIC);

    object_initialize_child(obj, "grf", &s->grf, TYPE_ROCKCHIP_GRF);
    object_initialize_child(obj, "pmugrf", &s->pmugrf, TYPE_ROCKCHIP_GRF);
    object_initialize_child(obj, "core-grf", &s->core_grf, TYPE_ROCKCHIP_GRF);
    object_initialize_child(obj, "cpu-boost", &s->cpu_boost,
                            TYPE_ROCKCHIP_GRF);
    object_initialize_child(obj, "usb2phy-grf", &s->usb2phy_grf,
                            TYPE_ROCKCHIP_GRF);
    object_initialize_child(obj, "video-phy", &s->video_phy,
                            TYPE_ROCKCHIP_GRF);
    object_initialize_child(obj, "cru", &s->cru, TYPE_RK3326_CRU);
    object_initialize_child(obj, "pmucru", &s->pmucru, TYPE_RK3326_CRU);
    object_initialize_child(obj, "pmu", &s->pmu, TYPE_RK3326_PMU);

    for (int i = 0; i < RK3326_NUM_GPIO; i++) {
        object_initialize_child(obj, "gpio[*]", &s->gpio[i],
                                TYPE_ROCKCHIP_GPIO);
    }
    for (int i = 0; i < RK3326_NUM_I2C; i++) {
        object_initialize_child(obj, "i2c[*]", &s->i2c[i], TYPE_ROCKCHIP_I2C);
    }
    object_initialize_child(obj, "saradc", &s->saradc, TYPE_ROCKCHIP_SARADC);
    object_initialize_child(obj, "timer", &s->timer, TYPE_ROCKCHIP_TIMER);
    object_initialize_child(obj, "stimer", &s->stimer, TYPE_ROCKCHIP_TIMER);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_DW_WDT);
    for (int i = 0; i < RK3326_NUM_PWM; i++) {
        object_initialize_child(obj, "pwm[*]", &s->pwm[i], TYPE_ROCKCHIP_PWM);
    }
    object_initialize_child(obj, "tsadc", &s->tsadc, TYPE_ROCKCHIP_TSADC);
    object_initialize_child(obj, "otp", &s->otp, TYPE_ROCKCHIP_OTP);
    object_initialize_child(obj, "sdmmc", &s->sdmmc, TYPE_DW_MMC);
    object_initialize_child(obj, "sdio", &s->sdio, TYPE_DW_MMC);
    object_initialize_child(obj, "emmc", &s->emmc, TYPE_DW_MMC);
    object_initialize_child(obj, "sfc", &s->sfc, TYPE_ROCKCHIP_SFC);
    for (int i = 0; i < RK3326_NUM_IOMMU; i++) {
        object_initialize_child(obj, "iommu[*]", &s->iommu[i],
                                TYPE_ROCKCHIP_IOMMU);
    }
    object_initialize_child(obj, "dsi", &s->dsi, TYPE_DW_MIPI_DSI);
    object_initialize_child(obj, "vopb", &s->vopb, TYPE_ROCKCHIP_VOP);
    object_initialize_child(obj, "dmac-irq", &s->dmac_irq, TYPE_OR_IRQ);

    /* let the board reach some of the children */
    object_property_add_alias(obj, "sfc-drive", OBJECT(&s->sfc), "drive");
}

static void rk3326_realize_grf(RockchipGRFState *g, const char *name,
                               uint32_t size, hwaddr base)
{
    qdev_prop_set_string(DEVICE(g), "name", name);
    qdev_prop_set_uint32(DEVICE(g), "size", size);
    sysbus_realize(SYS_BUS_DEVICE(g), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(g), 0, base);
}

static void rk3326_realize(DeviceState *dev, Error **errp)
{
    RK3326State *s = RK3326(dev);
    MemoryRegion *sysmem = get_system_memory();
    SysBusDevice *sbd;
    DeviceState *d;

    /* CPUs */
    for (int i = 0; i < RK3326_NUM_CPUS; i++) {
        Object *cpu = OBJECT(&s->cpus[i]);

        object_property_set_bool(cpu, "has_el3", s->has_el3, &error_abort);
        object_property_set_bool(cpu, "has_el2", true, &error_abort);
        /* the generic timer runs from the 24 MHz oscillator */
        object_property_set_uint(cpu, "cntfrq", 24000000, &error_abort);
        if (object_property_find(cpu, "reset-cbar")) {
            object_property_set_int(cpu, "reset-cbar",
                                    rk3326_memmap[RK3326_DEV_GIC_DIST] &
                                    ~0xffffULL, &error_abort);
        }
        /* secondaries are started through PSCI (or EL3 firmware) */
        object_property_set_bool(cpu, "start-powered-off", i > 0,
                                 &error_abort);
        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return;
        }
    }

    /* GIC-400 */
    d = DEVICE(&s->gic);
    qdev_prop_set_uint32(d, "num-irq", RK3326_GIC_NUM_SPI + GIC_INTERNAL);
    qdev_prop_set_uint32(d, "revision", 2);
    qdev_prop_set_uint32(d, "num-cpu", RK3326_NUM_CPUS);
    /* GIC-400 always implements the Security Extensions */
    qdev_prop_set_bit(d, "has-security-extensions", true);
    qdev_prop_set_bit(d, "has-virtualization-extensions", true);
    sbd = SYS_BUS_DEVICE(d);
    if (!sysbus_realize(sbd, errp)) {
        return;
    }
    if (!s->has_el3) {
        /*
         * Without EL3 firmware do what TF-A BL31 does on this SoC before
         * dropping to U-Boot/Linux: make every interrupt Group 1
         * (Non-secure) at reset.
         */
        ARMLinuxBootIfClass *albifc = ARM_LINUX_BOOT_IF_GET_CLASS(d);

        albifc->arm_linux_init(ARM_LINUX_BOOT_IF(d), false);
    }
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_GIC_DIST]);
    sysbus_mmio_map(sbd, 1, rk3326_memmap[RK3326_DEV_GIC_CPU]);
    sysbus_mmio_map(sbd, 2, rk3326_memmap[RK3326_DEV_GIC_HYP]);
    sysbus_mmio_map(sbd, 3, rk3326_memmap[RK3326_DEV_GIC_VCPU]);

    for (int i = 0; i < RK3326_NUM_CPUS; i++) {
        DeviceState *cpudev = DEVICE(&s->cpus[i]);
        int ppibase = RK3326_GIC_NUM_SPI + i * GIC_INTERNAL + GIC_NR_SGIS;
        /* PPI numbers from the armv8-timer and gic nodes of px30.dtsi */
        const int timer_irq[] = {
            [GTIMER_PHYS] = 14,
            [GTIMER_VIRT] = 11,
            [GTIMER_HYP]  = 10,
            [GTIMER_SEC]  = 13,
        };

        for (int t = 0; t < ARRAY_SIZE(timer_irq); t++) {
            qdev_connect_gpio_out(cpudev, t,
                                  qdev_get_gpio_in(d, ppibase +
                                                   timer_irq[t]));
        }
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(sbd, i + RK3326_NUM_CPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(sbd, i + 2 * RK3326_NUM_CPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(sbd, i + 3 * RK3326_NUM_CPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        /* GIC maintenance interrupt: PPI 9 */
        sysbus_connect_irq(sbd, i + 4 * RK3326_NUM_CPUS,
                           qdev_get_gpio_in(d, ppibase + 9));
        /* Cortex-A35 PMU overflow interrupts are SPIs 100..103 */
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    rk3326_spi(s, RK3326_SPI_PMU_CORE0 + i));
    }

    /* On-chip SRAMs */
    memory_region_init_ram(&s->sram, OBJECT(s), "rk3326.sram", 32 * KiB,
                           &error_fatal);
    memory_region_add_subregion(sysmem, rk3326_memmap[RK3326_DEV_SRAM],
                                &s->sram);
    memory_region_init_ram(&s->pmu_sram, OBJECT(s), "rk3326.pmu-sram",
                           8 * KiB, &error_fatal);
    memory_region_add_subregion(sysmem, rk3326_memmap[RK3326_DEV_PMU_SRAM],
                                &s->pmu_sram);

    /* Unimplemented blocks */
    for (int i = 0; i < ARRAY_SIZE(rk3326_unimp); i++) {
        create_unimplemented_device(rk3326_unimp[i].name,
                                    rk3326_memmap[rk3326_unimp[i].dev],
                                    rk3326_unimp[i].size);
    }

    /* General register files */
    rk3326_realize_grf(&s->grf, "rk3326.grf", 0x8000,
                       rk3326_memmap[RK3326_DEV_GRF]);
    /* GRF_SOC_STATUS0: APLL/DPLL/CPLL/GPLL/NPLL locked */
    rockchip_grf_set_reg(&s->grf, 0x480, 0x1f, RK_GRF_F_RO);
    rockchip_grf_set_reg(&s->grf, 0x484, 0, RK_GRF_F_RO);

    rk3326_realize_grf(&s->pmugrf, "rk3326.pmugrf", 0x1000,
                       rk3326_memmap[RK3326_DEV_PMUGRF]);
    /* OS_REG0..11: plain registers, kept across warm resets */
    rockchip_grf_set_range_flags(&s->pmugrf, 0x200, 0x230,
                                 RK_GRF_F_PLAIN | RK_GRF_F_KEEP);
    rockchip_grf_set_reg(&s->pmugrf, 0x208,
                         rk3326_dram_os_reg2(s->ram_size),
                         RK_GRF_F_PLAIN | RK_GRF_F_KEEP);
    rockchip_grf_set_reg(&s->pmugrf, 0x20c, 0,
                         RK_GRF_F_PLAIN | RK_GRF_F_KEEP);

    /*
     * PVTM ring oscillators.  The count selects the CPU OPP voltage bin
     * (rockchip,pvtm-voltage-sel in px30.dtsi): 55000 per ms is a
     * typical part (bin 2).
     */
    rockchip_grf_add_pvtm(&s->pmugrf, 0x180, 0x190, 55000);

    rk3326_realize_grf(&s->core_grf, "rk3326.core-grf", 0x1000,
                       rk3326_memmap[RK3326_DEV_CORE_GRF]);
    rockchip_grf_add_pvtm(&s->core_grf, 0x80, 0x88, 55000);
    rk3326_realize_grf(&s->cpu_boost, "rk3326.cpu-boost", 0x1000,
                       rk3326_memmap[RK3326_DEV_CPU_BOOST]);
    rk3326_realize_grf(&s->usb2phy_grf, "rk3326.usb2phy-grf", 0x10000,
                       rk3326_memmap[RK3326_DEV_USB2PHY_GRF]);
    /* the inno video combo PHY uses plain 32-bit registers */
    rk3326_realize_grf(&s->video_phy, "rk3326.video-phy", 0x10000,
                       rk3326_memmap[RK3326_DEV_VIDEO_PHY]);
    rockchip_grf_set_range_flags(&s->video_phy, 0, 0x10000, RK_GRF_F_PLAIN);

    /* Clocks, power */
    sysbus_realize(SYS_BUS_DEVICE(&s->cru), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->cru), 0, rk3326_memmap[RK3326_DEV_CRU]);
    qdev_prop_set_bit(DEVICE(&s->pmucru), "pmucru", true);
    sysbus_realize(SYS_BUS_DEVICE(&s->pmucru), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pmucru), 0,
                    rk3326_memmap[RK3326_DEV_PMUCRU]);
    sysbus_realize(SYS_BUS_DEVICE(&s->pmu), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pmu), 0, rk3326_memmap[RK3326_DEV_PMU]);

    /* UARTs: 16550 compatible DesignWare APB UARTs */
    for (int i = 0; i < RK3326_NUM_UARTS; i++) {
        static const int irqs[] = {
            RK3326_SPI_UART0, RK3326_SPI_UART1, RK3326_SPI_UART2,
            RK3326_SPI_UART3, RK3326_SPI_UART4, RK3326_SPI_UART5,
        };
        int idx = -1;

        for (int j = 0; j < RK3326_NUM_UARTS; j++) {
            if (rk3326_serial_map[j] == i) {
                idx = j;
            }
        }
        memory_region_init(&s->uart_container[i], OBJECT(s), "dw-apb-uart",
                           0x100);
        s->uart[i] = serial_mm_init(&s->uart_container[i], 0, 2,
                                    rk3326_spi(s, irqs[i]), 115200,
                                    serial_hd(idx), DEVICE_LITTLE_ENDIAN);
        memory_region_init_io(&s->uart_dw[i], OBJECT(s), &dw_uart_ext_ops,
                              s->uart[i], "dw-apb-uart-ext", 0x100);
        memory_region_add_subregion_overlap(&s->uart_container[i], 0,
                                            &s->uart_dw[i], -1);
        memory_region_add_subregion(sysmem, rk3326_uart_base[i],
                                    &s->uart_container[i]);
    }

    /* GPIO banks */
    for (int i = 0; i < RK3326_NUM_GPIO; i++) {
        static const int devs[] = {
            RK3326_DEV_GPIO0, RK3326_DEV_GPIO1, RK3326_DEV_GPIO2,
            RK3326_DEV_GPIO3,
        };
        sbd = SYS_BUS_DEVICE(&s->gpio[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3326_memmap[devs[i]]);
        sysbus_connect_irq(sbd, 0, rk3326_spi(s, RK3326_SPI_GPIO0 + i));
    }

    /* I2C */
    for (int i = 0; i < RK3326_NUM_I2C; i++) {
        sbd = SYS_BUS_DEVICE(&s->i2c[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_I2C0 + i]);
        sysbus_connect_irq(sbd, 0, rk3326_spi(s, RK3326_SPI_I2C0 + i));
    }

    /* SARADC */
    sbd = SYS_BUS_DEVICE(&s->saradc);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_SARADC]);
    sysbus_connect_irq(sbd, 0, rk3326_spi(s, RK3326_SPI_SARADC));

    /* Timers: 6 non-secure timers, 2 secure timers */
    sbd = SYS_BUS_DEVICE(&s->timer);
    qdev_prop_set_uint32(DEVICE(sbd), "num-channels", 6);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_TIMER]);
    for (int i = 0; i < 6; i++) {
        sysbus_connect_irq(sbd, i, rk3326_spi(s, RK3326_SPI_TIMER0 + i));
    }
    sbd = SYS_BUS_DEVICE(&s->stimer);
    qdev_prop_set_uint32(DEVICE(sbd), "num-channels", 2);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_STIMER]);

    /* Watchdog */
    sbd = SYS_BUS_DEVICE(&s->wdt);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_WDT]);
    sysbus_connect_irq(sbd, 0, rk3326_spi(s, RK3326_SPI_WDT));

    /* PWM */
    for (int i = 0; i < RK3326_NUM_PWM; i++) {
        sbd = SYS_BUS_DEVICE(&s->pwm[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_PWM0 + i]);
    }

    /* TSADC, OTP */
    sbd = SYS_BUS_DEVICE(&s->tsadc);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_TSADC]);
    sysbus_connect_irq(sbd, 0, rk3326_spi(s, RK3326_SPI_TSADC));

    if (s->otp_serial) {
        qdev_prop_set_string(DEVICE(&s->otp), "serial", s->otp_serial);
    }
    sbd = SYS_BUS_DEVICE(&s->otp);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_OTP]);

    /* PL330 DMA controller */
    {
        DeviceState *dma = qdev_new("pl330");
        const int nevents = 16;

        object_property_set_link(OBJECT(dma), "memory", OBJECT(sysmem),
                                 &error_fatal);
        qdev_prop_set_uint8(dma, "num_events", nevents);
        qdev_prop_set_uint8(dma, "num_chnls", 8);
        qdev_prop_set_uint8(dma, "num_periph_req", 32);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
        /*
         * The 4 KiB PL330 register block is mirrored over the 16 KiB
         * window of px30.dtsi; the AMBA bus code reads the PrimeCell ID
         * at the end of the window.
         */
        for (int i = 0; i < 4; i++) {
            MemoryRegion *alias = g_new(MemoryRegion, 1);

            memory_region_init_alias(alias, OBJECT(s), "pl330-mirror",
                                     sysbus_mmio_get_region(
                                         SYS_BUS_DEVICE(dma), 0),
                                     0, 0x1000);
            memory_region_add_subregion(sysmem,
                                        rk3326_memmap[RK3326_DEV_DMAC] +
                                        i * 0x1000, alias);
        }
        object_property_set_int(OBJECT(&s->dmac_irq), "num-lines",
                                nevents + 1, &error_abort);
        qdev_realize(DEVICE(&s->dmac_irq), NULL, &error_fatal);
        for (int i = 0; i < nevents + 1; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dma), i,
                               qdev_get_gpio_in(DEVICE(&s->dmac_irq), i));
        }
        qdev_connect_gpio_out(DEVICE(&s->dmac_irq), 0,
                              rk3326_spi(s, RK3326_SPI_DMAC0));
    }

    /* SD/MMC controllers */
    {
        DWMMCState *mmc[] = { &s->sdmmc, &s->sdio, &s->emmc };
        const int devs[] = { RK3326_DEV_SDMMC, RK3326_DEV_SDIO,
                             RK3326_DEV_EMMC };
        const int irqs[] = { RK3326_SPI_SDMMC, RK3326_SPI_SDIO,
                             RK3326_SPI_EMMC };

        for (int i = 0; i < ARRAY_SIZE(mmc); i++) {
            sbd = SYS_BUS_DEVICE(mmc[i]);
            object_property_set_link(OBJECT(sbd), "dma-memory",
                                     OBJECT(sysmem), &error_abort);
            sysbus_realize(sbd, &error_fatal);
            sysbus_mmio_map(sbd, 0, rk3326_memmap[devs[i]]);
            sysbus_connect_irq(sbd, 0, rk3326_spi(s, irqs[i]));
        }
    }

    /* SFC + SPI NOR */
    sbd = SYS_BUS_DEVICE(&s->sfc);
    object_property_set_link(OBJECT(sbd), "dma-memory", OBJECT(sysmem),
                             &error_abort);
    if (!sysbus_realize(sbd, errp)) {
        return;
    }
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_SFC]);
    sysbus_connect_irq(sbd, 0, rk3326_spi(s, RK3326_SPI_SFC));

    /* USB: DWC2 OTG, EHCI/OHCI host */
    {
        DeviceState *otg = qdev_new("dwc2-usb");

        object_property_add_const_link(OBJECT(otg), "dma-mr", OBJECT(sysmem));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(otg), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(otg), 0,
                        rk3326_memmap[RK3326_DEV_USB_OTG]);
        sysbus_connect_irq(SYS_BUS_DEVICE(otg), 0,
                           rk3326_spi(s, RK3326_SPI_OTG));
    }
    sysbus_create_simple(TYPE_PLATFORM_EHCI,
                         rk3326_memmap[RK3326_DEV_USB_EHCI],
                         rk3326_spi(s, RK3326_SPI_EHCI));
    sysbus_create_simple("sysbus-ohci", rk3326_memmap[RK3326_DEV_USB_OHCI],
                         rk3326_spi(s, RK3326_SPI_OHCI));

    /* IOMMUs */
    {
        OrIRQState *hevc_or = OR_IRQ(object_new(TYPE_OR_IRQ));

        object_property_add_child(OBJECT(s), "hevc-mmu-irq", OBJECT(hevc_or));
        object_property_set_int(OBJECT(hevc_or), "num-lines", 2,
                                &error_abort);
        qdev_realize_and_unref(DEVICE(hevc_or), NULL, &error_fatal);
        qdev_connect_gpio_out(DEVICE(hevc_or), 0,
                              rk3326_spi(s, RK3326_SPI_HEVC_MMU));

        for (int i = 0; i < RK3326_NUM_IOMMU; i++) {
            sbd = SYS_BUS_DEVICE(&s->iommu[i]);
            object_property_set_link(OBJECT(sbd), "dma-memory",
                                     OBJECT(sysmem), &error_abort);
            qdev_prop_set_uint32(DEVICE(sbd), "size", rk3326_iommus[i].size);
            sysbus_realize(sbd, &error_fatal);
            memory_region_add_subregion_overlap(sysmem,
                                                rk3326_iommus[i].base,
                                                sysbus_mmio_get_region(sbd, 0),
                                                1);
            if (rk3326_iommus[i].irq == RK3326_SPI_HEVC_MMU) {
                sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(hevc_or),
                                                           i == 2 ? 0 : 1));
            } else if (i > 0) {
                sysbus_connect_irq(sbd, 0,
                                   rk3326_spi(s, rk3326_iommus[i].irq));
            }
        }
    }

    /* Display: VOPB -> DSI -> panel */
    sbd = SYS_BUS_DEVICE(&s->dsi);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_DSI]);
    sysbus_connect_irq(sbd, 0, rk3326_spi(s, RK3326_SPI_DSI));

    {
        OrIRQState *vop_or = OR_IRQ(object_new(TYPE_OR_IRQ));

        object_property_add_child(OBJECT(s), "vop-irq", OBJECT(vop_or));
        object_property_set_int(OBJECT(vop_or), "num-lines", 2, &error_abort);
        qdev_realize_and_unref(DEVICE(vop_or), NULL, &error_fatal);
        qdev_connect_gpio_out(DEVICE(vop_or), 0,
                              rk3326_spi(s, RK3326_SPI_VOPB));

        sbd = SYS_BUS_DEVICE(&s->vopb);
        object_property_set_link(OBJECT(sbd), "dma-memory", OBJECT(sysmem),
                                 &error_abort);
        object_property_set_link(OBJECT(sbd), "iommu", OBJECT(&s->iommu[0]),
                                 &error_abort);
        object_property_set_link(OBJECT(sbd), "dsi", OBJECT(&s->dsi),
                                 &error_abort);
        qdev_prop_set_uint32(DEVICE(sbd), "rotation", s->display_rotation);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, rk3326_memmap[RK3326_DEV_VOPB]);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(vop_or), 0));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->iommu[0]), 0,
                           qdev_get_gpio_in(DEVICE(vop_or), 1));
    }
}

static const Property rk3326_props[] = {
    DEFINE_PROP_UINT64("ram-size", RK3326State, ram_size, 1 * GiB),
    DEFINE_PROP_STRING("otp-serial", RK3326State, otp_serial),
    DEFINE_PROP_UINT32("display-rotation", RK3326State, display_rotation, 0),
    DEFINE_PROP_BOOL("has-el3", RK3326State, has_el3, false),
};

static void rk3326_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3326_realize;
    device_class_set_props(dc, rk3326_props);
    /* Reason: uses serial_hd() and get_system_memory() */
    dc->user_creatable = false;
}

static const TypeInfo rk3326_type_info = {
    .name = TYPE_RK3326,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3326State),
    .instance_init = rk3326_init,
    .class_init = rk3326_class_init,
};

static void rk3326_register_types(void)
{
    type_register_static(&rk3326_type_info);
}

type_init(rk3326_register_types)
