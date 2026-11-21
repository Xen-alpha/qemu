/*
 * Synopsys DesignWare Mobile Storage Host Controller (dw-mshc) model
 *
 * Used for the SDMMC/SDIO/eMMC controllers of Rockchip SoCs
 * ("rockchip,rk3288-dw-mshc", version 2.70a).  Supports:
 *  - command path with short/long responses, timeouts, auto-stop
 *  - PIO data transfers through the FIFO window at 0x200
 *  - the internal DMA controller (IDMAC) with 32-bit descriptors in
 *    chained or ring mode
 *  - card detect and write protect from the SD bus
 *
 * Commands complete after a short delay; data transfers are performed
 * at once (DMA) or on FIFO access (PIO).
 *
 * Register layout follows drivers/mmc/host/dw_mmc.h (Linux) and
 * include/dwmmc.h (u-boot).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/sd/dw_mmc.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"

#define SDMMC_CTRL          0x000
#define SDMMC_PWREN         0x004
#define SDMMC_CLKDIV        0x008
#define SDMMC_CLKSRC        0x00c
#define SDMMC_CLKENA        0x010
#define SDMMC_TMOUT         0x014
#define SDMMC_CTYPE         0x018
#define SDMMC_BLKSIZ        0x01c
#define SDMMC_BYTCNT        0x020
#define SDMMC_INTMASK       0x024
#define SDMMC_CMDARG        0x028
#define SDMMC_CMD           0x02c
#define SDMMC_RESP0         0x030
#define SDMMC_RESP1         0x034
#define SDMMC_RESP2         0x038
#define SDMMC_RESP3         0x03c
#define SDMMC_MINTSTS       0x040
#define SDMMC_RINTSTS       0x044
#define SDMMC_STATUS        0x048
#define SDMMC_FIFOTH        0x04c
#define SDMMC_CDETECT       0x050
#define SDMMC_WRTPRT        0x054
#define SDMMC_GPIO          0x058
#define SDMMC_TCBCNT        0x05c
#define SDMMC_TBBCNT        0x060
#define SDMMC_DEBNCE        0x064
#define SDMMC_USRID         0x068
#define SDMMC_VERID         0x06c
#define SDMMC_HCON          0x070
#define SDMMC_UHS_REG       0x074
#define SDMMC_RST_N         0x078
#define SDMMC_BMOD          0x080
#define SDMMC_PLDMND        0x084
#define SDMMC_DBADDR        0x088
#define SDMMC_IDSTS         0x08c
#define SDMMC_IDINTEN       0x090
#define SDMMC_DSCADDR       0x094
#define SDMMC_BUFADDR       0x098
#define SDMMC_CDTHRCTL      0x100
#define SDMMC_EMMC_DDR      0x10c
#define SDMMC_ENABLE_SHIFT  0x110
#define SDMMC_DATA          0x200

#define CTRL_RESET          (1u << 0)
#define CTRL_FIFO_RESET     (1u << 1)
#define CTRL_DMA_RESET      (1u << 2)
#define CTRL_INT_ENABLE     (1u << 4)
#define CTRL_DMA_ENABLE     (1u << 5)
#define CTRL_USE_IDMAC      (1u << 25)

#define INT_CD              (1u << 0)
#define INT_RESP_ERR        (1u << 1)
#define INT_CMD_DONE        (1u << 2)
#define INT_DATA_OVER       (1u << 3)
#define INT_TXDR            (1u << 4)
#define INT_RXDR            (1u << 5)
#define INT_RCRC            (1u << 6)
#define INT_DCRC            (1u << 7)
#define INT_RTO             (1u << 8)
#define INT_DRTO            (1u << 9)
#define INT_HTO             (1u << 10)
#define INT_FRUN            (1u << 11)
#define INT_HLE             (1u << 12)
#define INT_SBE             (1u << 13)
#define INT_ACD             (1u << 14)
#define INT_EBE             (1u << 15)

#define CMD_INDEX_MASK      0x3f
#define CMD_RESP_EXP        (1u << 6)
#define CMD_RESP_LONG       (1u << 7)
#define CMD_RESP_CRC        (1u << 8)
#define CMD_DAT_EXP         (1u << 9)
#define CMD_DAT_WR          (1u << 10)
#define CMD_STRM_MODE       (1u << 11)
#define CMD_SEND_STOP       (1u << 12)
#define CMD_PRV_DAT_WAIT    (1u << 13)
#define CMD_STOP            (1u << 14)
#define CMD_INIT            (1u << 15)
#define CMD_UPD_CLK         (1u << 21)
#define CMD_VOLT_SWITCH     (1u << 28)
#define CMD_USE_HOLD_REG    (1u << 29)
#define CMD_START           (1u << 31)

#define STATUS_FIFO_EMPTY   (1u << 2)
#define STATUS_FIFO_FULL    (1u << 3)
#define STATUS_DATA_BUSY    (1u << 9)
#define STATUS_FCNT_SHIFT   17

#define BMOD_SWR            (1u << 0)
#define BMOD_FB             (1u << 1)
#define BMOD_DSL_SHIFT      2
#define BMOD_DSL_MASK       (0x1f << BMOD_DSL_SHIFT)
#define BMOD_DE             (1u << 7)

#define IDSTS_TI            (1u << 0)
#define IDSTS_RI            (1u << 1)
#define IDSTS_FBE           (1u << 2)
#define IDSTS_DU            (1u << 4)
#define IDSTS_CES           (1u << 5)
#define IDSTS_NIS           (1u << 8)
#define IDSTS_AIS           (1u << 9)
#define IDSTS_W1C_MASK      0x337

#define DES0_DIC            (1u << 1)
#define DES0_LD             (1u << 2)
#define DES0_FD             (1u << 3)
#define DES0_CH             (1u << 4)
#define DES0_ER             (1u << 5)
#define DES0_CES            (1u << 30)
#define DES0_OWN            (1u << 31)

/*
 * HCON: SD/MMC card type, 1 card, AHB, 32-bit host data bus,
 * 32-bit address width, IDMAC, 32-bit DMA data width, FIFO RAM inside,
 * hold register, 1 clock divider, 32-bit IDMAC addressing.
 */
