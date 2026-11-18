/*
 * MediaTek MT6833 (Dimensity 6080 / Tecno Pova 5 Pro 5G - LH8n) Touchscreen Driver.
 *
 * Provides multi-touch tracking and coordinate calibration for Darwin's /dev/hakios-touch.
 *
 * Hardware Mapping & Citations (Tecno Pova 5 Pro 5G / LH8n):
 *   Touch Controller : FocalTech FT8726 (FT8725/FT8726 noflash family) on SPI4
 *                      Source: focaltech_noflash_common.ko fts_ts_probe_entry (.rodata:0x124864) [VERIFIED]
 *   Firmware Binary  : focaltech_ts_fw_00.bin (PRAM=61112B @ 0x000000, DRAM=8904B @ 0xD00000)
 *                      Source: focaltech_noflash_common.ko fts_fw_write_start (.text:0x108c40) [VERIFIED]
 *   Physical Screen  : 1080 x 2460 FHD+ @ 120Hz (LK fb at 0x76950000, device.conf:11) [VERIFIED]
 *   SPI4 Controller  : 0x11018000 (size 0x1000, spi-mt65xx.ko mtk_spi_fifo_transfer) [VERIFIED]
 *   GPIO Controller  : 0x10005000 (size 0x1000) [VERIFIED]
 *   TOPCKGEN Base    : 0x10000000 (size 0x1000) [VERIFIED]
 *   INFRACFG_AO Base : 0x10001000 (size 0x1000) [VERIFIED]
 *   Power Regulator  : MT6360 PMU LDO2 (vtp) @ 1.8V (regulator-default-on = 1) [VERIFIED]
 *   Reset GPIO       : GPIO 19 (PIN_TOUCH_RST 19, fts_reset_gpio, output) [VERIFIED]
 *   IRQ GPIO         : GPIO 14 (PIN_TOUCH_IRQ 14, active-low EINT, fts_irq_gpio, input) [VERIFIED]
 *   SPI4 Clock Gate  : INFRACFG_AO + 0x00a8 (MODULE_SW_CG_0) bit 25 (ifrao_spi4=0, un-gated) [VERIFIED]
 *   SPI4 Pinmux Mode : GPIO 12/13 (MODE1 bits [23:16]=0x22), GPIO 97/98 (MODE12 bits [11:4]=0x22) [VERIFIED]
 *   SPI4 Pad Select  : Pad 0 (/sys/firmware/devicetree/base/spi4@11018000/mediatek,pad-select = 0) [VERIFIED]
 *
 * Activated automatically at boot by hakios_input_init() via hakios_tcm_init().
 * Enabled via boot-arg: touchscreen=1.
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

#include "hakios.ft8726_fw.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

/* --- Hardware Physical Addresses --- */
#define MT6833_SPI4_PHYS        0x11018000ULL
#define MT6833_SPI4_SIZE        0x1000
#define MT6833_GPIO_PHYS        0x10005000ULL
#define MT6833_GPIO_SIZE        0x1000
#define MT6833_TOPCKGEN_PHYS    0x10000000ULL
#define MT6833_TOPCKGEN_SIZE    0x1000
#define MT6833_INFRACFG_PHYS    0x10001000ULL
#define MT6833_INFRACFG_SIZE    0x1000
#define MT6833_RAMOOPS_PHYS     0x480ef000ULL
#define MT6833_RAMOOPS_SIZE     0x40000u
#define MT6833_RAMOOPS_SIG      0x43474244u

struct touch_ramoops_hdr {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
};

/* --- MediaTek SPI Controller Registers (spi-mt65xx.ko / mt6765-spi) --- */
#define SPI_CFG0_REG            0x0000          /* CS hold [15:0] & CS setup [31:16] */
#define SPI_CFG1_REG            0x0004          /* CS idle [7:0], packet loop [15:8], packet len [25:16] */
#define SPI_TX_SRC_REG          0x0008
#define SPI_RX_DST_REG          0x000c
#define SPI_TX_DATA_REG         0x0010          /* TX FIFO data register (32-bit words, 32B depth) */
#define SPI_RX_DATA_REG         0x0014          /* RX FIFO data register (32-bit words, 32B depth) */
#define SPI_CMD_REG             0x0018          /* Transfer command & mode */
#define SPI_STATUS0_REG         0x001c          /* Transfer done / pause status (clear-on-read) */
#define SPI_STATUS1_REG         0x0020          /* Busy / completion status */
#define SPI_PAD_SEL_REG         0x0024          /* Pad selection (0 on LH8n) */
#define SPI_CFG2_REG            0x0028          /* SCK high [15:0] & SCK low [31:16] */

#define SPI_CMD_ACT             (1u << 0)
#define SPI_CMD_RESUME          (1u << 1)
#define SPI_CMD_RST             (1u << 2)
#define SPI_CMD_PAUSE_EN        (1u << 4)
#define SPI_CMD_DEASSERT        (1u << 5)
#define SPI_CMD_CPHA            (1u << 8)
#define SPI_CMD_CPOL            (1u << 9)
#define SPI_CMD_TXMSBF          (1u << 12)
#define SPI_CMD_RXMSBF          (1u << 13)

#define SPI_FIFO_BYTES          32
#define SPI_TICKS               3               /* 26 MHz / (2 * 3) = 4.33 MHz (< 6.0 MHz FT8726 max) */

/* --- MediaTek GPIO Controller Registers --- */
#define GPIO_DIR0               0x0000
#define GPIO_DOUT0              0x0100          /* Data out [31:0] */
#define GPIO_DIN0               0x0200          /* Data in [31:0] */
#define GPIO_MODE1              0x0310          /* Function mode for GPIO 8..15 */
#define GPIO_MODE2              0x0320          /* Function mode for GPIO 16..23 */
#define GPIO_MODE12             0x03c0          /* Function mode for GPIO 96..103 */

