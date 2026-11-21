Hardkernel ODROID-GO Advance Black Edition (``odroid-go-advance-be``)
=====================================================================

The ``odroid-go-advance-be`` machine emulates the Hardkernel ODROID-GO
Advance Black Edition, board revision 1.1, a handheld game console based
on the Rockchip RK3326 SoC.  The machine is intended for operating system
and boot loader porting work, so the SoC peripherals and the parts on the
board are modelled at register level following the Hardkernel sources:

* u-boot ``odroidgoA-v2017.09`` (``configs/odroidgoa_defconfig``,
  ``board/rockchip/odroidgoa``)
* linux ``odroidgoA-4.4.y``
  (``arch/arm64/boot/dts/rockchip/rk3326-odroidgo2-linux-v11.dts``)

Emulated hardware
-----------------

SoC (RK3326):

* 4x Cortex-A35 (EL2, optional EL3), 24 MHz generic timer
* GIC-400 (GICv2 with Security and Virtualization Extensions)
* CRU/PMUCRU (PLL lock reporting, global soft reset), PMU power domains,
  GRF/PMUGRF/CORE_GRF/USB2PHY_GRF (HIWORD write masks, OS_REGx kept
  across warm resets), 32 KiB SRAM, 8 KiB PMU SRAM
* 6 DesignWare APB UARTs (16550 + DW extension registers)
* 4 GPIO banks, 4 RK3x I2C controllers, SARADC, TSADC, OTP
* 6+2 Rockchip timers, DesignWare watchdog, 8 PWM channels
* PL330 DMA controller
* 3 DesignWare MSHC SD/MMC controllers (PIO and IDMAC)
* SFC serial flash controller
* DWC2 OTG (host mode), EHCI and OHCI host controllers
* VOP (VOP big) display controller with IOMMU, DesignWare MIPI DSI host,
  PX30 video PHY
* IOMMUs of the VOP, VPU, HEVC, VIP and ISP

Board:

* 1 GiB DDR3L (``-m`` up to 4 GiB)
* RK817 PMIC on I2C0 @ 0x20: regulators, RTC, power key, fuel gauge and
  charger, interrupt on GPIO0_B2, SLEEP pin on GPIO0_A4
* micro SD slot on SDMMC, card detect on GPIO0_A3
* 16 MiB XTX XT25F128B SPI NOR flash on the SFC
* 3.5" 320x480 Elida KD35T133 MIPI DSI panel (reset on GPIO3_C0), PWM1
  backlight
* 18 GPIO buttons and the analog stick on SARADC channels 1 and 2
* hardware revision divider on SARADC channel 0 (rev 1.1)
* DC jack detect (GPIO0_B3), headphone detect (GPIO2_C6)

Not emulated (register accesses are ignored and logged with
``-d unimp``): Mali-G31 GPU, video codecs, RGA, camera interface/ISP,
I2S/PDM audio and the RK817 codec, crypto, SPI controllers, the ESP8089
WiFi on SDIO and the DDR controller.

Boot options
------------

The Rockchip BootROM, the DDR init blob/miniloader and TF-A are not
emulated.  Instead the board leaves the hardware in the state the
loaders would (PLLs locked, DRAM geometry in PMUGRF OS_REG2, all
interrupts Non-secure) and QEMU provides PSCI through SMC.  Three ways to
start the guest are supported:

Direct kernel boot
  ``-kernel Image -dtb <board dtb> [-initrd ...] [-append ...]``.
  As with Rockchip U-Boot, the kernel gets the DRAM from 0x00200000 up;
  the first 2 MiB belong to the trusted firmware (the Hardkernel DTB
  places ramoops at 0x110000).

U-Boot proper
  ``-bios u-boot.bin``: the image is loaded at 0x00200000 and started at
  EL2.

SD card image only
  Without ``-kernel``/``-bios`` the board behaves like the Rockchip
  miniloader: it reads the ``LOADER`` image (``uboot.img``) from sector
  0x4000 of the SD card, as written by Hardkernel's ``sd_fusing.sh``,
  loads it to its load address and starts it at EL2.