#define DW_MMC_HCON         ((1u << 0) | (1u << 6) | (1u << 7) | \
                             (31u << 10) | (0u << 16) | (1u << 18) | \
                             (1u << 21) | (1u << 22))

#define DW_MMC_CMD_DELAY_NS 2000

static void dw_mmc_update_irq(DWMMCState *s)
{
    uint32_t idsts = s->idsts;
    bool level;

    if (idsts & (IDSTS_TI | IDSTS_RI)) {
        idsts |= IDSTS_NIS;
    }
    if (idsts & (IDSTS_FBE | IDSTS_DU | IDSTS_CES)) {
        idsts |= IDSTS_AIS;
    }
    s->idsts = idsts;

    level = ((s->ctrl & CTRL_INT_ENABLE) && (s->rintsts & s->intmask)) ||
            (s->idsts & s->idinten & IDSTS_W1C_MASK);
    qemu_set_irq(s->irq, level);
}

static bool dw_mmc_use_idmac(DWMMCState *s)
{
    return (s->ctrl & CTRL_USE_IDMAC) && (s->bmod & BMOD_DE);
}

static void dw_mmc_auto_stop(DWMMCState *s)
{
    SDRequest req = { .cmd = 12, .arg = 0 };
    uint8_t resp[16];

    if (sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp)) == 4) {
        s->resp[1] = ldl_be_p(resp);
    }
    s->rintsts |= INT_ACD;
}

static void dw_mmc_data_done(DWMMCState *s)
{
    s->data_active = false;
    s->rintsts &= ~(INT_RXDR | INT_TXDR);
    s->rintsts |= INT_DATA_OVER;
    if (s->cmd & CMD_SEND_STOP) {
        dw_mmc_auto_stop(s);
    }
}