#define PIN_TOUCH_IRQ           14              /* GPIO 14: Active-low EINT line for LH8n */
#define PIN_TOUCH_RST           19              /* GPIO 19: Touch reset line for LH8n */

/* --- Panel & Multi-Touch Geometry --- */
#define PANEL_WIDTH             1080
#define PANEL_HEIGHT            2460            /* LH8n 1080x2460 FHD+ panel */
#define MAX_TOUCH_CONTACTS      10

/* --- FocalTech FT8726 SPI v2 Protocol Definitions (focaltech_noflash_common.ko) --- */
#define FTS_SPI_WRITE_CMD       0x00
#define FTS_SPI_READ_CMD        0xa0            /* READ_CMD (0x80) | DATA_CRC_EN (0x20) */
#define FTS_SPI_HEADER_LEN      7               /* 4 header bytes + 3 dummy bytes */
#define FTS_SPI_CRC_LEN         2
#define FTS_SPI_MAX_DATA        32752
#define FTS_SPI_BUF_SIZE        32768

#define FTS_CMD_START1          0x55
#define FTS_CMD_START2          0xaa
#define FTS_CMD_READ_BOOT_ID    0x90
#define FTS_CMD_SET_PRAM_ADDR   0xad
#define FTS_CMD_WRITE_PRAM      0xae
#define FTS_CMD_START_APP       0x08
#define FTS_CMD_ECC_START       0xcc
#define FTS_CMD_ECC_STATUS      0xce
#define FTS_CMD_ECC_READ        0xcd
#define FTS_ECC_OK_VAL          0xa5            /* upgrade_setting_list[FT8726].ecc_ok_val (+0x0c) */

#define FTS_REG_TOUCH_DATA      0x01
#define FTS_REG_CHIP_ID         0xa3
#define FTS_TOUCH_REPORT_LEN    84              /* 0x54 bytes (fts_ts_probe_entry +0x47c) */
#define FTS_POINT_REPORT_LEN    6

#define FTS_EVENT_DOWN          0x00
#define FTS_EVENT_UP            0x01
#define FTS_EVENT_MOVE          0x02

struct touch_contact {
	bool down;
	bool seen;
	uint32_t x;
	uint32_t y;
	uint32_t raw_x;
	uint32_t raw_y;
};

static vm_offset_t spi4_regs;
static vm_offset_t gpio_regs;
static vm_offset_t topckgen_regs;
static vm_offset_t infracfg_regs;
static vm_offset_t ramoops_regs;

static struct touch_contact contacts[MAX_TOUCH_CONTACTS];
static uint8_t fts_tx_buf[FTS_SPI_BUF_SIZE];
static uint8_t fts_rx_buf[FTS_SPI_BUF_SIZE];
static uint32_t flip_mode = 0;                  /* 0: none, 1: flip X, 2: flip Y, 3: both */
static uint32_t total_reports = 0;
static uint32_t total_touches = 0;
static uint32_t fts_dbg_pkt_offset = 0;
static uint32_t fts_status_fail_logs = 0;
static uint32_t spi_xfer_diag_logs = 0;
static uint32_t dpram_diag_logs = 0;
static bool ft8726_upload_gave_up = false;

static inline uint32_t spi_rd(uint32_t off) {
	return *(volatile uint32_t *)(spi4_regs + off);
}

static inline void spi_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(spi4_regs + off) = val;
}

static inline uint32_t gpio_rd(uint32_t off) {
	return *(volatile uint32_t *)(gpio_regs + off);
}

static inline void gpio_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(gpio_regs + off) = val;
}

static inline uint32_t topckgen_rd(uint32_t off) {
	return *(volatile uint32_t *)(topckgen_regs + off);
}

static inline void topckgen_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(topckgen_regs + off) = val;
}

static inline uint32_t infracfg_rd(uint32_t off) {
	return *(volatile uint32_t *)(infracfg_regs + off);
}

static inline void infracfg_wr(uint32_t off, uint32_t val) {
	*(volatile uint32_t *)(infracfg_regs + off) = val;
}

static inline void
ramoops_live_putc(char c)
{
	if (!ramoops_regs) {
		return;
	}
	volatile struct touch_ramoops_hdr *hdr = (volatile struct touch_ramoops_hdr *)ramoops_regs;
	volatile char *data = (volatile char *)(ramoops_regs + sizeof(struct touch_ramoops_hdr));
	uint32_t cap = MT6833_RAMOOPS_SIZE - (uint32_t)sizeof(struct touch_ramoops_hdr);
	uint32_t at = hdr->start;
	uint32_t size = hdr->size;

	if (hdr->sig != MT6833_RAMOOPS_SIG || at >= cap || size > cap) {
		hdr->sig = MT6833_RAMOOPS_SIG;
		hdr->start = 0;
		hdr->size = 0;
		at = 0;
		size = 0;
	}
	data[at] = c;
	if (++at == cap) {
		at = 0;
	}
	if (size < cap) {
		size++;
	}
	hdr->start = at;
	hdr->size = size;
}

