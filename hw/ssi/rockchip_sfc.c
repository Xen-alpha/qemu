/*
 * Rockchip Serial Flash Controller (SFC) with an attached SPI NOR flash
 *
 * The SFC is a command level controller: software programs the opcode,
 * address width, dummy cycles, direction and length in SFC_CMD, the
 * address in SFC_ADDR and moves the data through the FIFO (SFC_DATA) or
 * by DMA.  Register layout from drivers/rkflash/sfc.[ch] of the
 * Hardkernel u-boot.
 *
 * Because the controller sees whole flash operations, the NOR flash is
 * modelled here directly (instead of an SSI bus): JEDEC id (default
 * XTX XT25F128B, 0x0b4018, as on the ODROID-GO Advance), status
 * registers with WEL/QE, read (1/2/4 line variants), page program,
 * 4K/32K/64K/chip erase, unique id.  Contents are backed by an optional
 * MTD drive and written through to it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/ssi/rockchip_sfc.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/dma.h"

#define SFC_CTRL        0x00
#define SFC_IMR         0x04
#define SFC_ICLR        0x08
#define SFC_FTLR        0x0c
#define SFC_RCVR        0x10
#define SFC_AX          0x14
#define SFC_ABIT        0x18
#define SFC_MASKISR     0x1c
#define SFC_FSR         0x20
#define SFC_SR          0x24
#define SFC_RAWISR      0x28
#define SFC_VER         0x2c
#define SFC_QOP         0x30
#define SFC_DMA_TRIGGER 0x80
#define SFC_DMA_ADDR    0x84
#define SFC_CMD         0x100
#define SFC_ADDR        0x104
#define SFC_DATA        0x108

#define FSR_TXFULL      (1u << 0)
#define FSR_TXEMPTY     (1u << 1)
#define FSR_RXEMPTY     (1u << 2)
#define FSR_RXFULL      (1u << 3)

#define INT_FINISH      (1u << 4)
#define INT_DMA         (1u << 7)


#define CMD_OPCODE(c)   ((c) & 0xff)
#define CMD_RW(c)       (((c) >> 12) & 1)
#define CMD_ADDRBITS(c) (((c) >> 14) & 3)
#define CMD_DATASIZE(c) (((c) >> 16) & 0x3fff)

#define SFC_VERSION     0x00000001

/* SPI NOR opcodes */
#define OP_WRSR         0x01
#define OP_PP           0x02
#define OP_READ         0x03
#define OP_WRDI         0x04
#define OP_RDSR         0x05
#define OP_WREN         0x06
#define OP_FAST_READ    0x0b
#define OP_RDSR3        0x15
#define OP_SE_4K        0x20
#define OP_WRSR3        0x11
#define OP_WRSR2        0x31
#define OP_QPP          0x32
#define OP_RDSR2        0x35
#define OP_DOR          0x3b
#define OP_UID          0x4b
#define OP_BE_32K       0x52
#define OP_SFDP         0x5a
#define OP_CE           0x60
#define OP_RSTEN        0x66
#define OP_QOR          0x6b
#define OP_REMS         0x90
#define OP_RST          0x99
#define OP_RDID         0x9f
#define OP_RES          0xab
#define OP_DP           0xb9
#define OP_DIOR         0xbb
#define OP_CE2          0xc7
#define OP_BE_64K       0xd8
#define OP_QIOR         0xeb

static void sfc_update_irq(RockchipSFCState *s)
{
    qemu_set_irq(s->irq, !!(s->rawisr & ~s->imr & 0xff));
}

static void sfc_flash_sync(RockchipSFCState *s, uint32_t off, uint32_t len)
{
    if (s->blk && blk_is_writable(s->blk)) {
        blk_pwrite(s->blk, off, len, s->storage + off, 0);
    }
}

static void sfc_flash_erase(RockchipSFCState *s, uint32_t addr,
                            uint32_t size)
{
    if (!s->wel) {
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-sfc: erase without WEL\n");
        return;
    }
    addr &= ~(size - 1) & (s->flash_size - 1);
    memset(s->storage + addr, 0xff, size);
    sfc_flash_sync(s, addr, size);
    s->wel = false;
}

