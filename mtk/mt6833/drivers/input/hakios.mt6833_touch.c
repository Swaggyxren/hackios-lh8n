/*
 * MediaTek MT6833 (Dimensity 6080 / Tecno Pova 5 Pro 5G - LH8n) Touchscreen Driver.
 *
 * Provides multi-touch tracking and coordinate calibration for Darwin's /dev/hakios-touch.
 *
 * Hardware Mapping & Citations (Tecno Pova 5 Pro 5G / LH8n):
 *   Touch Controller : FocalTech FT8726 (FT8725/FT8726 family) on SPI4
 *                      Source: focaltech_noflash_common.ko fts_ts_probe_entry (.rodata:0x84bc) [VERIFIED]
 *                      Secondary Supplier: Ilitek ILI7807S [VERIFIED]
 *   Physical Screen  : 1080 x 2460 FHD+ @ 120Hz (LK fb at 0x76950000, device.conf:11) [VERIFIED]
 *   SPI4 Controller  : 0x11018000 (size 0x1000) [VERIFIED]
 *   GPIO Controller  : 0x10005000 (size 0x1000) [VERIFIED]
 *   TOPCKGEN Base    : 0x10000000 (size 0x1000) [VERIFIED]
 *   INFRACFG_AO Base : 0x10001000 (size 0x1000) [VERIFIED]
 *   Reset GPIO       : GPIO 19 (PIN_TOUCH_RST 19) [VERIFIED]
 *   Reset Timing     : 2 ms low pulse, 200 ms high recovery
 *                      Source: focaltech_noflash_common.ko fts_reset_proc (.text:0x634-0x6a8) [VERIFIED]
 *   IRQ GPIO         : GPIO 14 (PIN_TOUCH_IRQ 14, active-low EINT) [VERIFIED]
 *   SPI4 Clock Gate  : INFRACFG_AO + 0x00a8 (MODULE_SW_CG_0) bit 25 (ifrao_spi4) [UNVERIFIED for LH8n]
 *   SPI4 Clock Mux   : TOPCKGEN + 0x0078 (CLK_CFG_7) bit 26 (spi_sel 26MHz) [UNVERIFIED for LH8n]
 *   SPI4 Pinmux Mode : GPIO 12/13/97/98 Mode 2 (SPI4_CLK, SPI4_CS, SPI4_MI, SPI4_MO) [UNVERIFIED for LH8n]
 *   Chip ID Read     : Register 0xA3 (2 bytes: 0x87, 0x25 / 0x87, 0x26) [VERIFIED]
 *                      Register 0x90 (2 boot ID bytes) [VERIFIED]
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

/* --- MediaTek SPI Controller Registers (mt6765-spi / mtkspi.h) --- */
#define SPI_CFG0_REG            0x0000          /* Clock divider & CS setup/hold */
#define SPI_CFG1_REG            0x0004          /* Packet & transfer length */
#define SPI_TX_SRC_REG          0x0008
#define SPI_RX_DST_REG          0x000c
#define SPI_TX_DATA_REG         0x0010          /* TX FIFO data register */
#define SPI_RX_DATA_REG         0x0014          /* RX FIFO data register */
#define SPI_CMD_REG             0x0018          /* Transfer command */
#define SPI_STATUS0_REG         0x001c          /* Status flags */
#define SPI_STATUS1_REG         0x0020
#define SPI_PAD_SEL_REG         0x0024          /* Pad selection */
#define SPI_CFG2_REG            0x0028          /* SCK high/low timing */

/* --- MediaTek GPIO Controller Registers --- */
#define GPIO_DIR0               0x0000
#define GPIO_DOUT0              0x0100          /* Data out [31:0] */
#define GPIO_DIN0               0x0200          /* Data in [31:0] */
#define GPIO_MODE1              0x0310          /* Function mode for GPIO 8..15 */
#define GPIO_MODE12             0x03c0          /* Function mode for GPIO 96..103 */

#define PIN_TOUCH_IRQ           14              /* GPIO 14: Active-low EINT line for LH8n */
#define PIN_TOUCH_RST           19              /* GPIO 19: Touch reset line for LH8n */

/* --- Panel & Multi-Touch Geometry --- */
#define PANEL_WIDTH             1080
#define PANEL_HEIGHT            2460            /* LH8n 1080x2460 FHD+ panel */
#define MAX_TOUCH_CONTACTS      10

/* --- FocalTech FTS Protocol Definitions --- */
#define FTS_REG_CHIP_ID         0xa3
#define FTS_REG_BOOT_ID         0x90
#define FTS_REG_FLOW_WORK_MODE  0x00
#define FTS_REG_POINTS          0x02            /* Touch point count & status */
#define FTS_POINT_REPORT_LEN    6               /* 6 bytes per contact */

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