static void
log_touch(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	for (const char *p = buf; *p; p++) {
		loader_log_putc(*p);
		ramoops_live_putc(*p);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	printf("%s", buf);
}

/* --- Coordinate Normalization: 1080x2460 -> 0..65535 --- */
static inline uint32_t
calibrate_x(uint32_t raw_x)
{
	if (raw_x >= PANEL_WIDTH) {
		raw_x = PANEL_WIDTH - 1;
	}
	if (flip_mode & 1) {
		raw_x = (PANEL_WIDTH - 1) - raw_x;
	}
	return (uint32_t)(((uint64_t)raw_x * 65535ULL) / (PANEL_WIDTH - 1));
}

static inline uint32_t
calibrate_y(uint32_t raw_y)
{
	if (raw_y >= PANEL_HEIGHT) {
		raw_y = PANEL_HEIGHT - 1;
	}
	if (flip_mode & 2) {
		raw_y = (PANEL_HEIGHT - 1) - raw_y;
	}
	return (uint32_t)(((uint64_t)raw_y * 65535ULL) / (PANEL_HEIGHT - 1));
}

/*
 * Layer 3: MediaTek SPI4 Hardware FIFO Transfer (spi-mt65xx.ko mtk_spi_fifo_transfer + mtk_spi_interrupt).
 *
 * Holds CS low across arbitrary-length transfers using SPI_CMD_PAUSE_EN (bit 4) and 32-byte FIFO bursts
 * (SPI_CMD_ACT for first burst, SPI_CMD_RESUME when paused), polling SPI_STATUS0_REG (+0x1c) bits [1:0].
 */
static bool
spi4_transfer_bytes(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (!spi4_regs || len == 0) {
		return false;
	}

	/* Reset controller and configure Mode 0 (CPOL=0, CPHA=0), Pad 0, 4.33 MHz clock */
	spi_wr(SPI_CMD_REG, SPI_CMD_RST);
	spi_wr(SPI_CMD_REG, 0);
	spi_wr(SPI_CFG2_REG, ((SPI_TICKS - 1) << 0) | ((SPI_TICKS - 1) << 16));
	spi_wr(SPI_CFG0_REG, ((SPI_TICKS - 1) << 0) | ((SPI_TICKS - 1) << 16));
	spi_wr(SPI_PAD_SEL_REG, 0);
	(void)spi_rd(SPI_STATUS0_REG);

	/* Assert CS in pause mode (mtk_spi_set_cs: SPI_CMD |= SPI_CMD_PAUSE_EN), MSB-first, no IRQs */
	uint32_t cmd_base = SPI_CMD_TXMSBF | SPI_CMD_RXMSBF | SPI_CMD_PAUSE_EN;
	spi_wr(SPI_CMD_REG, cmd_base);
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t offset = 0;
	bool paused = false;

	while (offset < len) {
		uint32_t chunk = len - offset;
		if (chunk > SPI_FIFO_BYTES) {
			chunk = SPI_FIFO_BYTES;
		}

		/* mtk_spi_setup_packet: PACKET_LEN = chunk - 1, PACKET_LOOP = 0 */
		spi_wr(SPI_CFG1_REG, ((chunk - 1) << 16) | (0 << 8) | ((SPI_TICKS - 1) << 0));

		/* Pack 32-bit little-endian words into SPI_TX_DATA_REG */
		uint32_t words = (chunk + 3) / 4;
		for (uint32_t w = 0; w < words; w++) {
			uint32_t v = 0;
			for (uint32_t b = 0; b < 4; b++) {
				uint32_t idx = offset + w * 4 + b;
				uint8_t byte_val = (tx && idx < len) ? tx[idx] : 0x00;
				v |= ((uint32_t)byte_val << (8 * b));
			}
			spi_wr(SPI_TX_DATA_REG, v);
		}
		__asm__ volatile("dsb sy" ::: "memory");

		/* Trigger burst: ACT (bit 0) on first chunk, RESUME (bit 1) when paused */
		uint32_t trig = paused ? SPI_CMD_RESUME : SPI_CMD_ACT;
		spi_wr(SPI_CMD_REG, spi_rd(SPI_CMD_REG) | trig);
		__asm__ volatile("dsb sy" ::: "memory");

		/*
		 * Completion: accept STATUS1 (+0x20) bit 0 (IDLE) or STATUS0
		 * (+0x1c, clear-on-read) bits [1:0]. The MARK120 log proved that
		 * STATUS0 DONE alone can stay 0 on this controller even when the
		 * transfer completes -- requiring it made every transfer time out
		 * ("st0=0" for the 0x55 boot command that used to work).
		 * PAUSE comes from STATUS0 bit 1.
		 */
		bool done = false;
		uint32_t st0_seen = 0, st1_seen = 0;
		for (uint32_t spin = 0; spin < 500000; spin++) {
			uint32_t st1 = spi_rd(SPI_STATUS1_REG);
			uint32_t st0 = spi_rd(SPI_STATUS0_REG);
			st1_seen = st1;
			st0_seen = st0;
			if ((st1 & 1) != 0 || (st0 & 0x3) != 0) {
				paused = ((st0 >> 1) & 1) != 0;
				done = true;
				break;
			}
		}

		if (!done) {
			if (spi_xfer_diag_logs < 12) {
				spi_xfer_diag_logs++;
				log_touch("mt6833_touch: SPI xfer timeout len=%u off=%u cmd=%#x st0=%#x st1=%#x\n",
				          len, offset, spi_rd(SPI_CMD_REG), st0_seen, st1_seen);
			}
			spi_wr(SPI_CMD_REG, spi_rd(SPI_CMD_REG) & ~SPI_CMD_PAUSE_EN);
			spi_wr(SPI_CMD_REG, spi_rd(SPI_CMD_REG) | SPI_CMD_RST);
			spi_wr(SPI_CMD_REG, 0);
			return false;
		}

		/* Unpack 32-bit little-endian words from SPI_RX_DATA_REG */
		for (uint32_t w = 0; w < words; w++) {
			uint32_t v = spi_rd(SPI_RX_DATA_REG);
			for (uint32_t b = 0; b < 4; b++) {
				uint32_t idx = offset + w * 4 + b;
				if (rx && idx < len) {
					rx[idx] = (uint8_t)((v >> (8 * b)) & 0xff);
				}
			}
		}

		offset += chunk;
	}

	/* Deassert CS and reset FIFO (mtk_spi_set_cs + mtk_spi_reset) */
	spi_wr(SPI_CMD_REG, spi_rd(SPI_CMD_REG) & ~SPI_CMD_PAUSE_EN);
	spi_wr(SPI_CMD_REG, spi_rd(SPI_CMD_REG) | SPI_CMD_RST);
	spi_wr(SPI_CMD_REG, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	return true;
}

/* --- FocalTech SPI v2 CRC-16/Kermit (focaltech_noflash_common.ko fts_read @ 0x10b4c0) --- */
static uint16_t
fts_crc16(const uint8_t *data, uint32_t len)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 1) {
				crc = (uint16_t)((crc >> 1) ^ 0x8408);
			} else {
				crc = (uint16_t)(crc >> 1);
			}
		}
	}
	return crc;
}

