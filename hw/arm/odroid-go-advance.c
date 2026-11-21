/*
 * Hardkernel ODROID-GO Advance Black Edition (board rev 1.1) emulation
 *
 * The board definition follows rk3326-odroidgo2-linux-v11.dts of the
 * Hardkernel odroidgoA-4.4.y kernel and the odroidgoa board support of
 * the Hardkernel odroidgoa-v2017.09 u-boot:
 *
 *   SoC        Rockchip RK3326, 1 GiB DDR3L
 *   PMIC       RK817 on I2C0 @ 0x20, INT -> GPIO0_B2, SLEEP <- GPIO0_A4
 *   Storage    micro SD on SDMMC (CD: GPIO0_A3, active low),
 *              16 MiB XTX XT25F128B SPI NOR on the SFC
 *   Display    3.5" 320x480 Elida KD35T133 MIPI DSI panel (1 lane),
 *              reset GPIO3_C0, PWM1 backlight
 *   Input      18 GPIO buttons, analog stick on SARADC ch1/ch2,
 *              power key on the PMIC
 *   HW rev     SARADC ch0 (read by u-boot "hwrev": 655..695 = rev 1.1)
 *   Console    UART2 (mux m1), 115200 8N1 (serial0)
 *   Other      DC jack detect GPIO0_B3, headphone detect GPIO2_C6,
 *              blue LED GPIO0_C1, ESP8089 WiFi on SDIO (not emulated)
 *
 * Boot:
 *   -kernel Image -dtb rk3326-odroidgo2-linux-v11.dtb  direct Linux boot,
 *                       PSCI is provided by QEMU (no EL3)
 *   -bios u-boot.bin    U-Boot proper at 0x00200000, entered at EL2
 *   SD image only       "virtual miniloader": the board reads the
 *                       Rockchip LOADER image (uboot.img) from sector
 *                       0x4000 of the SD card like the Rockchip
 *                       miniloader does and starts it at EL2
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qapi/visitor.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "hw/arm/boot.h"
#include "hw/arm/rk3326.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/rk817.h"
#include "hw/sd/sd.h"
#include "qapi/qapi-types-ui.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/qtest.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/input.h"

#define TYPE_ODROID_GO_ADVANCE_MACHINE MACHINE_TYPE_NAME("odroid-go-advance-be")
OBJECT_DECLARE_SIMPLE_TYPE(OdroidGoAdvanceState, ODROID_GO_ADVANCE_MACHINE)

#define OGA_RAM_DEFAULT         (1 * GiB)
#define OGA_UBOOT_TEXT_BASE     0x00200000
#define OGA_FW_RESERVED         (2 * MiB)
#define OGA_LOADER_SECTOR       0x4000      /* uboot.img */
#define OGA_HWREV_ADC_V11       675         /* 655 < x < 695 */
#define OGA_JOY_CENTER          512
#define OGA_JOY_RANGE           360

/* GPIO pins: bank, pin */
#define GPIO_PIN(b, letter, n)  ((b) * 32 + ((letter) - 'A') * 8 + (n))

#define OGA_GPIO_SD_CD          GPIO_PIN(0, 'A', 3)
#define OGA_GPIO_PMIC_SLEEP     GPIO_PIN(0, 'A', 4)
#define OGA_GPIO_PMIC_INT       GPIO_PIN(0, 'B', 2)
#define OGA_GPIO_DC_DET         GPIO_PIN(0, 'B', 3)
#define OGA_GPIO_HP_DET         GPIO_PIN(2, 'C', 6)
#define OGA_GPIO_LCD_RESET      GPIO_PIN(3, 'C', 0)

typedef struct OgaButton {
    const char *name;
    int pin;
    QKeyCode key;
} OgaButton;