static struct touch_contact contacts[MAX_TOUCH_CONTACTS];
static uint32_t flip_mode = 0;                  /* 0: none, 1: flip X, 2: flip Y, 3: both */
static uint32_t total_reports = 0;
static uint32_t total_touches = 0;

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
	}
	printf("%s", buf);
}

/* --- Coordinate Normalization: 1080x2460 -> 0..65535 --- */
static inline uint32_t __unused
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

static inline uint32_t __unused
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

/* --- SPI4 Hardware Transfer --- */
static bool __unused
spi4_transfer_bytes(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (!spi4_regs || len == 0 || len > 64) {
		return false;
	}

	/* Reset SPI controller state */
	spi_wr(SPI_CMD_REG, (1u << 2));          /* Controller soft reset */
	delay(10);
	spi_wr(SPI_CFG0_REG, 0x00000800);       /* Master mode, CPOL=0, CPHA=0, clock div */
	spi_wr(SPI_CFG1_REG, (len << 16) | len);/* Packet & transfer length */
	spi_wr(SPI_PAD_SEL_REG, 0x00000001);    /* Pad select 1 */

	/* Fill TX FIFO */
	for (uint32_t i = 0; i < len; i++) {
		spi_wr(SPI_TX_DATA_REG, tx ? tx[i] : 0x00);
	}

	/* Start transfer */
	spi_wr(SPI_CMD_REG, 1);

	/* Wait for completion (timeout 5ms) */
	for (int i = 0; i < 500; i++) {
		uint32_t stat = spi_rd(SPI_STATUS0_REG);
		if (stat & (1u << 0)) {         /* Transfer finish flag */
			/* Read RX FIFO */
			for (uint32_t j = 0; j < len; j++) {
				uint32_t val = spi_rd(SPI_RX_DATA_REG);
				if (rx) {
					rx[j] = (uint8_t)(val & 0xff);
				}
			}
			return true;
		}
		delay(10);
	}

	return false;
}

/* --- Process FocalTech Multi-Finger Packet --- */
static void __unused
process_touch_report(const uint8_t *pkt, uint32_t len)
{
	if (len < 2) return;

	uint32_t num_fingers = pkt[1] & 0x0f;
	if (num_fingers > MAX_TOUCH_CONTACTS) {
		num_fingers = MAX_TOUCH_CONTACTS;
	}

	for (uint32_t i = 0; i < MAX_TOUCH_CONTACTS; i++) {
		contacts[i].seen = false;
	}

	for (uint32_t i = 0; i < num_fingers; i++) {
		uint32_t off = 2 + (i * FTS_POINT_REPORT_LEN);
		if (off + FTS_POINT_REPORT_LEN > len) break;

		const uint8_t *p = &pkt[off];
		uint32_t event = (p[0] >> 6) & 0x03;
		uint32_t raw_x = ((uint32_t)(p[0] & 0x0f) << 8) | p[1];
		uint32_t finger = (p[2] >> 4) & 0x0f;
		uint32_t raw_y = ((uint32_t)(p[2] & 0x0f) << 8) | p[3];

		if (finger >= MAX_TOUCH_CONTACTS) continue;

		struct touch_contact *c = &contacts[finger];
		c->seen = true;

		if (event == FTS_EVENT_UP) {
			if (c->down) {
				c->down = false;
				hakios_touch_post(HAKIOS_TOUCH_UP, finger, c->x, c->y);
			}
			continue;
		}

		uint32_t norm_x = calibrate_x(raw_x);
		uint32_t norm_y = calibrate_y(raw_y);

		/* 2-pixel jitter suppression filter */
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
		} else if (dx >= 2 || dy >= 2) {
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
		}
	}

	total_reports++;
}