static void sfc_flash_program(RockchipSFCState *s, uint32_t addr,
                              const uint8_t *data, uint32_t len)
{
    uint32_t page = addr & ~0xffu;

    if (!s->wel) {
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-sfc: program without WEL\n");
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        /* page program wraps inside the 256 byte page */
        uint32_t a = (page + ((addr + i) & 0xff)) & (s->flash_size - 1);

        s->storage[a] &= data[i];
    }
    sfc_flash_sync(s, page & (s->flash_size - 1), 256);
    s->wel = false;
}

/*
 * SFDP: JESD216 header + Basic Flash Parameter Table (v1.0, 9 DWORDs)
 * of a 128 Mbit quad SPI NOR with 4K/32K/64K erase, 1-1-2/1-2-2/1-1-4/
 * 1-4-4 fast reads (values as reported by XT25F128B class parts).
 */
static const uint8_t sfc_sfdp[] = {
    /* SFDP header */
    'S', 'F', 'D', 'P', 0x00, 0x01, 0x00, 0xff,
    /* parameter header 0: BFPT v1.0, 9 DWORDs at 0x30 */
    0x00, 0x00, 0x01, 0x09, 0x30, 0x00, 0x00, 0xff,
    [0x30] =
    0xe5, 0x20, 0xf9, 0xff,     /* DW1: 4K erase 0x20, fast reads */
    0xff, 0xff, 0xff, 0x07,     /* DW2: density 128 Mbit - 1 */
    0x44, 0xeb, 0x08, 0x6b,     /* DW3: 1-4-4 0xeb, 1-1-4 0x6b */
    0x08, 0x3b, 0x42, 0xbb,     /* DW4: 1-1-2 0x3b, 1-2-2 0xbb */
    0xfe, 0xff, 0xff, 0xff,     /* DW5 */
    0xff, 0xff, 0x00, 0xff,     /* DW6 */
    0xff, 0xff, 0x40, 0xeb,     /* DW7 */
    0x0c, 0x20, 0x0f, 0x52,     /* DW8: 4K 0x20, 32K 0x52 */
    0x10, 0xd8, 0x00, 0xff,     /* DW9: 64K 0xd8 */
};