/* Button switch -> GPIO assignment of rk3326-odroidgo2-linux-v11.dts */
static const OgaButton oga_buttons[] = {
    { "dpad-up",     GPIO_PIN(1, 'B', 4), Q_KEY_CODE_UP },
    { "dpad-down",   GPIO_PIN(1, 'B', 5), Q_KEY_CODE_DOWN },
    { "dpad-left",   GPIO_PIN(1, 'B', 6), Q_KEY_CODE_LEFT },
    { "dpad-right",  GPIO_PIN(1, 'B', 7), Q_KEY_CODE_RIGHT },
    { "a",           GPIO_PIN(1, 'A', 2), Q_KEY_CODE_X },
    { "b",           GPIO_PIN(1, 'A', 5), Q_KEY_CODE_Z },
    { "y",           GPIO_PIN(1, 'A', 6), Q_KEY_CODE_A },
    { "x",           GPIO_PIN(1, 'A', 7), Q_KEY_CODE_S },
    { "f1",          GPIO_PIN(2, 'A', 0), Q_KEY_CODE_1 },
    { "f2",          GPIO_PIN(2, 'A', 1), Q_KEY_CODE_2 },
    { "f3",          GPIO_PIN(2, 'A', 2), Q_KEY_CODE_3 },
    { "f4",          GPIO_PIN(2, 'A', 3), Q_KEY_CODE_4 },
    { "f5",          GPIO_PIN(2, 'A', 4), Q_KEY_CODE_5 },
    { "f6",          GPIO_PIN(2, 'A', 5), Q_KEY_CODE_6 },
    { "top-left",    GPIO_PIN(2, 'A', 6), Q_KEY_CODE_Q },
    { "top-right",   GPIO_PIN(2, 'A', 7), Q_KEY_CODE_W },
    { "top-left2",   GPIO_PIN(3, 'B', 7), Q_KEY_CODE_E },
    { "top-right2",  GPIO_PIN(3, 'B', 2), Q_KEY_CODE_R },
};

struct OdroidGoAdvanceState {
    MachineState parent;

    RK3326State soc;
    RK817State *pmic;
    struct arm_boot_info binfo;
    QemuInputHandlerState *input;
    Notifier powerdown;
    QEMUTimer *pwrkey_timer;
    bool joy[4];            /* up, down, left, right */

    /* properties */
    bool dc_in;
    bool gamepad_keys;
    uint32_t rotation;
    uint32_t battery_mv;
    uint16_t hwrev_adc;
};

static qemu_irq oga_gpio_in(OdroidGoAdvanceState *s, int pin)
{
    return qdev_get_gpio_in(DEVICE(&s->soc.gpio[pin / 32]), pin % 32);
}

static void oga_gpio_set(OdroidGoAdvanceState *s, int pin, int level)
{
    qemu_set_irq(oga_gpio_in(s, pin), level);
}

static void oga_update_stick(OdroidGoAdvanceState *s)
{
    int x = OGA_JOY_CENTER, y = OGA_JOY_CENTER;

    if (s->joy[2]) {
        x -= OGA_JOY_RANGE;
    }
    if (s->joy[3]) {
        x += OGA_JOY_RANGE;
    }
    if (s->joy[0]) {
        y -= OGA_JOY_RANGE;
    }
    if (s->joy[1]) {
        y += OGA_JOY_RANGE;
    }
    rockchip_saradc_set_input(&s->soc.saradc, 1, x);
    rockchip_saradc_set_input(&s->soc.saradc, 2, y);
}