/* --- FocalTech SPI v2 Framed Write (focaltech_noflash_common.ko fts_write @ 0x10ad88) --- */
static bool
fts_write(uint8_t addr, const uint8_t *data, uint32_t datalen)
{
	uint32_t xfer_len = (datalen == 0) ? 4 : (FTS_SPI_HEADER_LEN + datalen);
	if (xfer_len > sizeof(fts_tx_buf)) {
		return false;
	}

	bzero(fts_tx_buf, xfer_len);
	fts_tx_buf[0] = addr;
	fts_tx_buf[1] = FTS_SPI_WRITE_CMD;
	fts_tx_buf[2] = (uint8_t)(datalen >> 8);
	fts_tx_buf[3] = (uint8_t)(datalen & 0xff);
	if (data && datalen > 0) {
		memcpy(&fts_tx_buf[FTS_SPI_HEADER_LEN], data, datalen);
	}

	for (int retry = 0; retry < 3; retry++) {
		bzero(fts_rx_buf, xfer_len);
		if (spi4_transfer_bytes(fts_tx_buf, fts_rx_buf, xfer_len) &&
		    (fts_rx_buf[3] & 0xa0) == 0) {
			delay(150);
			return true;
		}
		if (fts_status_fail_logs < 40) {
			fts_status_fail_logs++;
			log_touch("mt6833_touch: spi write fail cmd=%#x off=%#x try=%d rx=%02x %02x %02x %02x %02x %02x %02x %02x\n",
			          addr, fts_dbg_pkt_offset, retry,
			          fts_rx_buf[0], fts_rx_buf[1], fts_rx_buf[2], fts_rx_buf[3],
			          fts_rx_buf[4], fts_rx_buf[5], fts_rx_buf[6], fts_rx_buf[7]);
		}
		delay(150);
	}
	return false;
}

/* --- FocalTech SPI v2 Framed Read (focaltech_noflash_common.ko fts_read @ 0x10b2cc) --- */
static bool
fts_read(uint8_t addr, uint8_t *data, uint32_t datalen)
{
	uint32_t xfer_len = FTS_SPI_HEADER_LEN + datalen + FTS_SPI_CRC_LEN;
	if (datalen == 0 || xfer_len > sizeof(fts_tx_buf)) {
		return false;
	}

	for (int retry = 0; retry < 3; retry++) {
		bzero(fts_tx_buf, xfer_len);
		bzero(fts_rx_buf, xfer_len);
		fts_tx_buf[0] = addr;
		fts_tx_buf[1] = FTS_SPI_READ_CMD;
		fts_tx_buf[2] = (uint8_t)(datalen >> 8);
		fts_tx_buf[3] = (uint8_t)(datalen & 0xff);

		if (spi4_transfer_bytes(fts_tx_buf, fts_rx_buf, xfer_len) &&
		    (fts_rx_buf[3] & 0xa0) == 0) {
			const uint8_t *payload = &fts_rx_buf[FTS_SPI_HEADER_LEN];
			uint16_t calc_crc = fts_crc16(payload, datalen);
			uint16_t rx_crc = (uint16_t)payload[datalen] |
			                  ((uint16_t)payload[datalen + 1] << 8);
			if (calc_crc == rx_crc) {
				memcpy(data, payload, datalen);
				delay(150);
				return true;
			}
		}
		if (fts_status_fail_logs < 40) {
			fts_status_fail_logs++;
			log_touch("mt6833_touch: spi read fail cmd=%#x off=%#x try=%d rx=%02x %02x %02x %02x %02x %02x %02x %02x\n",
			          addr, fts_dbg_pkt_offset, retry,
			          fts_rx_buf[0], fts_rx_buf[1], fts_rx_buf[2], fts_rx_buf[3],
			          fts_rx_buf[4], fts_rx_buf[5], fts_rx_buf[6], fts_rx_buf[7]);
		}
		delay(150);
	}
	return false;
}

/* --- Layer 2/3.5 Hardware Reset Pulse (focaltech_noflash_common.ko fts_reset_proc) --- */
static void
fts_hw_reset_pulse(uint32_t recovery_ms)
{
	uint32_t dout0 = gpio_rd(GPIO_DOUT0);
	dout0 &= ~(1u << PIN_TOUCH_RST);
	gpio_wr(GPIO_DOUT0, dout0);
	delay(2000);                            /* 2 ms low pulse */

	dout0 = gpio_rd(GPIO_DOUT0);
	dout0 |= (1u << PIN_TOUCH_RST);
	gpio_wr(GPIO_DOUT0, dout0);
	delay(recovery_ms * 1000);
}

/*
 * Enter FT8726 Boot ROM and verify Boot ID 0x87 0xA5 / 0x87 0x25
 * (focaltech_noflash_common.ko fts_enter_into_boot @ 0x108d58 & fts_ts_probe_entry @ 0x1017cc).
 */