/* Execute a command that returns data (fills s->buf with s->len bytes) */
static void sfc_flash_read_cmd(RockchipSFCState *s, uint8_t op)
{
    uint32_t a = s->addr & (s->flash_size - 1);

    memset(s->buf, 0xff, s->len);
    switch (op) {
    case OP_READ:
    case OP_FAST_READ:
    case OP_DOR:
    case OP_QOR:
    case OP_DIOR:
    case OP_QIOR:
        for (uint32_t i = 0; i < s->len; i++) {
            s->buf[i] = s->storage[(a + i) & (s->flash_size - 1)];
        }
        break;
    case OP_RDID:
        for (uint32_t i = 0; i < s->len; i++) {
            s->buf[i] = i < 3 ? s->jedec_id >> (8 * (2 - i)) : 0;
        }
        break;
    case OP_RDSR:
        for (uint32_t i = 0; i < s->len; i++) {
            s->buf[i] = s->sr[0] | (s->wel ? 2 : 0);
        }
        break;
    case OP_RDSR2:
        memset(s->buf, s->sr[1], s->len);
        break;
    case OP_RDSR3:
        memset(s->buf, s->sr[2], s->len);
        break;
    case OP_REMS:
        for (uint32_t i = 0; i < s->len; i++) {
            s->buf[i] = (i & 1) ? (s->jedec_id & 0xff) - 1 :
                                  s->jedec_id >> 16;
        }
        break;
    case OP_RES:
        memset(s->buf, (s->jedec_id & 0xff) - 1, s->len);
        break;
    case OP_UID:
        for (uint32_t i = 0; i < s->len; i++) {
            s->buf[i] = 0x51 + i;
        }
        break;
    case OP_SFDP:
        for (uint32_t i = 0; i < s->len; i++) {
            uint32_t o = (s->addr & 0xffffff) + i;

            s->buf[i] = o < sizeof(sfc_sfdp) ? sfc_sfdp[o] : 0xff;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rockchip-sfc: read opcode 0x%02x\n", op);
    }
}

static void sfc_flash_write_cmd(RockchipSFCState *s, uint8_t op)
{
    switch (op) {
    case OP_WREN:
        s->wel = true;
        break;
    case OP_WRDI:
        s->wel = false;
        break;
    case OP_WRSR:
        if (s->len >= 1) {
            s->sr[0] = s->buf[0] & 0xfc;
        }
        if (s->len >= 2) {
            s->sr[1] = s->buf[1];
        }
        s->wel = false;
        break;
    case OP_WRSR2:
        if (s->len >= 1) {
            s->sr[1] = s->buf[0];
        }
        s->wel = false;
        break;
    case OP_WRSR3:
        if (s->len >= 1) {
            s->sr[2] = s->buf[0];
        }
        s->wel = false;
        break;
    case OP_PP:
    case OP_QPP:
        sfc_flash_program(s, s->addr, s->buf, s->len);
        break;
    case OP_SE_4K:
        sfc_flash_erase(s, s->addr, 4 * KiB);
        break;
    case OP_BE_32K:
        sfc_flash_erase(s, s->addr, 32 * KiB);
        break;
    case OP_BE_64K:
        sfc_flash_erase(s, s->addr, 64 * KiB);
        break;
    case OP_CE:
    case OP_CE2:
        if (s->wel) {
            memset(s->storage, 0xff, s->flash_size);
            sfc_flash_sync(s, 0, s->flash_size);
            s->wel = false;
        }
        break;
    case OP_RSTEN:
    case OP_RST:
    case OP_DP:
    case OP_RES:
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rockchip-sfc: write opcode 0x%02x\n", op);
    }
}

static void sfc_finish(RockchipSFCState *s)
{
    s->cmd_pending = false;
    s->rawisr |= INT_FINISH;
    sfc_update_irq(s);
}

/*
 * All parameters of the command are known: start it.  Data phases can
 * be serviced either through the FIFO (SFC_DATA) or by the built-in DMA
 * engine (SFC_DMA_TRIGGER); the hardware does not need to be told in
 * advance which one software will use.
 */
static void sfc_start(RockchipSFCState *s)
{
    uint8_t op = CMD_OPCODE(s->cmd);

    s->len = MIN(CMD_DATASIZE(s->cmd), ROCKCHIP_SFC_MAX_XFER);
    s->pos = 0;
    s->rx_active = false;
    s->tx_active = false;

    if (!s->len) {
        /* no data phase: e.g. WREN, erase */
        sfc_flash_write_cmd(s, op);
        sfc_finish(s);
        return;
    }
    if (CMD_RW(s->cmd) == 0) {
        sfc_flash_read_cmd(s, op);
        s->rx_active = true;
    } else {
        s->tx_active = true;
    }
}

static void sfc_dma(RockchipSFCState *s)
{
    if (!s->cmd_pending) {
        return;
    }
    if (s->rx_active) {
        dma_memory_write(&s->dma_as, s->dma_addr, s->buf + s->pos,
                         s->len - s->pos, MEMTXATTRS_UNSPECIFIED);
        s->pos = s->len;
        s->rx_active = false;
    } else if (s->tx_active) {
        dma_memory_read(&s->dma_as, s->dma_addr, s->buf + s->pos,
                        s->len - s->pos, MEMTXATTRS_UNSPECIFIED);
        s->pos = s->len;
        s->tx_active = false;
        sfc_flash_write_cmd(s, CMD_OPCODE(s->cmd));
    } else {
        return;
    }
    s->rawisr |= INT_DMA;
    sfc_finish(s);
}

static uint32_t sfc_fsr(RockchipSFCState *s)
{
    uint32_t val = FSR_TXEMPTY;
    uint32_t rxwords = 0;

    if (s->rx_active && s->pos < s->len) {
        rxwords = MIN(DIV_ROUND_UP(s->len - s->pos, 4),
                      ROCKCHIP_SFC_FIFO_WORDS);
    }
    if (!rxwords) {
        val |= FSR_RXEMPTY;
    }
    if (rxwords == ROCKCHIP_SFC_FIFO_WORDS) {
        val |= FSR_RXFULL;
    }
    val |= ROCKCHIP_SFC_FIFO_WORDS << 8;     /* tx free level */
    val |= rxwords << 16;                   /* rx level */
    return val;
}

static void sfc_reset_ctrl(RockchipSFCState *s)
{
    s->ctrl = 0;
    s->imr = 0xffffffff;
    s->ftlr = 0x0f0f;
    s->ax = 0;
    s->abit = 0x1f;
    s->rawisr = 0;
    s->qop = 0;
    s->dma_addr = 0;
    s->cmd = 0;
    s->addr = 0;
    s->cmd_pending = false;
    s->rx_active = false;
    s->tx_active = false;
    s->len = 0;
    s->pos = 0;
}

static uint64_t sfc_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipSFCState *s = opaque;
    uint32_t val = 0;

    switch (offset) {
    case SFC_CTRL:
        return s->ctrl;
    case SFC_IMR:
        return s->imr;
    case SFC_FTLR:
        return s->ftlr;
    case SFC_RCVR:
        return 0;
    case SFC_AX:
        return s->ax;
    case SFC_ABIT:
        return s->abit;
    case SFC_MASKISR:
        return s->rawisr & ~s->imr;
    case SFC_FSR:
        return sfc_fsr(s);
    case SFC_SR:
        return s->cmd_pending ? 1 : 0;
    case SFC_RAWISR:
        return s->rawisr;
    case SFC_VER:
        return SFC_VERSION;
    case SFC_QOP:
        return s->qop;
    case SFC_DMA_ADDR:
        return s->dma_addr;
    case SFC_CMD:
        return s->cmd;
    case SFC_ADDR:
        return s->addr;
    case SFC_DATA:
        if (!s->rx_active) {
            return 0;
        }
        for (int i = 0; i < 4 && s->pos < s->len; i++) {
            val |= (uint32_t)s->buf[s->pos++] << (8 * i);
        }
        if (s->pos >= s->len) {
            s->rx_active = false;
            sfc_finish(s);
        }
        return val;
    }
    return 0;
}