static void oga_input_event(DeviceState *dev, QemuConsole *src,
                            QemuInputEvent *evt)
{
    OdroidGoAdvanceState *s = ODROID_GO_ADVANCE_MACHINE(qdev_get_machine());
    QemuInputKeyEvent *key = &evt->key;
    int qcode = qemu_input_linux_to_qcode(key->key);
    static const QKeyCode stick[4] = {
        Q_KEY_CODE_I, Q_KEY_CODE_K, Q_KEY_CODE_J, Q_KEY_CODE_L,
    };

    for (int i = 0; i < ARRAY_SIZE(oga_buttons); i++) {
        if (oga_buttons[i].key == qcode) {
            /* buttons pull the line low when pressed */
            oga_gpio_set(s, oga_buttons[i].pin, !key->down);
            return;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (stick[i] == qcode) {
            s->joy[i] = key->down;
            oga_update_stick(s);
            return;
        }
    }
    if (qcode == Q_KEY_CODE_P) {
        rk817_set_pwrkey(s->pmic, key->down);
    }
}

static const QemuInputHandler oga_input_handler = {
    .name  = "ODROID-GO Advance buttons",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = oga_input_event,
};

static void oga_pwrkey_release(void *opaque)
{
    OdroidGoAdvanceState *s = opaque;

    rk817_set_pwrkey(s->pmic, false);
}

static void oga_powerdown_req(Notifier *n, void *opaque)
{
    OdroidGoAdvanceState *s = container_of(n, OdroidGoAdvanceState,
                                           powerdown);

    /* short press of the power key */
    rk817_set_pwrkey(s->pmic, true);
    timer_mod(s->pwrkey_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 300);
}

/*
 * Virtual Rockchip miniloader: load the "LOADER" image (uboot.img,
 * second_loader_hdr + payload) from the SD card.
 */
static bool oga_load_sd_loader(OdroidGoAdvanceState *s, BlockBackend *blk,
                               hwaddr *entry)
{
    uint8_t hdr[2048];
    uint32_t load_addr, load_size;
    g_autofree uint8_t *payload = NULL;

    if (blk_pread(blk, OGA_LOADER_SECTOR * 512ULL, sizeof(hdr), hdr, 0) < 0) {
        return false;
    }
    if (memcmp(hdr, "LOADER", 6) != 0) {
        return false;
    }
    load_addr = ldl_le_p(&hdr[16]);
    load_size = ldl_le_p(&hdr[20]);
    if (!load_size || load_size > 64 * MiB ||
        load_addr + (uint64_t)load_size > s->parent.ram_size) {
        error_report("odroid-go-advance: bad LOADER image on SD "
                     "(addr 0x%x size 0x%x)", load_addr, load_size);
        return false;
    }
    payload = g_malloc(load_size);
    if (blk_pread(blk, OGA_LOADER_SECTOR * 512ULL + sizeof(hdr), load_size,
                  payload, 0) < 0) {
        return false;
    }
    rom_add_blob_fixed("sd:uboot.img", payload, load_size, load_addr);
    info_report("odroid-go-advance: loaded U-Boot from SD (0x%x bytes "
                "at 0x%x)", load_size, load_addr);
    *entry = load_addr;
    return true;
}

static void oga_firmware_boot(OdroidGoAdvanceState *s, hwaddr entry)
{
    MachineState *ms = MACHINE(s);

    /* CPUs come out of reset at the loader entry point (EL2) */
    for (int i = 0; i < RK3326_NUM_CPUS; i++) {
        object_property_set_uint(OBJECT(&s->soc.cpus[i]), "rvbar", entry,
                                 &error_abort);
    }
    /*
     * There is no EL3 firmware: arm_load_kernel() in firmware mode
     * registers the CPU reset handlers and makes QEMU answer PSCI SMCs
     * from the EL2 loader (U-Boot) and the kernel it starts.
     */
    s->binfo.loader_start = RK3326_SDRAM_BASE;
    s->binfo.ram_size = MIN(ms->ram_size, RK3326_SDRAM_MAX);
    s->binfo.board_id = -1;
    s->binfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    s->binfo.firmware_loaded = true;
    arm_load_kernel(&s->soc.cpus[0], ms, &s->binfo);
}

static void oga_init(MachineState *machine)
{
    OdroidGoAdvanceState *s = ODROID_GO_ADVANCE_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DriveInfo *dinfo;
    BlockBackend *sd_blk = NULL;
    I2CBus *i2c0;
    uint64_t ram_map;

    if (machine->ram_size > 4 * GiB) {
        error_report("odroid-go-advance: at most 4 GiB of RAM is supported");
        exit(1);
    }

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_RK3326);
    qdev_prop_set_uint64(DEVICE(&s->soc), "ram-size", machine->ram_size);
    qdev_prop_set_uint32(DEVICE(&s->soc), "display-rotation", s->rotation);
    dinfo = drive_get(IF_MTD, 0, 0);
    if (dinfo) {
        qdev_prop_set_drive_err(DEVICE(&s->soc), "sfc-drive",
                                blk_by_legacy_dinfo(dinfo), &error_fatal);
    }
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    /* DRAM at 0; the top 16 MiB of the 32-bit space are peripherals */
    ram_map = MIN(machine->ram_size, RK3326_SDRAM_MAX);
    if (ram_map < machine->ram_size) {
        MemoryRegion *alias = g_new(MemoryRegion, 1);

        memory_region_init_alias(alias, NULL, "ram-low", machine->ram, 0,
                                 ram_map);
        memory_region_add_subregion(sysmem, RK3326_SDRAM_BASE, alias);
    } else {
        memory_region_add_subregion(sysmem, RK3326_SDRAM_BASE, machine->ram);
    }

    /* micro SD card slot on SDMMC */
    dinfo = drive_get(IF_SD, 0, 0);
    if (dinfo) {
        DeviceState *card = qdev_new(TYPE_SD_CARD);

        sd_blk = blk_by_legacy_dinfo(dinfo);
        qdev_prop_set_drive_err(card, "drive", sd_blk, &error_fatal);
        qdev_realize_and_unref(card,
                               qdev_get_child_bus(DEVICE(&s->soc.sdmmc),
                                                  "sd-bus"),
                               &error_fatal);
    }
    /* card detect switch closes to GND when a card is present */
    oga_gpio_set(s, OGA_GPIO_SD_CD, dinfo ? 0 : 1);

    /* RK817 PMIC */
    i2c0 = I2C_BUS(qdev_get_child_bus(DEVICE(&s->soc.i2c[0]), "i2c"));
    s->pmic = RK817(qdev_new(TYPE_RK817));
    qdev_prop_set_uint32(DEVICE(s->pmic), "battery-mv", s->battery_mv);
    qdev_prop_set_bit(DEVICE(s->pmic), "dc-in", s->dc_in);
    qdev_prop_set_uint8(DEVICE(s->pmic), "address", 0x20);
    i2c_slave_realize_and_unref(I2C_SLAVE(s->pmic), i2c0, &error_fatal);
    qdev_connect_gpio_out_named(DEVICE(s->pmic), "irq", 0,
                                oga_gpio_in(s, OGA_GPIO_PMIC_INT));
    qdev_connect_gpio_out(DEVICE(&s->soc.gpio[OGA_GPIO_PMIC_SLEEP / 32]),
                          OGA_GPIO_PMIC_SLEEP % 32,
                          qdev_get_gpio_in_named(DEVICE(s->pmic), "sleep", 0));
    oga_gpio_set(s, OGA_GPIO_PMIC_INT, 1);

    /* DC jack detect (high = adapter present), headphone detect */
    oga_gpio_set(s, OGA_GPIO_DC_DET, s->dc_in);
    oga_gpio_set(s, OGA_GPIO_HP_DET, 1);

    /* LCD reset */
    qdev_connect_gpio_out(DEVICE(&s->soc.gpio[OGA_GPIO_LCD_RESET / 32]),
                          OGA_GPIO_LCD_RESET % 32,
                          qdev_get_gpio_in_named(DEVICE(&s->soc.dsi),
                                                 "panel-reset", 0));

    /* buttons: pulled up, released */
    for (int i = 0; i < ARRAY_SIZE(oga_buttons); i++) {
        oga_gpio_set(s, oga_buttons[i].pin, 1);
    }

    /* SARADC: board revision divider and the analog stick at rest */
    rockchip_saradc_set_input(&s->soc.saradc, 0, s->hwrev_adc);
    oga_update_stick(s);

    if (s->gamepad_keys) {
        s->input = qemu_input_handler_register(DEVICE(&s->soc),
                                               &oga_input_handler);
        qemu_input_handler_activate(s->input);
    }
    s->pwrkey_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, oga_pwrkey_release, s);
    s->powerdown.notify = oga_powerdown_req;
    qemu_register_powerdown_notifier(&s->powerdown);

    if (qtest_enabled()) {
        return;
    }

    /* Boot */
    if (machine->kernel_filename) {
        /*
         * Present the same memory to the kernel as Rockchip U-Boot does:
         * the first 2 MiB of DRAM belong to the trusted firmware (and are
         * used for ramoops by the Hardkernel DTB), so the kernel bank
         * starts at 0x00200000 and the Image is placed above it.
         */
        s->binfo.loader_start = RK3326_SDRAM_BASE + OGA_FW_RESERVED;
        s->binfo.ram_size = ram_map - OGA_FW_RESERVED;
        s->binfo.board_id = -1;
        s->binfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
        arm_load_kernel(&s->soc.cpus[0], machine, &s->binfo);
    } else if (machine->firmware) {
        g_autofree char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                             machine->firmware);
        int64_t size;

        if (!fn) {
            error_report("odroid-go-advance: cannot find '%s'",
                         machine->firmware);
            exit(1);
        }
        size = load_image_targphys(fn, OGA_UBOOT_TEXT_BASE,
                                   ram_map - OGA_UBOOT_TEXT_BASE, NULL);
        if (size < 0) {
            error_report("odroid-go-advance: failed to load '%s'", fn);
            exit(1);
        }
        oga_firmware_boot(s, OGA_UBOOT_TEXT_BASE);
    } else {
        hwaddr entry;

        if (!sd_blk || !oga_load_sd_loader(s, sd_blk, &entry)) {
            error_report("odroid-go-advance: nothing to boot: use -kernel, "
                         "-bios or an SD card image with uboot.img at "
                         "sector 0x%x", OGA_LOADER_SECTOR);
            exit(1);
        }
        oga_firmware_boot(s, entry);
    }
}