static bool
fts_enter_boot_rom(void)
{
	uint8_t boot_id[2] = { 0, 0 };
	uint8_t cmd_aa = FTS_CMD_START2;

	for (uint32_t attempt = 0; attempt < 10; attempt++) {
		/* Reset with 8 ms + attempt ms recovery (upgrade_setting_list[FT8726].boot_delay = 8) */
		fts_hw_reset_pulse(8 + attempt);

		/* Try both {0x55} (fts_enter_into_boot) and {0x55, 0xAA} (fts_read_bootid) */
		for (int sub = 0; sub < 3; sub++) {
			if (sub == 0) {
				(void)fts_write(FTS_CMD_START1, &cmd_aa, 1);
				delay(12000);
			} else {
				(void)fts_write(FTS_CMD_START1, NULL, 0);
				delay(8000);
			}

			boot_id[0] = 0;
			boot_id[1] = 0;
			if (fts_read(FTS_CMD_READ_BOOT_ID, boot_id, 2)) {
				if (boot_id[0] == 0x87 &&
				    (boot_id[1] == 0xa5 || boot_id[1] == 0x25 ||
				     boot_id[1] == 0xa0 || boot_id[1] == 0x20)) {
					log_touch("mt6833_touch: FT8726 Boot ROM active (boot_id=%02x %02x, attempt=%u.%d)\n",
					          boot_id[0], boot_id[1], attempt, sub);
					return true;
				}
			}
		}
		log_touch("mt6833_touch: boot_id probe attempt %u got %02x %02x (rx3=%02x)\n",
		          attempt, boot_id[0], boot_id[1], fts_rx_buf[3]);
	}
	return false;
}

/*
 * Write PRAM (base=0x000000) or DRAM (base=0xD00000) via 0xAD + 0xAE
 * (focaltech_noflash_common.ko fts_dpram_write @ 0x1095d0).
 */
static bool
fts_dpram_write_region(uint32_t base_addr, const uint8_t *buf, uint32_t len, uint32_t pkt_size)
{
	uint8_t addr_bytes[3];

	for (uint32_t offset = 0; offset < len; offset += pkt_size) {
		uint32_t chunk = len - offset;
		if (chunk > pkt_size) {
			chunk = pkt_size;
		}
		uint32_t target_addr = base_addr + offset;
		fts_dbg_pkt_offset = target_addr;
		addr_bytes[0] = (uint8_t)((target_addr >> 16) & 0xff);
		addr_bytes[1] = (uint8_t)((target_addr >> 8) & 0xff);
		addr_bytes[2] = (uint8_t)(target_addr & 0xff);

		if (!fts_write(FTS_CMD_SET_PRAM_ADDR, addr_bytes, 3)) {
			log_touch("mt6833_touch: 0xAD addr write failed at %#x\n", target_addr);
			if (dpram_diag_logs < 8) {
				dpram_diag_logs++;
				log_touch("mt6833_touch: 0xAD diag addr=%02x %02x %02x rx=%02x %02x %02x %02x %02x %02x %02x %02x st1=%#x\n",
				          addr_bytes[0], addr_bytes[1], addr_bytes[2],
				          fts_rx_buf[0], fts_rx_buf[1], fts_rx_buf[2], fts_rx_buf[3],
				          fts_rx_buf[4], fts_rx_buf[5], fts_rx_buf[6], fts_rx_buf[7],
				          spi_rd(SPI_STATUS1_REG));
			}
			fts_dbg_pkt_offset = 0;
			return false;
		}
		if (!fts_write(FTS_CMD_WRITE_PRAM, buf + offset, chunk)) {
			log_touch("mt6833_touch: 0xAE data write failed at %#x (chunk=%u)\n",
			          target_addr, chunk);
			if (dpram_diag_logs < 8) {
				dpram_diag_logs++;
				log_touch("mt6833_touch: 0xAE diag rx=%02x %02x %02x %02x %02x %02x %02x %02x st1=%#x\n",
				          fts_rx_buf[0], fts_rx_buf[1], fts_rx_buf[2], fts_rx_buf[3],
				          fts_rx_buf[4], fts_rx_buf[5], fts_rx_buf[6], fts_rx_buf[7],
				          spi_rd(SPI_STATUS1_REG));
			}
			fts_dbg_pkt_offset = 0;
			return false;
		}
	}
	fts_dbg_pkt_offset = 0;
	return true;
}

/*
 * Host & TP ECC-16 verification (focaltech_noflash_common.ko fts_ecc_check @ 0x109840).
 */
static bool
fts_ecc_check_region(const uint8_t *buf, uint32_t len, const char *region_name)
{
	uint32_t host_ecc = 0;

	for (uint32_t i = 0; i + 1 < len; i += 2) {
		uint32_t word = ((uint32_t)buf[i] << 8) | (uint32_t)buf[i + 1];
		host_ecc ^= word;
		for (int bit = 0; bit < 16; bit++) {
			if (host_ecc & 1) {
				host_ecc = ((host_ecc >> 1) & 0x7fff) ^ 0x8408;
			} else {
				host_ecc = (host_ecc >> 1) & 0x7fff;
			}
		}
	}
	host_ecc &= 0xffff;

	uint8_t ecc_cmd[6] = {
		0x00, 0x00, 0x00,
		(uint8_t)((len >> 16) & 0xff),
		(uint8_t)((len >> 8) & 0xff),
		(uint8_t)(len & 0xff)
	};
	if (!fts_write(FTS_CMD_ECC_START, ecc_cmd, 6)) {
		log_touch("mt6833_touch: %s ECC start command (0xCC) failed\n", region_name);
		return false;
	}
	delay(2000);

	uint8_t status = 0;
	bool ready = false;
	for (int poll = 0; poll < 100; poll++) {
		if (fts_read(FTS_CMD_ECC_STATUS, &status, 1) && status == FTS_ECC_OK_VAL) {
			ready = true;
			break;
		}
		delay(1000);
	}
	if (!ready) {
		log_touch("mt6833_touch: %s ECC status timeout (last 0xCE=%02x, expected %02x)\n",
		          region_name, status, FTS_ECC_OK_VAL);
		return false;
	}

	uint8_t tp_ecc_bytes[2] = { 0, 0 };
	if (!fts_read(FTS_CMD_ECC_READ, tp_ecc_bytes, 2)) {
		log_touch("mt6833_touch: %s ECC read (0xCD) failed\n", region_name);
		return false;
	}
	uint32_t tp_ecc = ((uint32_t)tp_ecc_bytes[0] << 8) | (uint32_t)tp_ecc_bytes[1];
	log_touch("mt6833_touch: %s ECC check: tp=%04x host=%04x\n", region_name, tp_ecc, host_ecc);
	return (tp_ecc == host_ecc);
}