static void sfc_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    RockchipSFCState *s = opaque;

    switch (offset) {
    case SFC_CTRL:
        s->ctrl = value;
        break;
    case SFC_IMR:
        s->imr = value;
        sfc_update_irq(s);
        break;
    case SFC_ICLR:
        s->rawisr &= ~value;
        sfc_update_irq(s);
        break;
    case SFC_FTLR:
        s->ftlr = value;
        break;
    case SFC_RCVR:
        if (value & 1) {
            sfc_reset_ctrl(s);
            sfc_update_irq(s);
        }
        break;
    case SFC_AX:
        s->ax = value;
        break;
    case SFC_ABIT:
        s->abit = value & 0x1f;
        break;
    case SFC_QOP:
        s->qop = value;
        break;
    case SFC_DMA_ADDR:
        s->dma_addr = value;
        break;
    case SFC_DMA_TRIGGER:
        if (value & 1) {
            sfc_dma(s);
        }
        break;
    case SFC_CMD:
        s->cmd = value;
        s->cmd_pending = true;
        s->rx_active = false;
        s->tx_active = false;
        if (!CMD_ADDRBITS(value)) {
            s->addr = 0;
            sfc_start(s);
        }
        break;
    case SFC_ADDR:
        s->addr = value;
        if (s->cmd_pending && CMD_ADDRBITS(s->cmd)) {
            sfc_start(s);
        }
        break;
    case SFC_DATA:
        if (!s->tx_active) {
            qemu_log_mask(LOG_GUEST_ERROR, "rockchip-sfc: unexpected data\n");
            break;
        }
        for (int i = 0; i < 4 && s->pos < s->len; i++) {
            s->buf[s->pos++] = value >> (8 * i);
        }
        if (s->pos >= s->len) {
            s->tx_active = false;
            sfc_flash_write_cmd(s, CMD_OPCODE(s->cmd));
            sfc_finish(s);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rockchip-sfc: write to RO/bad offset "
                      "0x%" HWADDR_PRIx "\n", offset);
    }
}