static void oga_set_dc_in(Object *obj, bool value, Error **errp)
{
    OdroidGoAdvanceState *s = ODROID_GO_ADVANCE_MACHINE(obj);

    s->dc_in = value;
    if (s->pmic) {
        rk817_set_dc_in(s->pmic, value);
        oga_gpio_set(s, OGA_GPIO_DC_DET, value);
    }
}

static bool oga_get_dc_in(Object *obj, Error **errp)
{
    return ODROID_GO_ADVANCE_MACHINE(obj)->dc_in;
}

#define OGA_PROP_ACCESSORS(name, field, vtype, ctype)                   \
static void oga_get_##name(Object *obj, Visitor *v, const char *pname,  \
                           void *opaque, Error **errp)                  \
{                                                                       \
    ctype val = ODROID_GO_ADVANCE_MACHINE(obj)->field;                  \
    visit_type_##vtype(v, pname, &val, errp);                           \
}                                                                       \
static void oga_set_##name(Object *obj, Visitor *v, const char *pname,  \
                           void *opaque, Error **errp)                  \
{                                                                       \
    ctype val;                                                          \
    if (visit_type_##vtype(v, pname, &val, errp)) {                     \
        ODROID_GO_ADVANCE_MACHINE(obj)->field = val;                    \
    }                                                                   \
}