/* Walk the IDMAC descriptor list and move all data */
static void dw_mmc_idmac_transfer(DWMMCState *s)
{
    uint32_t desc = s->dbaddr;
    uint32_t remaining = s->bytcnt;
    unsigned dsl = (s->bmod & BMOD_DSL_MASK) >> BMOD_DSL_SHIFT;
    uint8_t buf[4096];
    int guard = 0;

    while (remaining) {
        uint32_t d[4];
        uint32_t sizes[2], addrs[2];

        if (++guard > 65536) {
            break;
        }
        s->dscaddr = desc;
        if (dma_memory_read(&s->dma_as, desc, d, sizeof(d),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            s->idsts |= IDSTS_FBE;
            return;
        }
        for (int i = 0; i < 4; i++) {
            d[i] = le32_to_cpu(d[i]);
        }
        if (!(d[0] & DES0_OWN)) {
            s->idsts |= IDSTS_DU;
            return;
        }
        sizes[0] = d[1] & 0x1fff;
        sizes[1] = (d[0] & DES0_CH) ? 0 : (d[1] >> 13) & 0x1fff;
        addrs[0] = d[2];
        addrs[1] = d[3];

        for (int b = 0; b < 2 && remaining; b++) {
            uint32_t len = MIN(sizes[b], remaining);
            uint32_t addr = addrs[b];

            s->bufaddr = addr;
            while (len) {
                uint32_t chunk = MIN(len, sizeof(buf));

                if (s->cmd & CMD_DAT_WR) {
                    dma_memory_read(&s->dma_as, addr, buf, chunk,
                                    MEMTXATTRS_UNSPECIFIED);
                    sdbus_write_data(&s->sdbus, buf, chunk);
                } else {
                    sdbus_read_data(&s->sdbus, buf, chunk);
                    dma_memory_write(&s->dma_as, addr, buf, chunk,
                                     MEMTXATTRS_UNSPECIFIED);
                }
                addr += chunk;
                len -= chunk;
                remaining -= chunk;
                s->tcbcnt += chunk;
                s->tbbcnt += chunk;
            }
        }

        /* hand the descriptor back to the host */
        d[0] &= ~DES0_OWN;
        d[0] = cpu_to_le32(d[0]);
        dma_memory_write(&s->dma_as, desc, &d[0], 4, MEMTXATTRS_UNSPECIFIED);
        d[0] = le32_to_cpu(d[0]);

        if (d[0] & DES0_LD) {
            break;
        }
        if (d[0] & DES0_CH) {
            desc = d[3];
        } else if (d[0] & DES0_ER) {
            desc = s->dbaddr;
        } else {
            desc += 16 + dsl * 4;
        }
    }
    s->dscaddr = desc;
    s->idsts |= (s->cmd & CMD_DAT_WR) ? IDSTS_TI : IDSTS_RI;
    dw_mmc_data_done(s);
}

static void dw_mmc_start_data(DWMMCState *s)
{
    s->tcbcnt = 0;
    s->tbbcnt = 0;
    s->data_write = !!(s->cmd & CMD_DAT_WR);
    s->data_remaining = s->bytcnt;

    if (dw_mmc_use_idmac(s)) {
        dw_mmc_idmac_transfer(s);
        return;
    }
    if (!s->data_remaining) {
        dw_mmc_data_done(s);
        return;
    }
    s->data_active = true;
    s->rintsts |= s->data_write ? INT_TXDR : INT_RXDR;
}

static void dw_mmc_run_command(void *opaque)
{
    DWMMCState *s = opaque;
    SDRequest req;
    uint8_t resp[16];
    size_t rlen;

    req.cmd = s->cmd & CMD_INDEX_MASK;
    req.arg = s->cmdarg;
    req.crc = 0;

    rlen = sdbus_do_command(&s->sdbus, &req, resp, sizeof(resp));

    s->cmd &= ~CMD_START;
    if (s->cmd & CMD_RESP_EXP) {
        if (s->cmd & CMD_RESP_LONG) {
            if (rlen != 16) {
                goto timeout;
            }
            s->resp[0] = ldl_be_p(&resp[12]);
            s->resp[1] = ldl_be_p(&resp[8]);
            s->resp[2] = ldl_be_p(&resp[4]);
            s->resp[3] = ldl_be_p(&resp[0]);
        } else {
            if (rlen != 4) {
                goto timeout;
            }
            s->resp[0] = ldl_be_p(&resp[0]);
        }
    }
    s->rintsts |= INT_CMD_DONE;
    if (s->cmd & CMD_DAT_EXP) {
        dw_mmc_start_data(s);
    }
    dw_mmc_update_irq(s);
    return;

timeout:
    s->rintsts |= INT_CMD_DONE | INT_RTO;
    dw_mmc_update_irq(s);
}

static void dw_mmc_write_cmd(DWMMCState *s, uint32_t value)
{
    s->cmd = value;
    if (!(value & CMD_START)) {
        return;
    }
    if (value & CMD_UPD_CLK) {
        /* clock update only: no command is sent to the card */
        s->cmd &= ~CMD_START;
        return;
    }
    timer_mod(s->cmd_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + DW_MMC_CMD_DELAY_NS);
}

static uint32_t dw_mmc_fifo_words(DWMMCState *s)
{
    if (!s->data_active || s->data_write) {
        return 0;
    }
    return MIN(DIV_ROUND_UP(s->data_remaining, 4), s->fifo_depth);
}

static uint32_t dw_mmc_fifo_read(DWMMCState *s)
{
    uint32_t val = 0;

    if (!s->data_active || s->data_write) {
        qemu_log_mask(LOG_GUEST_ERROR, "dw-mshc: FIFO underrun\n");
        return 0;
    }
    for (int i = 0; i < 4 && s->data_remaining; i++) {
        val |= (uint32_t)sdbus_read_byte(&s->sdbus) << (8 * i);
        s->data_remaining--;
        s->tcbcnt++;
        s->tbbcnt++;
    }
    if (!s->data_remaining) {
        dw_mmc_data_done(s);
        dw_mmc_update_irq(s);
    }
    return val;
}

static void dw_mmc_fifo_write(DWMMCState *s, uint32_t val)
{
    if (!s->data_active || !s->data_write) {
        qemu_log_mask(LOG_GUEST_ERROR, "dw-mshc: FIFO overrun\n");
        return;
    }
    for (int i = 0; i < 4 && s->data_remaining; i++) {
        sdbus_write_byte(&s->sdbus, val >> (8 * i));
        s->data_remaining--;
        s->tcbcnt++;
        s->tbbcnt++;
    }
    if (!s->data_remaining) {
        dw_mmc_data_done(s);
        dw_mmc_update_irq(s);
    }
}

static void dw_mmc_soft_reset(DWMMCState *s, uint32_t what)
{
    if (what & CTRL_RESET) {
        timer_del(s->cmd_timer);
        s->cmd &= ~CMD_START;
        s->data_active = false;
        s->rintsts = 0;
    }
    if (what & (CTRL_FIFO_RESET | CTRL_RESET)) {
        s->data_active = false;
    }
    if (what & CTRL_DMA_RESET) {
        s->idsts = 0;
    }
}

static uint64_t dw_mmc_read(void *opaque, hwaddr offset, unsigned size)
{
    DWMMCState *s = opaque;
    uint32_t val;

    if (offset >= SDMMC_DATA) {
        return dw_mmc_fifo_read(s);
    }

    switch (offset) {
    case SDMMC_CTRL:
        return s->ctrl;
    case SDMMC_PWREN:
        return s->pwren;
    case SDMMC_CLKDIV:
        return s->clkdiv;
    case SDMMC_CLKSRC:
        return s->clksrc;
    case SDMMC_CLKENA:
        return s->clkena;
    case SDMMC_TMOUT:
        return s->tmout;
    case SDMMC_CTYPE:
        return s->ctype;
    case SDMMC_BLKSIZ:
        return s->blksiz;
    case SDMMC_BYTCNT:
        return s->bytcnt;
    case SDMMC_INTMASK:
        return s->intmask;
    case SDMMC_CMDARG:
        return s->cmdarg;
    case SDMMC_CMD:
        return s->cmd;
    case SDMMC_RESP0:
    case SDMMC_RESP1:
    case SDMMC_RESP2:
    case SDMMC_RESP3:
        return s->resp[(offset - SDMMC_RESP0) >> 2];
    case SDMMC_MINTSTS:
        return (s->ctrl & CTRL_INT_ENABLE) ? (s->rintsts & s->intmask) : 0;
    case SDMMC_RINTSTS:
        return s->rintsts;
    case SDMMC_STATUS:
        val = dw_mmc_fifo_words(s);
        val = (val << STATUS_FCNT_SHIFT);
        if (!dw_mmc_fifo_words(s)) {
            val |= STATUS_FIFO_EMPTY;
        }
        /* command FSM idle, card present on DAT3 */
        val |= (1u << 8);
        return val;
    case SDMMC_FIFOTH:
        return s->fifoth;
    case SDMMC_CDETECT:
        return sdbus_get_inserted(&s->sdbus) ? 0 : 1;
    case SDMMC_WRTPRT:
        return sdbus_get_readonly(&s->sdbus) ? 1 : 0;
    case SDMMC_GPIO:
        return 0;
    case SDMMC_TCBCNT:
        return s->tcbcnt;
    case SDMMC_TBBCNT:
        return s->tbbcnt;
    case SDMMC_DEBNCE:
        return s->debnce;
    case SDMMC_USRID:
        return s->usrid;
    case SDMMC_VERID:
        return s->verid;
    case SDMMC_HCON:
        return DW_MMC_HCON;
    case SDMMC_UHS_REG:
        return s->uhs_reg;
    case SDMMC_RST_N:
        return s->rst_n;
    case SDMMC_BMOD:
        return s->bmod;
    case SDMMC_PLDMND:
        return 0;
    case SDMMC_DBADDR:
        return s->dbaddr;
    case SDMMC_IDSTS:
        return s->idsts;
    case SDMMC_IDINTEN:
        return s->idinten;
    case SDMMC_DSCADDR:
        return s->dscaddr;
    case SDMMC_BUFADDR:
        return s->bufaddr;
    case SDMMC_CDTHRCTL:
        return s->cardthrctl;
    case SDMMC_EMMC_DDR:
        return s->emmc_ddr;
    case SDMMC_ENABLE_SHIFT:
        return s->enable_shift;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "dw-mshc: bad read 0x%" HWADDR_PRIx "\n",
                  offset);
    return 0;
}

static void dw_mmc_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    DWMMCState *s = opaque;

    if (offset >= SDMMC_DATA) {
        dw_mmc_fifo_write(s, value);
        return;
    }

    switch (offset) {
    case SDMMC_CTRL:
        dw_mmc_soft_reset(s, value);
        /* reset bits are self clearing */
        s->ctrl = value & ~(CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET);
        break;
    case SDMMC_PWREN:
        s->pwren = value;
        break;
    case SDMMC_CLKDIV:
        s->clkdiv = value;
        break;
    case SDMMC_CLKSRC:
        s->clksrc = value;
        break;
    case SDMMC_CLKENA:
        s->clkena = value;
        break;
    case SDMMC_TMOUT:
        s->tmout = value;
        break;
    case SDMMC_CTYPE:
        s->ctype = value;
        break;
    case SDMMC_BLKSIZ:
        s->blksiz = value & 0xffff;
        break;
    case SDMMC_BYTCNT:
        s->bytcnt = value;
        break;
    case SDMMC_INTMASK:
        s->intmask = value;
        break;
    case SDMMC_CMDARG:
        s->cmdarg = value;
        break;
    case SDMMC_CMD:
        dw_mmc_write_cmd(s, value);
        break;
    case SDMMC_RINTSTS:
        s->rintsts &= ~value;
        /* the data request bits re-assert while the FIFO needs service */
        if (s->data_active) {
            s->rintsts |= s->data_write ? INT_TXDR : INT_RXDR;
        }
        break;
    case SDMMC_FIFOTH:
        s->fifoth = value;
        break;
    case SDMMC_DEBNCE:
        s->debnce = value & 0xffffff;
        break;
    case SDMMC_USRID:
        s->usrid = value;
        break;
    case SDMMC_UHS_REG:
        s->uhs_reg = value;
        sdbus_set_voltage(&s->sdbus, (value & 1) ? 1800 : 3300);
        break;
    case SDMMC_RST_N:
        s->rst_n = value;
        break;
    case SDMMC_BMOD:
        if (value & BMOD_SWR) {
            s->idsts = 0;
        }
        s->bmod = value & ~BMOD_SWR;
        break;
    case SDMMC_PLDMND:
        /* poll demand: descriptors are processed synchronously */
        break;
    case SDMMC_DBADDR:
        s->dbaddr = value;
        s->dscaddr = value;
        break;
    case SDMMC_IDSTS:
        s->idsts &= ~(value & IDSTS_W1C_MASK);
        if (!(s->idsts & (IDSTS_TI | IDSTS_RI))) {
            s->idsts &= ~IDSTS_NIS;
        }
        if (!(s->idsts & (IDSTS_FBE | IDSTS_DU | IDSTS_CES))) {
            s->idsts &= ~IDSTS_AIS;
        }
        break;
    case SDMMC_IDINTEN:
        s->idinten = value & IDSTS_W1C_MASK;
        break;
    case SDMMC_CDTHRCTL:
        s->cardthrctl = value;
        break;
    case SDMMC_EMMC_DDR:
        s->emmc_ddr = value;
        break;
    case SDMMC_ENABLE_SHIFT:
        s->enable_shift = value;
        break;
    case SDMMC_GPIO:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "dw-mshc: write to RO/bad offset 0x%"
                      HWADDR_PRIx "\n", offset);
        return;
    }
    dw_mmc_update_irq(s);
}