/* --- Touch Event Dispatcher Thread --- */
static void __unused
mt6833_touch_thread(__unused void *param, __unused wait_result_t res)
{
	uint8_t tx_cmd[3] = { 0x00, 0x00, 0x00 };
	uint8_t rx_buf[64];
	uint32_t poll_us = 8333;        /* 120 Hz default poll rate */

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("touch_flip", &flip_mode, sizeof(flip_mode));

	if (poll_us < 1000 || poll_us > 50000) {
		poll_us = 8333;
	}

	log_touch("mt6833_touch: touch monitor thread active (rate=%u us, flip=%u)\n",
	          poll_us, flip_mode);

	/* Check boot-arg touch_test=1 for simulated gesture verification */
	uint32_t do_test = 0;
	if (PE_parse_boot_argn("touch_test", &do_test, sizeof(do_test)) && do_test != 0) {
		log_touch("mt6833_touch: running simulated swipe gesture test...\n");
		delay(1000000); /* 1s pause */
		/* Center tap & drag: (32768, 40000) -> (32768, 20000) */
		hakios_touch_post(HAKIOS_TOUCH_DOWN, 0, 32768, 40000);
		delay(20000);
		for (int y = 40000; y >= 20000; y -= 1000) {
			hakios_touch_post(HAKIOS_TOUCH_MOVE, 0, 32768, y);
			delay(15000);
		}
		hakios_touch_post(HAKIOS_TOUCH_UP, 0, 32768, 20000);
		log_touch("mt6833_touch: simulated swipe gesture posted to /dev/hakios-touch\n");
	}

	for (;;) {
		bool pending = true;

		/* Check hardware IRQ line if GPIO registers are mapped */
		if (gpio_regs) {
			uint32_t din = gpio_rd(GPIO_DIN0);
			/* Pin 14 is active-low: 0 means touch report ready */
			pending = !(din & (1u << PIN_TOUCH_IRQ));
		}

		if (pending) {
			/* Read touch status and point registers via SPI4 */
			if (spi4_transfer_bytes(tx_cmd, rx_buf, 32)) {
				process_touch_report(rx_buf, 32);
			}
		}

		delay(poll_us);
	}
}

/* --- Public Driver Interface for sys/hakios.input.h --- */

void
hakios_tcm_init(void)
{
	uint32_t touch_arg = 0;
	boolean_t touch_present = PE_parse_boot_argn("touchscreen", &touch_arg, sizeof(touch_arg));

	if (!touch_present || touch_arg == 0) {
		log_touch("mt6833_touch: touch disabled (touchscreen boot-arg not set or 0)\n");
		return;
	}

	log_touch("mt6833_touch: LH8n FT8726 driver probe initialized (touchscreen=%u)\n", touch_arg);

	spi4_regs = ml_io_map(MT6833_SPI4_PHYS, MT6833_SPI4_SIZE);
	if (!spi4_regs) {
		log_touch("mt6833_touch: failed to map SPI4 registers at %#llx\n", MT6833_SPI4_PHYS);
		return;
	}

	gpio_regs = ml_io_map(MT6833_GPIO_PHYS, MT6833_GPIO_SIZE);
	if (!gpio_regs) {
		log_touch("mt6833_touch: failed to map GPIO registers at %#llx\n", MT6833_GPIO_PHYS);
		return;
	}

	topckgen_regs = ml_io_map(MT6833_TOPCKGEN_PHYS, MT6833_TOPCKGEN_SIZE);
	if (!topckgen_regs) {
		log_touch("mt6833_touch: failed to map TOPCKGEN registers at %#llx\n", MT6833_TOPCKGEN_PHYS);
		return;
	}

	infracfg_regs = ml_io_map(MT6833_INFRACFG_PHYS, MT6833_INFRACFG_SIZE);
	if (!infracfg_regs) {
		log_touch("mt6833_touch: failed to map INFRACFG registers at %#llx\n", MT6833_INFRACFG_PHYS);
		return;
	}

	log_touch("mt6833_touch: MMIO mapped: SPI4=%p GPIO=%p TOPCKGEN=%p INFRACFG=%p\n",
	          (void *)spi4_regs, (void *)gpio_regs, (void *)topckgen_regs, (void *)infracfg_regs);

	/* Read-only inspection of bootloader/LK handoff state */
	uint32_t cg0 = infracfg_rd(0x00a8);
	uint32_t clk_cfg7 = topckgen_rd(0x0078);
	uint32_t gpio_mode1 = gpio_rd(0x0310);
	uint32_t gpio_mode12 = gpio_rd(0x03c0);
	uint32_t gpio_din0 = gpio_rd(0x0200);

	log_touch("mt6833_touch: HW snapshot: CG0=%#x (spi4_cg=%u) CLK_CFG7=%#x MODE1=%#x MODE12=%#x DIN0=%#x (irq_pin%u=%u)\n",
	          cg0, (cg0 >> 25) & 1, clk_cfg7, gpio_mode1, gpio_mode12, gpio_din0,
	          PIN_TOUCH_IRQ, (gpio_din0 >> PIN_TOUCH_IRQ) & 1);

	bzero(contacts, sizeof(contacts));

	/* Note: Touch monitoring thread will be started in Layer 5 after SPI/IRQ bringup */
	log_touch("mt6833_touch: Layer 1 probe successful, rst=GPIO%u, irq=GPIO%u, MMIO ready\n",
	          PIN_TOUCH_RST, PIN_TOUCH_IRQ);
}