Examples
--------

Boot a kernel built from the Hardkernel tree::

  $ qemu-system-aarch64 -M odroid-go-advance-be \
      -kernel Image -dtb rk3326-odroidgo2-linux-v11.dtb \
      -append "console=ttyFIQ0 root=/dev/mmcblk0p2 rootwait" \
      -drive file=sd.img,if=sd,format=raw -serial stdio

Boot a Hardkernel SD card image (U-Boot, ``boot.ini`` and kernel from the
image)::

  $ qemu-system-aarch64 -M odroid-go-advance-be \
      -drive file=ArkOS.img,if=sd,format=raw -serial stdio

Boot U-Boot and keep the SPI flash contents in a file::

  $ qemu-system-aarch64 -M odroid-go-advance-be -bios u-boot.bin \
      -drive file=spi.img,if=mtd,format=raw \
      -drive file=sd.img,if=sd,format=raw -serial stdio

``serial0`` is UART2, the debug UART used by U-Boot and the kernel.

Machine options
---------------

``display-rotation=0|90|180|270``
  Rotation of the panel picture.  The panel is 320x480 portrait and is
  mounted rotated in the landscape handheld; the default (90) shows the
  picture as seen on the device (Hardkernel software uses
  ``fbcon=rotate:3``).  Use 0 to see the raw panel orientation.

``gamepad-keys=on|off``
  Map host keys to the buttons (default on).

``dc-in=on|off``
  DC adapter connected (DC_DET and the RK817 PLUG_IN status).  With DC
  plugged in, Hardkernel U-Boot shows its charging screen.

``battery-mv=<mV>``
  Battery voltage reported by the fuel gauge (default 3900).  Hardkernel
  U-Boot refuses to boot below 3530 mV without DC.

``hwrev-adc=<code>``
  SARADC channel 0 value that U-Boot's ``hwrev`` uses to select the board
  revision and DTB (675: rev 1.1, 856: rev 1.0, 83: ODROID-GO Super).

Key mapping
-----------

================  ===========================
Host key          ODROID-GO Advance
================  ===========================
cursor keys       D-pad
X / Z / S / A     A / B / X / Y
Q / W             L1 / R1 (top left/right)
E / R             L2 / R2 (rev 1.1 only)
1 ... 6           F1 ... F6
I / K / J / L     analog stick up/down/left/right
P                 power key
================  ===========================

The QEMU ``system_powerdown`` command produces a short press of the power
key.  Sensor and power values can also be changed at run time, e.g.::

  (qemu) qom-set /machine/soc/saradc channel1 200
  (qemu) qom-set /machine/soc/tsadc temperature0 85000
  (qemu) qom-set /machine dc-in on

Tested software
---------------

* Hardkernel u-boot ``odroidgoA-v2017.09`` (``odroidgoa_defconfig``):
  hw revision detection, PMIC, SPI NOR, SD/FAT, DSI panel bring-up,
  ``boot.ini`` and ``booti``, both with ``-bios`` and from an SD image.
* Hardkernel linux ``odroidgoA-4.4.y`` (``odroidgoa_defconfig``) with
  ``rk3326-odroidgo2-linux-v11.dtb``: ttyFIQ0 console, RK817 BSP
  regulator/RTC/power key/fuel gauge/charger drivers, odroidgo2 joypad,
  rockchip DRM (VOP + IOMMU + DSI + panel), dw_mmc, SARADC, TSADC,
  PVTM, OTP.
* Mainline Linux 6.12 with ``rk3326-odroid-go2-v11.dtb``: the same set
  of devices with the upstream drivers, plus SPI NOR (SFDP).

Limitations
-----------

* No BootROM: ``idbloader.img`` (DDR blob, miniloader or SPL/TPL) cannot
  be executed; use one of the boot options above.
* TF-A is not run, Rockchip SIP SMC calls (DDR DVFS, FIQ debugger
  routing, suspend) return "not supported".
* Clocks are not modelled: clock dividers are plain registers.
* USB OTG works in host mode only.