static const MemoryRegionOps dw_mmc_ops = {
    .read = dw_mmc_read,
    .write = dw_mmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dw_mmc_set_inserted(DeviceState *dev, bool inserted)
{
    DWMMCState *s = DW_MMC(dev);

    s->rintsts |= INT_CD;
    dw_mmc_update_irq(s);
}

static void dw_mmc_reset_hold(Object *obj, ResetType type)
{
    DWMMCState *s = DW_MMC(obj);

    timer_del(s->cmd_timer);
    s->ctrl = 0;
    s->pwren = 0;
    s->clkdiv = 0;
    s->clksrc = 0;
    s->clkena = 0;
    s->tmout = 0xffffff40;
    s->ctype = 0;
    s->blksiz = 0x200;
    s->bytcnt = 0x200;
    s->intmask = 0;
    s->cmdarg = 0;
    s->cmd = 0;
    memset(s->resp, 0, sizeof(s->resp));
    s->rintsts = 0;
    s->fifoth = (s->fifo_depth - 1) << 16;
    s->debnce = 0xffffff;
    s->usrid = 0;
    s->uhs_reg = 0;
    s->rst_n = 1;
    s->bmod = 0;
    s->dbaddr = 0;
    s->idsts = 0;
    s->idinten = 0;
    s->dscaddr = 0;
    s->bufaddr = 0;
    s->cardthrctl = 0;
    s->emmc_ddr = 0;
    s->enable_shift = 0;
    s->tcbcnt = 0;
    s->tbbcnt = 0;
    s->data_active = false;
    s->data_write = false;
    s->data_remaining = 0;
}

static void dw_mmc_realize(DeviceState *dev, Error **errp)
{
    DWMMCState *s = DW_MMC(dev);

    if (!s->fifo_depth || s->fifo_depth > 4096) {
        error_setg(errp, "dw-mshc: invalid fifo-depth");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr ? s->dma_mr : get_system_memory(),
                       "dw-mshc-dma");
}

static void dw_mmc_init(Object *obj)
{
    DWMMCState *s = DW_MMC(obj);

    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_DW_MMC_BUS, DEVICE(s),
              "sd-bus");
    memory_region_init_io(&s->iomem, obj, &dw_mmc_ops, s, TYPE_DW_MMC,
                          0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->cmd_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dw_mmc_run_command, s);
}