OGA_PROP_ACCESSORS(rotation, rotation, uint32, uint32_t)
OGA_PROP_ACCESSORS(battery, battery_mv, uint32, uint32_t)
OGA_PROP_ACCESSORS(hwrev, hwrev_adc, uint16, uint16_t)

static bool oga_get_gamepad(Object *obj, Error **errp)
{
    return ODROID_GO_ADVANCE_MACHINE(obj)->gamepad_keys;
}

static void oga_set_gamepad(Object *obj, bool value, Error **errp)
{
    ODROID_GO_ADVANCE_MACHINE(obj)->gamepad_keys = value;
}

static void oga_instance_init(Object *obj)
{
    OdroidGoAdvanceState *s = ODROID_GO_ADVANCE_MACHINE(obj);

    s->gamepad_keys = true;
    s->rotation = 90;
    s->battery_mv = 3900;
    s->hwrev_adc = OGA_HWREV_ADC_V11;

}

static const char * const oga_valid_cpu_types[] = {
    ARM_CPU_TYPE_NAME("cortex-a35"),
    NULL
};

static void oga_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Hardkernel ODROID-GO Advance Black Edition (RK3326, rev 1.1)";
    mc->init = oga_init;
    mc->min_cpus = RK3326_NUM_CPUS;
    mc->max_cpus = RK3326_NUM_CPUS;
    mc->default_cpus = RK3326_NUM_CPUS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a35");
    mc->valid_cpu_types = oga_valid_cpu_types;
    mc->default_ram_size = OGA_RAM_DEFAULT;
    mc->default_ram_id = "odroid-go-advance.ram";
    mc->block_default_type = IF_SD;
    mc->units_per_default_bus = 1;
    mc->ignore_memory_transaction_failures = false;

    object_class_property_add_bool(oc, "dc-in", oga_get_dc_in, oga_set_dc_in);
    object_class_property_set_description(oc, "dc-in",
        "DC adapter / charger connected (DC_DET + RK817 PLUG_IN)");
    object_class_property_add_bool(oc, "gamepad-keys", oga_get_gamepad,
                                   oga_set_gamepad);
    object_class_property_set_description(oc, "gamepad-keys",
        "Map host keyboard keys to the gamepad buttons (default on)");
    object_class_property_add(oc, "display-rotation", "uint32",
                              &uint32_type_info, oga_get_rotation,
                              oga_set_rotation, NULL, NULL);
    object_class_property_set_description(oc, "display-rotation",
        "Rotate the emulated panel output clockwise by 0, 90, 180 or 270 "
        "degrees.  The 320x480 portrait panel is mounted so that the "
        "default 90 shows the picture as seen on the landscape device");
    object_class_property_add(oc, "battery-mv", "uint32", &uint32_type_info,
                              oga_get_battery, oga_set_battery, NULL, NULL);
    object_class_property_set_description(oc, "battery-mv",
        "Battery voltage reported by the RK817 fuel gauge (mV)");
    object_class_property_add(oc, "hwrev-adc", "uint16", &uint16_type_info,
                              oga_get_hwrev, oga_set_hwrev, NULL, NULL);
    object_class_property_set_description(oc, "hwrev-adc",
        "SARADC channel 0 board revision code (675 = rev 1.1 / Black "
        "Edition, 856 = rev 1.0, 83 = ODROID-GO Super)");
}

static const TypeInfo oga_machine_type = {
    .name = TYPE_ODROID_GO_ADVANCE_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(OdroidGoAdvanceState),
    .instance_init = oga_instance_init,
    .class_init = oga_machine_class_init,
};

static void oga_machine_register(void)
{
    type_register_static(&oga_machine_type);
}

type_init(oga_machine_register)