static const MemoryRegionOps sfc_ops = {
    .read = sfc_read,
    .write = sfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void sfc_reset_hold(Object *obj, ResetType type)
{
    RockchipSFCState *s = ROCKCHIP_SFC(obj);

    sfc_reset_ctrl(s);
    s->wel = false;
}

static void sfc_realize(DeviceState *dev, Error **errp)
{
    RockchipSFCState *s = ROCKCHIP_SFC(dev);

    if (!is_power_of_2(s->flash_size)) {
        error_setg(errp, "rockchip-sfc: flash-size must be a power of 2");
        return;
    }
    s->storage = blk_blockalign(s->blk, s->flash_size);
    memset(s->storage, 0xff, s->flash_size);
    if (s->blk) {
        uint64_t perm = BLK_PERM_CONSISTENT_READ |
                        (blk_supports_write_perm(s->blk) ? BLK_PERM_WRITE : 0);
        int64_t len;

        if (blk_set_perm(s->blk, perm, BLK_PERM_ALL, errp) < 0) {
            return;
        }
        len = blk_getlength(s->blk);
        if (len < 0) {
            error_setg(errp, "rockchip-sfc: cannot get drive size");
            return;
        }
        if (blk_pread(s->blk, 0, MIN(len, s->flash_size), s->storage,
                      0) < 0) {
            error_setg(errp, "rockchip-sfc: failed to read the drive");
            return;
        }
    }
    s->sr[0] = 0;
    s->sr[1] = 0;
    s->sr[2] = 0;
    address_space_init(&s->dma_as, s->dma_mr ? s->dma_mr : get_system_memory(),
                       "rockchip-sfc-dma");
}

static void sfc_init(Object *obj)
{
    RockchipSFCState *s = ROCKCHIP_SFC(obj);

    memory_region_init_io(&s->iomem, obj, &sfc_ops, s, TYPE_ROCKCHIP_SFC,
                          0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const Property sfc_props[] = {
    DEFINE_PROP_DRIVE("drive", RockchipSFCState, blk),
    DEFINE_PROP_UINT32("flash-size", RockchipSFCState, flash_size, 16 * MiB),
    DEFINE_PROP_UINT32("jedec-id", RockchipSFCState, jedec_id, 0x0b4018),
    DEFINE_PROP_LINK("dma-memory", RockchipSFCState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription vmstate_sfc = {
    .name = TYPE_ROCKCHIP_SFC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(sr, RockchipSFCState, 3),
        VMSTATE_BOOL(wel, RockchipSFCState),
        VMSTATE_UINT32(ctrl, RockchipSFCState),
        VMSTATE_UINT32(imr, RockchipSFCState),
        VMSTATE_UINT32(ftlr, RockchipSFCState),
        VMSTATE_UINT32(ax, RockchipSFCState),
        VMSTATE_UINT32(abit, RockchipSFCState),
        VMSTATE_UINT32(rawisr, RockchipSFCState),
        VMSTATE_UINT32(qop, RockchipSFCState),
        VMSTATE_UINT32(dma_addr, RockchipSFCState),
        VMSTATE_UINT32(cmd, RockchipSFCState),
        VMSTATE_UINT32(addr, RockchipSFCState),
        VMSTATE_BOOL(cmd_pending, RockchipSFCState),
        VMSTATE_UINT8_ARRAY(buf, RockchipSFCState, ROCKCHIP_SFC_MAX_XFER),
        VMSTATE_UINT32(len, RockchipSFCState),
        VMSTATE_UINT32(pos, RockchipSFCState),
        VMSTATE_BOOL(rx_active, RockchipSFCState),
        VMSTATE_BOOL(tx_active, RockchipSFCState),
        VMSTATE_END_OF_LIST()
    }
};

static void sfc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = sfc_realize;
    dc->vmsd = &vmstate_sfc;
    device_class_set_props(dc, sfc_props);
    rc->phases.hold = sfc_reset_hold;
}

static const TypeInfo sfc_info = {
    .name = TYPE_ROCKCHIP_SFC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipSFCState),
    .instance_init = sfc_init,
    .class_init = sfc_class_init,
};

static void sfc_register_types(void)
{
    type_register_static(&sfc_info);
}

type_init(sfc_register_types)