static const Property dw_mmc_props[] = {
    DEFINE_PROP_UINT32("fifo-depth", DWMMCState, fifo_depth, 256),
    DEFINE_PROP_UINT32("verid", DWMMCState, verid, 0x5342270a),
    DEFINE_PROP_LINK("dma-memory", DWMMCState, dma_mr, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static const VMStateDescription vmstate_dw_mmc = {
    .name = TYPE_DW_MMC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, DWMMCState),
        VMSTATE_UINT32(pwren, DWMMCState),
        VMSTATE_UINT32(clkdiv, DWMMCState),
        VMSTATE_UINT32(clksrc, DWMMCState),
        VMSTATE_UINT32(clkena, DWMMCState),
        VMSTATE_UINT32(tmout, DWMMCState),
        VMSTATE_UINT32(ctype, DWMMCState),
        VMSTATE_UINT32(blksiz, DWMMCState),
        VMSTATE_UINT32(bytcnt, DWMMCState),
        VMSTATE_UINT32(intmask, DWMMCState),
        VMSTATE_UINT32(cmdarg, DWMMCState),
        VMSTATE_UINT32(cmd, DWMMCState),
        VMSTATE_UINT32_ARRAY(resp, DWMMCState, 4),
        VMSTATE_UINT32(rintsts, DWMMCState),
        VMSTATE_UINT32(fifoth, DWMMCState),
        VMSTATE_UINT32(debnce, DWMMCState),
        VMSTATE_UINT32(usrid, DWMMCState),
        VMSTATE_UINT32(uhs_reg, DWMMCState),
        VMSTATE_UINT32(rst_n, DWMMCState),
        VMSTATE_UINT32(bmod, DWMMCState),
        VMSTATE_UINT32(dbaddr, DWMMCState),
        VMSTATE_UINT32(idsts, DWMMCState),
        VMSTATE_UINT32(idinten, DWMMCState),
        VMSTATE_UINT32(dscaddr, DWMMCState),
        VMSTATE_UINT32(bufaddr, DWMMCState),
        VMSTATE_UINT32(cardthrctl, DWMMCState),
        VMSTATE_UINT32(emmc_ddr, DWMMCState),
        VMSTATE_UINT32(enable_shift, DWMMCState),
        VMSTATE_UINT32(tcbcnt, DWMMCState),
        VMSTATE_UINT32(tbbcnt, DWMMCState),
        VMSTATE_BOOL(data_active, DWMMCState),
        VMSTATE_BOOL(data_write, DWMMCState),
        VMSTATE_UINT32(data_remaining, DWMMCState),
        VMSTATE_TIMER_PTR(cmd_timer, DWMMCState),
        VMSTATE_END_OF_LIST()
    }
};

static void dw_mmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dw_mmc_realize;
    dc->vmsd = &vmstate_dw_mmc;
    device_class_set_props(dc, dw_mmc_props);
    rc->phases.hold = dw_mmc_reset_hold;
}

static void dw_mmc_bus_class_init(ObjectClass *klass, const void *data)
{
    SDBusClass *sbc = SD_BUS_CLASS(klass);

    sbc->set_inserted = dw_mmc_set_inserted;
}

static const TypeInfo dw_mmc_types[] = {
    {
        .name = TYPE_DW_MMC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(DWMMCState),
        .instance_init = dw_mmc_init,
        .class_init = dw_mmc_class_init,
    }, {
        .name = TYPE_DW_MMC_BUS,
        .parent = TYPE_SD_BUS,
        .instance_size = sizeof(SDBus),
        .class_init = dw_mmc_bus_class_init,
    },
};

DEFINE_TYPES(dw_mmc_types)