/*
 * Layer 3.5: Full FT8726 Firmware Upload (PRAM + DRAM + ECC + Start App)
 * (focaltech_noflash_common.ko fts_fw_write_start @ 0x108c40).
 */
static uint64_t
elapsed_ms_since(uint64_t t0)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns / 1000000ULL;
}

static bool
ft8726_upload_firmware(void)
{
	if (ft8726_upload_gave_up) {
		return false;
	}

	static const uint32_t pkt_sizes[3] = { 32752, 128, 24 };

	for (uint32_t attempt = 0; attempt < 6; attempt++) {
		uint32_t pkt_size = pkt_sizes[attempt % 3];
		uint64_t t0 = mach_absolute_time();

		if (!fts_enter_boot_rom()) {
			log_touch("mt6833_touch: failed to enter FT8726 Boot ROM\n");
			log_touch("mt6833_touch: attempt %u/6 pkt=%u result=boot_rom_fail elapsed=%llu ms\n",
			          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
			continue;
		}

		log_touch("mt6833_touch: uploading PRAM (%u bytes @ 0x000000, pkt=%u)...\n",
		          FT8726_FW_PRAM_LEN, pkt_size);
		if (!fts_dpram_write_region(0x000000, ft8726_fw_bin, FT8726_FW_PRAM_LEN, pkt_size)) {
			log_touch("mt6833_touch: attempt %u/6 pkt=%u result=pram_write_fail elapsed=%llu ms\n",
			          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
			continue;
		}
		if (!fts_ecc_check_region(ft8726_fw_bin, FT8726_FW_PRAM_LEN, "PRAM")) {
			log_touch("mt6833_touch: attempt %u/6 pkt=%u result=pram_ecc_fail elapsed=%llu ms\n",
			          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
			continue;
		}

		log_touch("mt6833_touch: uploading DRAM (%u bytes @ 0xD00000, pkt=%u)...\n",
		          FT8726_FW_DRAM_LEN, pkt_size);
		const uint8_t *dram_ptr = ft8726_fw_bin + FT8726_FW_PRAM_LEN;
		if (!fts_dpram_write_region(0xd00000, dram_ptr, FT8726_FW_DRAM_LEN, pkt_size)) {
			log_touch("mt6833_touch: attempt %u/6 pkt=%u result=dram_write_fail elapsed=%llu ms\n",
			          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
			continue;
		}
		if (!fts_ecc_check_region(dram_ptr, FT8726_FW_DRAM_LEN, "DRAM")) {
			log_touch("mt6833_touch: attempt %u/6 pkt=%u result=dram_ecc_fail elapsed=%llu ms\n",
			          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
			continue;
		}

		/* Start application firmware in PRAM (0x08) */
		if (!fts_write(FTS_CMD_START_APP, NULL, 0)) {
			log_touch("mt6833_touch: start app command (0x08) failed\n");
			log_touch("mt6833_touch: attempt %u/6 pkt=%u result=start_app_fail elapsed=%llu ms\n",
			          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
			continue;
		}
		delay(50000);                   /* 50 ms app init delay (fts_fwupg_work) */

		/* Poll register 0xA3 (CHIP_ID) for 0x87 (fts_wait_tp_to_valid @ 0x1004b0) */
		uint8_t chip_id = 0;
		for (int wait = 0; wait < 10; wait++) {
			if (fts_read(FTS_REG_CHIP_ID, &chip_id, 1) && chip_id == 0x87) {
				log_touch("mt6833_touch: FT8726 firmware running! (0xA3=%02x, fw_ver=%02x)\n",
				          chip_id, ft8726_fw_bin[0x10e]);
				log_touch("mt6833_touch: attempt %u/6 pkt=%u result=ok elapsed=%llu ms\n",
				          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
				return true;
			}
			delay(50000);
		}
		log_touch("mt6833_touch: app start wait timeout (last 0xA3=%02x)\n", chip_id);
		log_touch("mt6833_touch: attempt %u/6 pkt=%u result=app_timeout elapsed=%llu ms\n",
		          attempt + 1, pkt_size, (unsigned long long)elapsed_ms_since(t0));
	}
	ft8726_upload_gave_up = true;
	log_touch("mt6833_touch: giving up after 6 attempts\n");
	return false;
}

/* --- Process FocalTech Multi-Finger Packet (focaltech_noflash_common.ko fts_irq_handler @ 0x101cf0) --- */
static void
process_touch_report(const uint8_t *pkt, uint32_t len)
{
	if (len < 2) {
		return;
	}

	/* Check for firmware crash/reset signature 0xEF 0xEF 0xEF (fts_irq_handler @ 0x101de4) */
	if (pkt[1] == 0xef && pkt[2] == 0xef && pkt[3] == 0xef) {
		if (!ft8726_upload_gave_up) {
			log_touch("mt6833_touch: firmware reset signature (0xEF) detected, recovering...\n");
			(void)ft8726_upload_firmware();
		}
		return;
	}

	if (pkt[1] == 0xff) {
		return;
	}

	uint32_t high_nibble = (pkt[1] >> 4) & 0x0f;
	uint32_t num_fingers = pkt[1] & 0x0f;
	if (num_fingers > MAX_TOUCH_CONTACTS) {
		return;
	}

	uint32_t start_off = 2;
	uint32_t stride = FTS_POINT_REPORT_LEN;
	uint32_t max_entries = MAX_TOUCH_CONTACTS;
	if (high_nibble == 8) {
		start_off = 4;
		stride = 6;
		max_entries = num_fingers;
	} else if (high_nibble == 2) {
		start_off = 4;
		stride = 8;
		max_entries = num_fingers;
	} else if (high_nibble != 0) {
		return;
	}

	if (num_fingers > 0 && total_touches < 4 && len >= 10) {
		log_touch("mt6833_touch: raw pkt[0..9]=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		          pkt[0], pkt[1], pkt[2], pkt[3], pkt[4],
		          pkt[5], pkt[6], pkt[7], pkt[8], pkt[9]);
	}

	for (uint32_t i = 0; i < MAX_TOUCH_CONTACTS; i++) {
		contacts[i].seen = false;
	}

	for (uint32_t i = 0; i < max_entries; i++) {
		uint32_t off = start_off + (i * stride);
		if (off + 4 > len) {
			break;
		}

		const uint8_t *p = &pkt[off];
		if (high_nibble == 0 && p[2] > 0x9f) {
			break;
		}

		uint32_t event = (p[0] >> 6) & 0x03;
		uint32_t finger = (p[2] >> 4) & 0x0f;
		uint32_t raw_x = ((uint32_t)(p[0] & 0x0f) << 8) | p[1];
		uint32_t raw_y = ((uint32_t)(p[2] & 0x0f) << 8) | p[3];

		if (finger >= MAX_TOUCH_CONTACTS) {
			continue;
		}
		if ((event == FTS_EVENT_DOWN || event == FTS_EVENT_MOVE) && num_fingers == 0) {
			break;
		}

		struct touch_contact *c = &contacts[finger];
		c->seen = true;

		if (event == FTS_EVENT_UP) {
			if (c->down) {
				c->down = false;
				hakios_touch_post(HAKIOS_TOUCH_UP, finger, c->x, c->y);
				if (total_touches <= 16) {
					log_touch("mt6833_touch: UP slot=%u norm=(%u,%u)\n",
					          finger, c->x, c->y);
				}
			}
			continue;
		}

		uint32_t norm_x = calibrate_x(raw_x);
		uint32_t norm_y = calibrate_y(raw_y);

		int dx = (int)raw_x - (int)c->raw_x;
		int dy = (int)raw_y - (int)c->raw_y;
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;

		if (!c->down) {
			c->down = true;
			c->x = norm_x;
			c->y = norm_y;
			c->raw_x = raw_x;
			c->raw_y = raw_y;
			hakios_touch_post(HAKIOS_TOUCH_DOWN, finger, norm_x, norm_y);
			total_touches++;
			if (total_touches <= 16) {
				log_touch("mt6833_touch: DOWN slot=%u raw=(%u,%u) norm=(%u,%u)\n",
				          finger, raw_x, raw_y, norm_x, norm_y);
			}
		} else if (dx >= 1 || dy >= 1) {
			c->x = norm_x;
			c->y = norm_y;
			c->raw_x = raw_x;
			c->raw_y = raw_y;
			hakios_touch_post(HAKIOS_TOUCH_MOVE, finger, norm_x, norm_y);
		}
	}

	/* Lift fingers that disappeared from report */
	for (uint32_t i = 0; i < MAX_TOUCH_CONTACTS; i++) {
		struct touch_contact *c = &contacts[i];
		if (c->down && !c->seen) {
			c->down = false;
			hakios_touch_post(HAKIOS_TOUCH_UP, i, c->x, c->y);
			if (total_touches <= 16) {
				log_touch("mt6833_touch: UP(gone) slot=%u norm=(%u,%u)\n",
				          i, c->x, c->y);
			}
		}
	}

	total_reports++;
}

/* --- Layer 4/5: Touch Event Dispatcher Thread --- */
static void
mt6833_touch_thread(__unused void *param, __unused wait_result_t res)
{
	uint8_t report_buf[FTS_TOUCH_REPORT_LEN];
	uint32_t poll_us = 8000;        /* ~125 Hz default poll rate */

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_flip", &flip_mode, sizeof(flip_mode));

	if (poll_us < 1000 || poll_us > 50000) {
		poll_us = 8000;
	}

	log_touch("mt6833_touch: Layer 4/5 touch thread started (poll=%u us, flip=%u)\n",
	          poll_us, flip_mode);

	/* Perform FT8726 PRAM/DRAM firmware upload inside kernel thread so boot isn't blocked */
	if (!ft8726_upload_firmware()) {
		log_touch("mt6833_touch: ERROR: initial FT8726 firmware upload failed\n");
	}

	/* Check boot-arg touch_test=1 for simulated gesture verification */
	uint32_t do_test = 0;
	if (PE_parse_boot_argn("touch_test", &do_test, sizeof(do_test)) && do_test != 0) {
		log_touch("mt6833_touch: running simulated swipe gesture test...\n");
		delay(1000000);
		hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, 32768, 40000);
		delay(20000);
		for (int y = 40000; y >= 20000; y -= 1000) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, 32768, y);
			delay(15000);
		}
		hakios_touch_post(HAKIOS_TOUCH_UP, 0, 32768, 20000);
		log_touch("mt6833_touch: simulated swipe gesture posted to /dev/hakios-touch\n");
	}

	/*
	 * Poll FTS_REG_TOUCH_DATA (0x01) directly every poll_us (~125 Hz).
	 * FT8726 uses IRQF_TRIGGER_FALLING (0x2002) pulses on GPIO 14, so unconditional SPI polling
	 * avoids missing short falling-edge pulses between sleeps.
	 */
	for (;;) {
		if (fts_read(FTS_REG_TOUCH_DATA, report_buf, FTS_TOUCH_REPORT_LEN)) {
			process_touch_report(report_buf, FTS_TOUCH_REPORT_LEN);
		} else if (fts_read(FTS_REG_TOUCH_DATA, report_buf, 20)) {
			process_touch_report(report_buf, 20);
		} else if (fts_rx_buf[FTS_SPI_HEADER_LEN] == 0xef) {
			log_touch("mt6833_touch: 0xEF on read failure, recovering firmware...\n");
			(void)ft8726_upload_firmware();
		}

		delay(poll_us);
	}
}

/*
 * Layer 2: Power rails, SPI4 clocks, SPI4 pinmux, & hardware reset sequence.
 */
static bool
mt6833_touch_hw_init(void)
{
	if (!gpio_regs || !infracfg_regs || !topckgen_regs) {
		return false;
	}

	/* 1a. Enable TOPCKGEN spi_sel at 26 MHz (parent 0) & clear PDN_SPI (bit 23):
	 *     clk-mt6833.ko spi_sel: mux_ofs=0x0070, set_ofs=0x0074, clr_ofs=0x0078,
	 *     mux_shift=16, mux_width=3, gate_shift=23, upd_ofs=0x0004, upd_shift=26 */
	topckgen_wr(0x0078, (7u << 16) | (1u << 23));
	topckgen_wr(0x0004, (1u << 26));
	__asm__ volatile("dsb sy" ::: "memory");

	/* 1b. Un-gate INFRACFG_AO ifrao_spi4 (bit 25):
	 *     clk-mt6833-bus.ko ifrao_spi4: set_ofs=0x00a4, clr_ofs=0x00a8, sta_ofs=0x00ac */
	infracfg_wr(0x00a8, (1u << 25));
	__asm__ volatile("dsb sy" ::: "memory");

	/* 2. Configure SPI4 pins (GPIO 12, 13 in MODE1; GPIO 97, 98 in MODE12) to Mode 2
	 *    and IRQ pin (GPIO 14 in MODE1) to Mode 0 (GPIO input) */
	uint32_t mode1 = gpio_rd(GPIO_MODE1);
	mode1 &= ~((0xFu << 16) | (0xFu << 20) | (0xFu << 24));
	mode1 |= ((0x2u << 16) | (0x2u << 20)); /* GPIO 12=Mode2, GPIO 13=Mode2, GPIO 14=Mode0 */
	gpio_wr(GPIO_MODE1, mode1);

	uint32_t mode12 = gpio_rd(GPIO_MODE12);
	mode12 &= ~((0xFu << 4) | (0xFu << 8));
	mode12 |= ((0x2u << 4) | (0x2u << 8));  /* GPIO 97=Mode2, GPIO 98=Mode2 */
	gpio_wr(GPIO_MODE12, mode12);

	/* 3. Configure GPIO 19 (RST) as Mode 0 output, GPIO 14 (IRQ) as input */
	uint32_t mode2 = gpio_rd(GPIO_MODE2);
	mode2 &= ~(0xFu << 12);                 /* GPIO 19: bits [15:12] = 0 (GPIO mode) */
	gpio_wr(GPIO_MODE2, mode2);

	uint32_t dir0 = gpio_rd(GPIO_DIR0);
	dir0 |= (1u << PIN_TOUCH_RST);          /* GPIO 19 = output (1) */
	dir0 &= ~(1u << PIN_TOUCH_IRQ);         /* GPIO 14 = input (0) */
	gpio_wr(GPIO_DIR0, dir0);
	__asm__ volatile("dsb sy" ::: "memory");

	/* 4. Assert initial 2 ms reset pulse */
	fts_hw_reset_pulse(20);

	uint32_t din_post = gpio_rd(GPIO_DIN0);
	uint32_t rst_state = (din_post >> PIN_TOUCH_RST) & 1;
	uint32_t irq_state = (din_post >> PIN_TOUCH_IRQ) & 1;

	log_touch("mt6833_touch: post-reset: rst=%u irq=%u CG2_STA=%#x CLK_CFG_6=%#x\n",
	          rst_state, irq_state, infracfg_rd(0x00ac), topckgen_rd(0x0070));
	return (rst_state == 1);
}

/* --- Public Driver Interface for sys/hakios.input.h --- */

void
hakios_tcm_init(void)
{
	ramoops_regs = ml_io_map(MT6833_RAMOOPS_PHYS, MT6833_RAMOOPS_SIZE);

	uint32_t touch_arg = 0;
	boolean_t touch_present = PE_parse_boot_argn("touchscreen", &touch_arg, sizeof(touch_arg));

	if (!touch_present || touch_arg == 0) {
		log_touch("mt6833_touch: touch disabled (touchscreen boot-arg not set or 0)\n");
		return;
	}

	log_touch("mt6833_touch: LH8n FT8726 SPI4 driver probe initialized (touchscreen=%u)\n", touch_arg);

	spi4_regs = ml_io_map(MT6833_SPI4_PHYS, MT6833_SPI4_SIZE);
	gpio_regs = ml_io_map(MT6833_GPIO_PHYS, MT6833_GPIO_SIZE);
	topckgen_regs = ml_io_map(MT6833_TOPCKGEN_PHYS, MT6833_TOPCKGEN_SIZE);
	infracfg_regs = ml_io_map(MT6833_INFRACFG_PHYS, MT6833_INFRACFG_SIZE);

	if (!spi4_regs || !gpio_regs || !topckgen_regs || !infracfg_regs) {
		log_touch("mt6833_touch: failed to map MMIO regions\n");
		return;
	}

	uint32_t cg2_sta = infracfg_rd(0x00ac);
	uint32_t clk_cfg6 = topckgen_rd(0x0070);
	log_touch("mt6833_touch: HW snapshot: CG2_STA=%#x (spi4_cg=%u) CLK_CFG_6=%#x MODE1=%#x MODE2=%#x MODE12=%#x\n",
	          cg2_sta, (cg2_sta >> 25) & 1, clk_cfg6,
	          gpio_rd(GPIO_MODE1), gpio_rd(GPIO_MODE2), gpio_rd(GPIO_MODE12));

	bzero(contacts, sizeof(contacts));

	if (!mt6833_touch_hw_init()) {
		log_touch("mt6833_touch: Layer 2 hardware init failed\n");
		return;
	}

	thread_t th = THREAD_NULL;
	kern_return_t kr = kernel_thread_start(mt6833_touch_thread, NULL, &th);
	if (kr != KERN_SUCCESS) {
		log_touch("mt6833_touch: failed to start touch thread (kr=%d)\n", kr);
		return;
	}
	thread_deallocate(th);
}
