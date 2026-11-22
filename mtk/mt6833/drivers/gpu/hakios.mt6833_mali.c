/*
 * The Mali-G77 of the Infinix GT 10 Pro (X6739, MT6893), brought up from the kernel for programs
 * to draw with. Only with boot-arg mali=N: every address below belongs to this phone.
 *
 * The GPU is a Valhall GPU of architecture version 9 (the phone reads 0x90800011), driven
 * through the job manager -- not CSF -- which is what ARM's own driver does on this phone, so
 * the job chains Mesa's panfrost builds are the ones the hardware expects.
 *
 * The order of the work is MediaTek's own (their clk-mt6893.c, mtk-scpsys-mt6893 and
 * clk-mt6885-pg.c), and it is the order the loader's GPU probe proved on the phone
 * (mtkloader/mtkloader.c, gpu_probe()):
 *
 *   1  the "mfg_sel" clock mux in topckgen, to the main PLL's
 *   2  the seven power domains in scpsys, each with its SRAM, and the bus protection between
 *      the GPU block and the rest of the chip
 *   3  the block's own clock gate (BG3D in mfgcfg)
 *   4  the GPU: which one it is, a soft reset, and its shader cores, tiler and L2 powered
 *   5  its address space: the tables, then the update that makes them live
 *
 * What there is for programs: the parameters of the GPU (as Linux's panfrost driver reports
 * them), buffers (made, entered into the GPU's address space, told their address, mapped), and
 * job chains (submitted and waited for). A chain is built by the program -- Mesa's panfrost
 * does it -- and the driver only points the GPU at it: the kernel never reads a descriptor.
 *
 * The GPU's own page tables are kbase's aarch64 mode, which is what the loader wrote and the
 * hardware walked: four levels of 512 entries, the entry's type in bits 1:0, the access flag in
 * bit 10, the AP field in bits 6:7, the NX flag in bit 54, and the count of the table's valid
 * entries in bits 55:58 of its first three words. The MT6768 reference port's tables are
 * Bifrost LPAE and do not work here.
 *
 * sysctl kern.hakios.mali says how far it got, and the log of the run repeats it.
 */

#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed;
extern uint64_t gMaliGpuUs, gMaliJs0Us, gMaliJs1Us, gMaliJs0Count, gMaliJs1Count, gMaliPipelined;
static int gMaliPipe = 1;
/*
 * Lines that must reach the end-of-run log whole (the display driver's frame statistics use it).
 * The shared stubs define this only when no Mali driver is built; ours is, so it lives here.
 */
void hakios_keep(const char *format, ...) __printflike(1, 2);

void
hakios_keep(const char *format, ...)
{
	char line[200];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
}

/* ---- MT6833 Mali-G57 register map (inlined: headers cannot be listed in a driver conf) ---- */
/*
 * The registers of the Mali-G57 MC2 of the Tecno Pova 5 Pro (LH8n, MT6833).
 *
 * Every number here is out of the loader's own header (mtkloader/mtkmali6893.h), which is where
 * the sources are cited: ARM's kbase r32p1 for the GPU's own registers, MediaTek's clk-mt6893,
 * mtk-scpsys-mt6893 and clk-mt6885-pg.c for the clock and the power. The loader proved this
 * register map on the phone: it brought the GPU up, read its id, and ran a job through its MMU.
 *
 * The G77 is a Valhall GPU of architecture version 9, driven through the job manager (not CSF),
 * which is what ARM's kbase does on this phone as well.
 */

#ifndef HAKIOS_MALI_MT6893_H
#define HAKIOS_MALI_MT6893_H

/* ---- MediaTek's clock and power controllers ---- */

#define TOPCKGEN_BASE       0x10000000ULL
#define INFRACFG_BASE       0x10001000ULL
#define SCPSYS_BASE         0x10006000ULL      /* the power controller */
#define MFGCFG_BASE         0x13fbf000ULL      /* the GPU block's own clock gate */
#define MALI_BASE           0x13000000ULL      /* the GPU's own registers */

/*
 * The GPU clock muxes. CORRECTED (2026-11-19): the old code used CLK_CFG_5 (0x60),
 * but on MT6833 that is camtg3_sel (a camera clock) — it never touched the GPU.
 * Verified from clk-mt6833.ko + /proc/device-tree:
 *   TOPCKGEN CLK_CFG_4 (0x50): mfg_ref_sel bits[17:16], PDN bit 23, UPDATE bit 18;
 *                              mfg_pll_sel bit[18] (0 = mfg_ref_sel_ck, 1 = mfgpll_ck).
 *   APMIXEDSYS MFGPLL at 0x1000C000: CON0 0x268 EN bit0, CON1 0x26c PCW[21:0]/POSDIV[26:24]/CHG bit31,
 *                                    CON3 0x274 PWR_ON bit0 / ISO_EN bit1. 26 MHz ref.
 */
#define APMIXEDSYS_BASE     0x1000C000ULL
#define CLK_CFG_4           0x0050
#define CLK_CFG_4_SET       0x0054
#define CLK_CFG_4_CLR       0x0058
#define CLK_CFG_UPDATE      0x0004
#define CLK_CFG_UPDATE_MFG  (1u << 18)
#define MFG_REF_LSB         16
#define MFG_REF_WIDTH       2
#define MFG_REF_FIELD_MASK  (((1u << MFG_REF_WIDTH) - 1) << MFG_REF_LSB)
#define MFG_REF_PDN         (1u << 23)
#define MFG_PLL_SEL         (1u << 18)          /* 1 = mfgpll_ck, 0 = mfg_ref_sel_ck */
#define MFG_REF_CLK26M      0u                  /* 26 MHz */
#define MFG_REF_MAINPLL_D6_D2 1u                /* 182 MHz */
#define MFG_REF_MAINPLL_D6  2u                  /* 364 MHz */
#define MFG_REF_MAINPLL_D5_D2 3u                /* 218.4 MHz (default) */

#define MFGPLL_CON0         0x268
#define MFGPLL_CON1         0x26c
#define MFGPLL_CON3         0x274
#define MFGPLL_EN           (1u << 0)           /* CON0 */
#define MFGPLL_PWR_ON       (1u << 0)           /* CON3 */
#define MFGPLL_ISO_EN       (1u << 1)           /* CON3 */
#define MFGPLL_PCW_CHG      (1u << 31)          /* CON1 */
#define MFGPLL_POSDIV_1     (1u << 24)          /* /2: VCO 2000 MHz -> 1000 MHz */
/* 1000 MHz stock OPP[08] @ 750 mV (vsram 750 mV), posdiv=1 (/2), VCO 2000 MHz;
 * PCW = round(2000/26 * 16384) = 0x133b13 => CON1 0x81133b13. LK leaves
 * buck_vproc1 (the MT6833 GPU rail) at 850 mV (proved by B7b/B8 probes), a
 * 100 mV headroom over the 750 mV OPP, so NO voltage change is needed. */
#define MFGPLL_1000MHZ_CON1 (MFGPLL_PCW_CHG | MFGPLL_POSDIV_1 | 0x133b13u)

/* mali_clk: 0=ref 218.4MHz (default), 1=ref 364MHz, 2=MFGPLL 1000MHz. */
static int mali_clk_mode;

/* The power controller's status registers for the GPU. */
#define GPU_SPM_PWR_STATUS      0x016c
#define GPU_GPU_SPM_PWR_STATUS_2ND 0x0170

/* The control register of each domain, and its bit in both status registers. */
#define MFG0_PWR_CON        0x0308
#define MFG1_PWR_CON        0x030c
#define MFG2_PWR_CON        0x0310
#define MFG3_PWR_CON        0x0314
#define MFG4_PWR_CON        0x0318
#define MFG5_PWR_CON        0x031c
#define MFG6_PWR_CON        0x0320

#define PWR_RST_B            (1u << 0)
#define PWR_ISO              (1u << 1)
#define PWR_ON               (1u << 2)
#define PWR_ON_2ND           (1u << 3)
#define PWR_CLK_DIS          (1u << 4)
#define PWR_SRAM_PDN         (1u << 8)
#define PWR_SRAM_PDN_ACK     (1u << 12)

/*
 * The bus protection between the GPU block and the rest of the chip: five steps, each its own
 * register pair, released from the last step to the first (that is the order MediaTek's own
 * code clears them in). The set register of a step is four bytes below its clear register.
 */
struct mali_bus_step {
	uint32_t clr;        /* write the mask here to release this step's protection */
	uint32_t sta;        /* read the mask here until it is clear */
	uint32_t mask;
};

#define MALI_BUS_SET_OF(step)  ((step)->clr - 4)

/* The block's clock gate: a set bit in the status means the clock is HELD OFF. */
#define MFG_CG_STA           0x000
#define MFG_CG_SET           0x004
#define MFG_CG_CLR           0x008
#define MFG_CG_BG3D          (1u << 0)

/* ---- the GPU's own registers ---- */

/*
 * One identity register: GPU_ID holds the version and the model. The model is
 * ((id >> 16) & 0xf00f): arch major in bits 31:28, product major in bits 19:16. This phone
 * reads 0x90800011: arch major 9 (Valhall), product major 0 -- the G77.
 */
#define GPU_ID                0x000
#define GPU_ID_MODEL         0xf00f0000u
#define GPU_ID_ARCH_MAJOR    (0xfu << 28)
#define GPU_ID_PRODUCT_MAJOR (0xfu << 16)

/* G57 (0x9093) hardware quirks from mainline panfrost (HW_ISSUE_TTRX_2968_TTRX_3162)
 * and the working viva G57 driver. Applied after soft reset, before core power-up.
 * Without SC_VAR_ALGORITHM the G57's varying hardware misbehaves -> DATA_INVALID_FAULT (0x58). */
#define GPU_COHERENCY_ENABLE   0x304
#define GPU_JM_CONFIG          0xf00
#define GPU_SHADER_CONFIG      0xf04
#define COHERENCY_NONE         31
#define SC_VAR_ALGORITHM       (1u << 29)
#define JM_IDVS_GROUP_SIZE(n)  ((uint32_t)(n) << 16)

#define GPU_IRQ_RAWSTAT       0x020
#define GPU_IRQ_CLEAR         0x024
#define GPU_IRQ_MASK          0x028
#define GPU_IRQ_STATUS        0x02c
#define GPU_COMMAND           0x030
#define GPU_STATUS            0x034

/* The MMU's own features: bits 7:0 the width of the addresses it takes. */
#define GPU_MMU_FEATURES      0x014

#define GPU_IRQ_FAULT               (1u << 0)
#define GPU_IRQ_MULTIPLE_FAULTS     (1u << 7)
#define GPU_IRQ_RESET_COMPLETED     (1u << 8)
#define GPU_IRQ_POWER_CHANGED_SINGLE (1u << 9)
#define GPU_IRQ_POWER_CHANGED_ALL   (1u << 10)
#define GPU_IRQ_PRFCNT_SAMPLE_DONE  (1u << 16)
#define GPU_IRQ_CLEAN_CACHES_DONE   (1u << 17)

#define GPU_COMMAND_NOP             0x00
#define GPU_COMMAND_SOFT_RESET      0x01
#define GPU_COMMAND_HARD_RESET      0x02
#define GPU_COMMAND_CLEAN_CACHES    0x07
#define GPU_COMMAND_CLEAN_INV_CACHES 0x08

#define AS_PRESENT            0x018
#define JS_PRESENT            0x01c
#define GPU_FEATURES_LO       0x060
#define GPU_FEATURES_HI       0x064

#define SHADER_PRESENT_LO     0x100
#define TILER_PRESENT_LO      0x110
#define L2_PRESENT_LO         0x120
#define SHADER_READY_LO       0x140
#define TILER_READY_LO        0x150
#define L2_READY_LO           0x160
#define SHADER_PWRON_LO       0x180
#define TILER_PWRON_LO        0x190
#define L2_PWRON_LO           0x1a0
#define SHADER_PWROFF_LO      0x1c0
#define SHADER_PWRTRANS_LO    0x200
#define SHADER_PWRACTIVE_LO   0x240
#define STACK_PRESENT_LO      0xe00
#define STACK_READY_LO        0xe10
#define STACK_PWRON_LO        0xe20

/* The job manager: a block at 0x1000, slot n at 0x1800 + n * 0x80. */
#define JOB_CONTROL_BASE      0x1000
#define JOB_IRQ_RAWSTAT       (JOB_CONTROL_BASE + 0x000)
#define JOB_IRQ_CLEAR         (JOB_CONTROL_BASE + 0x004)
#define JOB_IRQ_MASK          (JOB_CONTROL_BASE + 0x008)
#define JOB_IRQ_STATUS        (JOB_CONTROL_BASE + 0x00c)

#define JOB_IRQ_DONE(n)       (1u << (n))
#define JOB_IRQ_FAILED(n)     (1u << (16 + (n)))

#define JS_BASE(n)            (0x1800u + (n) * 0x80u)
#define JS_HEAD_LO            0x00
#define JS_HEAD_HI            0x04
#define JS_TAIL_LO            0x08
#define JS_TAIL_HI            0x0c
#define JS_AFFINITY_LO        0x10
#define JS_AFFINITY_HI        0x14
#define JS_CONFIG             0x18
#define JS_XAFFINITY          0x1c
#define JS_COMMAND            0x20
#define JS_STATUS             0x24
#define JS_HEAD_NEXT_LO       0x40
#define JS_HEAD_NEXT_HI       0x44
#define JS_AFFINITY_NEXT_LO   0x50
#define JS_AFFINITY_NEXT_HI   0x54
#define JS_CONFIG_NEXT        0x58
#define JS_COMMAND_NEXT       0x60

#define JS_COMMAND_NOP        0x00
#define JS_COMMAND_START      0x01

#define JS_STATUS_NO_EXCEPTION      0x00
#define JS_STATUS_INTERRUPTED       0x02
#define JS_STATUS_STOPPED           0x03
#define JS_STATUS_TERMINATED        0x04
#define JS_STATUS_CONFIG_FAULT      0x40
#define JS_STATUS_POWER_FAULT       0x41
#define JS_STATUS_READ_FAULT        0x42
#define JS_STATUS_WRITE_FAULT       0x43
#define JS_STATUS_AFFINITY_FAULT    0x44
#define JS_STATUS_BUS_FAULT         0x48
#define JS_STATUS_MEMORY_FAULT_BASE 0x60
#define JS_STATUS_OUT_OF_MEMORY     0x60
#define JS_STATUS_UNKNOWN           0x7f

/*
 * JS_CONFIG: bits 9:8 the flush before the job, bit 10 flush the MMU, bit 11 the chain flag,
 * bits 13:12 the flush after it, bit 14 flush reduction, bit 15 no descriptor write-back,
 * bits 19:16 the thread priority -- and, in the low nibble, the ADDRESS SPACE the job runs in.
 */
#define JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3u << 8)
#define JS_CONFIG_START_MMU                    (1u << 10)
#define JS_CONFIG_JOB_CHAIN_FLAG               (1u << 11)
#define JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE   (3u << 12)
#define JS_CONFIG_ENABLE_FLUSH_REDUCTION       (1u << 14)
#define JS_CONFIG_DISABLE_DESCRIPTOR_WR_BK     (1u << 15)
#define JS_CONFIG_THREAD_PRI(n)                ((n) << 16)

/* The MMU: a block at 0x2000, address space n at 0x2400 + n * 0x40. */
#define MEMORY_MANAGEMENT_BASE 0x2000
#define MMU_IRQ_RAWSTAT        (MEMORY_MANAGEMENT_BASE + 0x000)
#define MMU_IRQ_CLEAR          (MEMORY_MANAGEMENT_BASE + 0x004)
#define MMU_IRQ_MASK           (MEMORY_MANAGEMENT_BASE + 0x008)
#define MMU_IRQ_STATUS         (MEMORY_MANAGEMENT_BASE + 0x00c)

#define MMU_PAGE_FAULT(n)      (1u << (n))
#define MMU_BUS_ERROR(n)       (1u << (16 + (n)))

#define AS_BASE(n)            (MEMORY_MANAGEMENT_BASE + 0x400u + (n) * 0x40u)
#define AS_TRANSTAB_LO        0x00
#define AS_TRANSTAB_HI        0x04
#define AS_MEMATTR_LO         0x08
#define AS_MEMATTR_HI         0x0c
#define AS_LOCKADDR_LO        0x10
#define AS_LOCKADDR_HI        0x14
#define AS_COMMAND            0x18
#define AS_FAULTSTATUS        0x1c
#define AS_FAULTADDRESS_LO    0x20
#define AS_FAULTADDRESS_HI    0x24
#define AS_STATUS             0x28
#define AS_TRANSCFG_LO        0x30
#define AS_TRANSCFG_HI        0x34
#define AS_FAULTEXTRA_LO      0x38
#define AS_FAULTEXTRA_HI      0x3c

#define AS_COMMAND_UPDATE     0x01
#define AS_COMMAND_LOCK       0x02
#define AS_COMMAND_UNLOCK     0x03
#define AS_COMMAND_FLUSH_PT   0x04
#define AS_COMMAND_FLUSH_MEM  0x05
#define AS_STATUS_AS_ACTIVE   (1u << 0)

#define AS_TRANSCFG_ADRMODE_AARCH64_4K  6u
#define AS_TRANSCFG_ADRMODE_MASK        0xfu
#define AS_TRANSCFG_INA_BITS(n)          ((uint32_t)(n) << 6)   /* viva G57: 55-48 */
#define AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK (2ull << 24)
#define AS_TRANSCFG_PTW_SH_OS             (2ull << 28)
#define AS_TRANSCFG_PTW_R_ALLOCATE        (1ull << 30)
#define AS_TRANSTAB_MASK      0xfffffffff0ull

/*
 * The G77's page tables: kbase's aarch64 mode. Four levels of 512 entries; an entry's type in
 * bits 1:0, the access flag in bit 10, the AP field in bits 6:7, the NX flag in bit 54, and the
 * count of the table's valid entries in bits 55:58 of its first three words.
 *
 * The loader wrote exactly this and the GPU walked it (mtkloader/mtkloader.c, gpu_job()), so it
 * is what the driver uses. It is NOT the Bifrost LPAE format the MT6768 reference port uses.
 */
#define GPU_ENTRY_TYPE_MASK   3ull
#define GPU_ENTRY_TABLE       3ull        /* a table at level 0..2 */
#define GPU_ENTRY_LEAF        3ull        /* a page at level 3 */
#define GPU_ENTRY_AF          (1ull << 10)
#define GPU_ENTRY_AP_RO       (1ull << 6) /* bits 6:7: 01 is read-only */
#define GPU_ENTRY_NX          (1ull << 54)
#define GPU_ENTRY_COUNT       (1ull << 55) /* how many of this table's entries are valid */

/* AArch64 stage-2 leaf attributes. Match open-mapple MappleGPU.cpp:707 (PTE_AP_UNPRIV |
 * PTE_SH_IS | PTE_AF | PTE_NG): inner-shareable, not-global, AF set, unprivileged RW. */
#define PTE_AP_UNPRIV         (1ull << 6)
#define PTE_SH_IS             (3ull << 8)
#define PTE_NG                (1ull << 11)

/* The GPU's copy of the system counter (GET_PARAM 41). */
#define GPU_TIMESTAMP_LO      0x098
#define GPU_TIMESTAMP_HI      0x09c

#define GPU_PAGE_SIZE         4096
#define GPU_TABLE_ENTRIES     512

/*
 * The address space the jobs run in: JS_CONFIG's low bits carry it, and the driver's own tables
 * are put in that space. One space for everything, as in the loader.
 */
#define GPU_AS_NUMBER         0

/* The GPU addresses handed out: from Mesa's PAN_VA_USER_START upwards, never given out twice. */
#define GPU_ADDRESS_FIRST     0x02000000ull
#define GPU_ADDRESS_END       (1ull << 35)

#endif /* HAKIOS_MALI_MT6893_H */


extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern vm_offset_t phystokv(uint64_t pa);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);

#define MALI_JOB_SPI          398u
#define MALI_JOB_IRQ_MASK     (JOB_IRQ_DONE(0) | JOB_IRQ_DONE(1) | JOB_IRQ_FAILED(0) | JOB_IRQ_FAILED(1))

extern void HakiosMaliStart(void);
extern void HakiosVivaGPUStart(void);
extern uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed;
extern struct hakios_mali_made { uint64_t address, size; uint32_t asked, flags; } gMaliMade[32];

SYSCTL_DECL(_kern_hakios);

/* ---- the state ---- */

static vm_offset_t topckgen, infracfg, spm, mfgcfg, mali, apmixed;
static uint32_t reached, last_step, gpu_id, shader_mask;
/*
 * Open Mapple's mp_gpu_lkrails trick: with mali_lkrails=1 the clock mux, the seven power
 * domains, the bus protection and the block's clock gate are left exactly as the bootloader
 * (LK) left them. LK already brings the G57 up at its conservative rails/clock (300 MHz on
 * the realme 8i); reprogramming them here is a candidate for the DATA_INVALID_FAULT 0x58
 * every job still hits. The GPU core reset/power and the MMU are still done below.
 */
static int lkrails;
static LCK_GRP_DECLARE(mali_lck_grp, "hakios-mali");
static lck_mtx_t mali_lock;
static lck_mtx_t mali_slot_lock[2];
static volatile uint32_t slot_irq_bits[2];
static uint64_t mali_job_irqs;
static bool mali_irq_attached;
static int mali_irq_enable = 1;

SYSCTL_UINT(_kern_hakios, OID_AUTO, mali, CTLFLAG_RD | CTLFLAG_LOCKED, &reached, 0,
    "how far the Mali came up: 6 is a job run through its own page tables");

/* ---- reading and writing the registers ---- */

static inline uint32_t
rd(uint64_t reg)
{
	return *(volatile uint32_t *)(vm_offset_t)reg;
}

static inline void
wr(uint64_t reg, uint32_t value)
{
	*(volatile uint32_t *)(vm_offset_t)reg = value;
}

/* Until (register & mask) == want, for at most `ms`. 1 if it got there. */
static int
mali_wait(uint64_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t end = mach_absolute_time() + ms * (mach_absolute_time() / 1000) * 1000;
	uint32_t left = ms;

	while (left > 0) {
		if ((rd(reg) & mask) == want) {
			return 1;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		left--;
	}
	return (rd(reg) & mask) == want;
}

static void
mali_job_irq(__unused void *context, __unused uint32_t intid)
{
	uint32_t st = rd(mali + JOB_IRQ_STATUS);

	if (st != 0) {
		wr(mali + JOB_IRQ_CLEAR, st);
		__asm__ volatile("dsb sy\n" ::: "memory");
		mali_job_irqs++;
		for (uint32_t s = 0; s < 2; s++) {
			if (st & (JOB_IRQ_DONE(s) | JOB_IRQ_FAILED(s))) {
				__atomic_fetch_or(&slot_irq_bits[s], st, __ATOMIC_RELEASE);
				thread_wakeup((event_t)&slot_irq_bits[s]);
			}
		}
	}
}

/* B2.4 / Phase 4: spin briefly (<=60 us) for tiny vertex/tiler jobs, then block
 * on GICv3 SPI 398 (JOB_IRQ) so CPU threads yield while longer jobs run. */
static int
mali_wait_slot(uint32_t slot, uint32_t ms, uint32_t *raw_out)
{
	uint32_t mask = JOB_IRQ_DONE(slot) | JOB_IRQ_FAILED(slot);
	uint32_t want = JOB_IRQ_DONE(slot);
	uint64_t start = mach_absolute_time(), limit, elapsed, ns;
	uint32_t raw;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &limit);
	for (;;) {
		raw = (__atomic_load_n(&slot_irq_bits[slot], __ATOMIC_ACQUIRE) | rd(mali + JOB_IRQ_RAWSTAT)) & mask;
		if (raw != 0) {
			*raw_out = raw;
			return (raw & want) == want;
		}
		elapsed = mach_absolute_time() - start;
		if (elapsed >= limit) {
			*raw_out = 0;
			return 0;
		}
		absolutetime_to_nanoseconds(elapsed, &ns);
		if (ns < 60 * NSEC_PER_USEC || !mali_irq_attached) {
			delay(ns < 200 * NSEC_PER_USEC ? 1 : 10);
			continue;
		}
		uint64_t rem_ns = ((uint64_t)ms * NSEC_PER_MSEC > ns) ? ((uint64_t)ms * NSEC_PER_MSEC - ns) : 1;
		uint32_t rem_us = (uint32_t)(rem_ns / NSEC_PER_USEC);
		if (rem_us == 0) {
			rem_us = 1;
		}
		assert_wait_timeout((event_t)&slot_irq_bits[slot], THREAD_UNINT, rem_us, NSEC_PER_USEC);
		raw = (__atomic_load_n(&slot_irq_bits[slot], __ATOMIC_ACQUIRE) | rd(mali + JOB_IRQ_RAWSTAT)) & mask;
		if (raw != 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			*raw_out = raw;
			return (raw & want) == want;
		}
		wait_result_t wres = thread_block(THREAD_CONTINUE_NULL);
		raw = (__atomic_load_n(&slot_irq_bits[slot], __ATOMIC_ACQUIRE) | rd(mali + JOB_IRQ_RAWSTAT)) & mask;
		if (raw != 0) {
			*raw_out = raw;
			return (raw & want) == want;
		}
		if (wres == THREAD_TIMED_OUT) {
			*raw_out = 0;
			return 0;
		}
	}
}

static void
mali_set(uint64_t reg, uint32_t bits)
{
	wr(reg, rd(reg) | bits);
}

static void
mali_clear(uint64_t reg, uint32_t bits)
{
	wr(reg, rd(reg) & ~bits);
}

/* ---- the power domains ---- */

struct mali_domain {
	const char *name;
	uint32_t con;
	uint32_t status;
	int sram;
};

static const struct mali_domain mali_domains[7] = {
	{ "MFG0", MFG0_PWR_CON, 1u << 2, 1 },
	{ "MFG1", MFG1_PWR_CON, 1u << 3, 1 },
	{ "MFG2", MFG2_PWR_CON, 1u << 4, 1 },
	{ "MFG3", MFG3_PWR_CON, 1u << 5, 1 },
	{ "MFG4", MFG4_PWR_CON, 1u << 6, 1 },
	{ "MFG5", MFG5_PWR_CON, 1u << 7, 1 },
	{ "MFG6", MFG6_PWR_CON, 1u << 8, 1 },
};

/*
 * The bus protection between the GPU block and the rest of the chip: five steps, each its own
 * register pair, released from the last step to the first. The order is not what makes it work
 * -- each step is its own register and the chip acknowledges each on its own -- but this is the
 * order the one implementation written for this chip uses.
 */
static const struct mali_bus_step mali_bus_steps[5] = {
	{ 0x02a4, 0x0228, (1u << 21) | (1u << 22) },
	{ 0x0718, 0x0724, (1u << 7) },
	{ 0x0bbc, 0x0bc4, (1u << 17) | (1u << 19) },
	{ 0x02ac, 0x0258, (1u << 19) | (1u << 20) | (1u << 21) },
	{ 0x0718, 0x0724, (1u << 5) | (1u << 6) },
};

/* Up: power, then its second switch; then the clock in, the isolation down, the reset released,
 * and the SRAM woken. 0 means it never said: the step that stalled is in *stage. */
static int
mali_domain_on(const struct mali_domain * d, int * stage)
{
	uint64_t con = spm + d->con;

	*stage = 1;
	mali_set(con, PWR_ON);
	mali_set(con, PWR_ON_2ND);
	if (!mali_wait(spm + GPU_SPM_PWR_STATUS, d->status, d->status, 20) ||
	    !mali_wait(spm + GPU_GPU_SPM_PWR_STATUS_2ND, d->status, d->status, 20)) {
		return 0;
	}
	*stage = 2;
	mali_clear(con, PWR_CLK_DIS);
	mali_clear(con, PWR_ISO);
	mali_set(con, PWR_RST_B);
	if (d->sram) {
		*stage = 3;
		mali_clear(con, PWR_SRAM_PDN);
		if (!mali_wait(con, PWR_SRAM_PDN_ACK, 0, 20)) {
			return 0;
		}
	}
	*stage = 0;
	return 1;
}

/* Down, the way it came up but backwards. */
static int
mali_domain_off(const struct mali_domain * d)
{
	uint64_t con = spm + d->con;
	int answered = 1;

	if (d->sram) {
		mali_set(con, PWR_SRAM_PDN);
		answered = mali_wait(con, PWR_SRAM_PDN_ACK, PWR_SRAM_PDN_ACK, 20);
	}
	mali_set(con, PWR_ISO);
	mali_set(con, PWR_CLK_DIS);
	mali_clear(con, PWR_RST_B);
	mali_clear(con, PWR_ON);
	mali_clear(con, PWR_ON_2ND);
	return mali_wait(spm + GPU_SPM_PWR_STATUS, d->status, 0, 20) &&
	    mali_wait(spm + GPU_GPU_SPM_PWR_STATUS_2ND, d->status, 0, 20) && answered;
}

static int
mali_bus_open(void)
{
	for (int i = 0; i < (int)(sizeof(mali_bus_steps) / sizeof(mali_bus_steps[0])); i++) {
		wr(infracfg + mali_bus_steps[i].clr, mali_bus_steps[i].mask);
		if (!mali_wait(infracfg + mali_bus_steps[i].sta, mali_bus_steps[i].mask, 0, 20)) {
			return 0;
		}
	}
	return 1;
}

static void
mali_bus_close(void)
{
	for (int i = 0; i < (int)(sizeof(mali_bus_steps) / sizeof(mali_bus_steps[0])); i++) {
		wr(infracfg + MALI_BUS_SET_OF(&mali_bus_steps[i]), mali_bus_steps[i].mask);
	}
}

/* ---- the GPU's own page tables ---- */

/*
 * A table of 512 entries at each of the four levels. The tables are made once and kept: a
 * buffer's page is entered into them, and the MMU is told about the change afterwards.
 */
static uint64_t * pt_top;
static uint64_t pt_top_phys;

struct pt_map_entry {
	uint64_t phys;
	uint64_t * va;
};
#define PT_MAP_CAP 4096
static struct pt_map_entry pt_map_table[PT_MAP_CAP];

static uint64_t *
pt_lookup(uint64_t phys)
{
	uint32_t hash = ((phys >> 12) * 2654435761u) & (PT_MAP_CAP - 1);
	while (pt_map_table[hash].va != NULL) {
		if (pt_map_table[hash].phys == phys) {
			return pt_map_table[hash].va;
		}
		hash = (hash + 1) & (PT_MAP_CAP - 1);
	}
	return NULL;
}

static void
pt_insert(uint64_t phys, uint64_t * va)
{
	uint32_t hash = ((phys >> 12) * 2654435761u) & (PT_MAP_CAP - 1);
	while (pt_map_table[hash].va != NULL) {
		if (pt_map_table[hash].phys == phys) {
			pt_map_table[hash].va = va;
			return;
		}
		hash = (hash + 1) & (PT_MAP_CAP - 1);
	}
	pt_map_table[hash].phys = phys;
	pt_map_table[hash].va = va;
}

static uint64_t *
pt_page(uint64_t * phys)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys = kvtophys(page);
	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	return (uint64_t *)page;
}

/*
 * The table below `entry` at `level`, made if there is none yet.
 */
static uint64_t *
pt_below(int level, uint32_t index, uint64_t * entry)
{
	uint64_t phys;
	uint64_t * table;

	(void)level;
	(void)index;
	if (*entry & GPU_ENTRY_TYPE_MASK) {
		phys = *entry & 0x0000fffffffff000ull;
		table = pt_lookup(phys);
		if (table != NULL) {
			return table;
		}
	}
	table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	pt_insert(phys, table);
	/* AArch64 table descriptors: bit 10 (AF) is reserved on tables, not valid.
	 * open-mapple MappleGPU.cpp:634 writes just PTE_TABLE. */
	*entry = phys | GPU_ENTRY_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* The count of a table's valid entries: it goes in bits 55:58 of words 0, 1 and 2. */
static void
pt_set_count(uint64_t * table, uint32_t count)
{
	(void)table;
	(void)count;
}

/* One 4 KB page of physical memory at GPU address `gpu`. */
static bool
map_page(uint64_t gpu, uint64_t phys)
{
	uint64_t * l1, * l2, * l3;
	uint32_t i0 = (gpu >> 39) & (GPU_TABLE_ENTRIES - 1);
	uint32_t i1 = (gpu >> 30) & (GPU_TABLE_ENTRIES - 1);
	uint32_t i2 = (gpu >> 21) & (GPU_TABLE_ENTRIES - 1);
	uint32_t i3 = (gpu >> 12) & (GPU_TABLE_ENTRIES - 1);

	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	l1 = pt_below(0, i0, &pt_top[i0]);
	if (l1 == NULL) {
		return false;
	}
	l2 = pt_below(1, i1, &l1[i1]);
	if (l2 == NULL) {
		return false;
	}
	l3 = pt_below(2, i2, &l2[i2]);
	if (l3 == NULL) {
		return false;
	}
	/* inner-shareable (not outer), not-global, AF, unprivileged RW: match
	 * open-mapple MappleGPU.cpp:707 PTE_AP_UNPRIV | PTE_SH_IS | PTE_AF | PTE_NG. */
	l3[i3] = (phys & ~0xfffull) | GPU_ENTRY_LEAF | GPU_ENTRY_AF |
	    PTE_AP_UNPRIV | PTE_SH_IS | PTE_NG;
	FlushPoC_DcacheRegion((vm_offset_t)&l3[i3], sizeof(l3[i3]));
	return true;
}

static void
as_lock(uint64_t as)
{
	wr(as + AS_LOCKADDR_LO, 47u);
	wr(as + AS_LOCKADDR_HI, 0);
	wr(as + AS_COMMAND, AS_COMMAND_LOCK);
	if (!mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		printf("mali: AS_COMMAND_LOCK timeout\n");
	}
}

/* Program the address space once, in open-mapple MappleGPU.cpp:2131-2146 order:
 * lock, flush whatever the previous owner left in the GPU caches, then write the
 * root table / memory attributes / translation config and UPDATE. */
static void
as_program(uint64_t top_phys)
{
	uint64_t as = mali + AS_BASE(GPU_AS_NUMBER);

	if (!mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		printf("mali: the MMU is busy\n");
		return;
	}
	as_lock(as);
	wr(as + AS_COMMAND, AS_COMMAND_FLUSH_MEM);
	if (!mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		printf("mali: AS_COMMAND_FLUSH_MEM timeout\n");
		return;
	}
	/* Match viva's WORKING G57 (0x9093) MMU config exactly:
	 *  - TRANSCFG_INA_BITS(55-48) sets the invalid-address range used by FLUSH_PT
	 *  - MEMATTR 0x4c4c4c8f/0x4c4c4c4c: inner/outer write-back, GPU-internal
	 *    shareable (non-IO-coherent), others non-cacheable. */
	wr(as + AS_TRANSTAB_LO, (uint32_t)(top_phys & 0xffffffffu));
	wr(as + AS_TRANSTAB_HI, (uint32_t)(top_phys >> 32));
	wr(as + AS_MEMATTR_LO, 0x4c4c4c8fu);
	wr(as + AS_MEMATTR_HI, 0x4c4c4c4cu);
	wr(as + AS_TRANSCFG_LO, AS_TRANSCFG_ADRMODE_AARCH64_4K |
	    AS_TRANSCFG_INA_BITS(55 - 48) |
	    AS_TRANSCFG_PTW_MEMATTR_WRITE_BACK | AS_TRANSCFG_PTW_R_ALLOCATE);
	wr(as + AS_TRANSCFG_HI, 0);
	wr(as + AS_COMMAND, AS_COMMAND_UPDATE);
	if (!mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		printf("mali: AS_COMMAND_UPDATE timeout\n");
	}
}

/* New PTEs were added: lock and flush only the page-table cache, do not reprogram
 * the translation table on every buffer (the old as_update() did). */
static void
as_flush_pt(void)
{
	uint64_t as = mali + AS_BASE(GPU_AS_NUMBER);

	if (!mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		printf("mali: the MMU is busy\n");
		return;
	}
	as_lock(as);
	wr(as + AS_COMMAND, AS_COMMAND_FLUSH_PT);
	if (!mali_wait(as + AS_STATUS, AS_STATUS_AS_ACTIVE, 0, 100)) {
		printf("mali: AS_COMMAND_FLUSH_PT timeout\n");
	}
}

/* The counts of the tables, after the pages went in. */
static void
pt_counts(void)
{
}

/* ---- the GPU addresses ---- */

static uint64_t next_address = GPU_ADDRESS_FIRST;

uint64_t
hakios_mali_address(uint64_t size, uint64_t alignment)
{
	uint64_t address;

	if (alignment == 0) {
		alignment = 0x10000;
	}
	lck_mtx_lock(&mali_lock);
	address = (next_address + alignment - 1) & ~(alignment - 1);
	if (address + size > GPU_ADDRESS_END) {
		address = 0;
	} else {
		next_address = address + ((size + 0xffff) & ~0xffffull);
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

/* One page of physical memory at GPU address `gpu`. */
bool
hakios_mali_map(uint64_t gpu, uint64_t phys)
{
	bool ok;

	lck_mtx_lock(&mali_lock);
	ok = map_page(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return ok;
}

/* A range of GPU addresses is no longer used. */
void
hakios_mali_unmap(uint64_t gpu, uint64_t size)
{
	(void)gpu;
	(void)size;
}

/* The tables have changed: the MMU is told. */
void
hakios_mali_mapped(void)
{
	lck_mtx_lock(&mali_lock);
	pt_counts();
	as_flush_pt();
	lck_mtx_unlock(&mali_lock);
}

/* ---- the parameters ---- */

bool
hakios_mali_ready(void)
{
	return reached >= 6;
}

/*
 * Linux's DRM_PANFROST_PARAM_*, in its order. The numbers are the ones the enum in Mesa's
 * include/drm-uapi/panfrost_drm.h gives; the values come out of the GPU's own registers.
 */
bool
hakios_mali_parameter(uint32_t which, uint64_t * value)
{
	static const uint16_t simple[] = {
		[5] = 0xe00,    /* STACK_PRESENT */
		[6] = 0x018, [7] = 0x01c, [8] = 0x004, [9] = 0x008, [10] = 0x00c, [11] = 0x010, [12] = 0x014,
		[13] = 0x0ac, [14] = 0x0a0, [15] = 0x0a4, [16] = 0x0a8, [17] = 0x300,
		[18] = 0x0b0, [19] = 0x0b4, [20] = 0x0b8, [21] = 0x0bc,
	};

	if (!hakios_mali_ready()) {
		return false;
	}
	switch (which) {
	case 0:                                       /* GPU_PROD_ID */
		*value = (gpu_id >> 16) & 0xffff;
		return true;
	case 1:                                       /* GPU_REVISION */
		*value = gpu_id & 0xffff;
		return true;
	case 2:                                       /* SHADER_PRESENT */
	case 3:                                       /* TILER_PRESENT */
	case 4:                                       /* L2_PRESENT */
		*value = rd(mali + 0x100 + (which - 2) * 0x10) | ((uint64_t)rd(mali + 0x104 + (which - 2) * 0x10) << 32);
		return true;
	case 38:                                      /* NR_CORE_GROUPS */
		*value = 1;
		return true;
	case 39:                                      /* THREAD_TLS_ALLOC */
		*value = rd(mali + 0x310);
		if (*value < rd(mali + 0x0a0)) {
			*value = rd(mali + 0x0a0);
		}
		return true;
	case 40:                                      /* AFBC_FEATURES */
		*value = 0;
		return true;
	case 41: {                                    /* SYSTEM_TIMESTAMP */
		uint32_t hi, lo;
		do {
			hi = rd(mali + GPU_TIMESTAMP_HI);
			lo = rd(mali + GPU_TIMESTAMP_LO);
		} while (hi != rd(mali + GPU_TIMESTAMP_HI));
		*value = hi == 0 && lo == 0 ? mach_absolute_time() : ((uint64_t)hi << 32) | lo;
		return true;
	}
	case 42:                                      /* SYSTEM_TIMESTAMP_FREQUENCY */
		*value = gPEClockFrequencyInfo.timebase_frequency_hz;
		return true;
	case 43:                                      /* ALLOWED_JM_CTX_PRIORITIES */
		*value = 1u << 1;
		return true;
	case 44:                                      /* SELECTED_COHERENCY */
		*value = 31;                           /* VGPU_COHERENCY_NONE */
		return true;
	default:
		if (which >= 22 && which <= 37) {      /* JS_FEATURES0..15 */
			*value = rd(mali + 0x0c0 + (which - 22) * 4);
			return true;
		}
		if (which < sizeof(simple) / sizeof(simple[0]) && simple[which] != 0) {
			*value = rd(mali + simple[which]);
			return true;
		}
		return false;
	}
}

/* ---- the jobs ---- */

/*
 * One job chain, from GPU address `chain`, run to its end before this returns. 0 when it
 * finished; otherwise the slot's status, or 0xff when it did not answer within a second.
 */
static uint64_t
mali_start_slot(uint32_t slot, uint64_t chain)
{
	uint64_t js = mali + JS_BASE(slot);
	static int cfg_logged;

	lck_mtx_lock(&mali_slot_lock[slot]);
	lck_mtx_lock(&mali_lock);
	wr(mali + JOB_IRQ_CLEAR, JOB_IRQ_DONE(slot) | JOB_IRQ_FAILED(slot));
	__atomic_store_n(&slot_irq_bits[slot], 0, __ATOMIC_SEQ_CST);
	wr(js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	wr(js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	wr(js + JS_AFFINITY_NEXT_LO, shader_mask);
	wr(js + JS_AFFINITY_NEXT_HI, 0);
	{
		/* START_MMU (bit 10) deliberately clear: the working G57 driver does not
		 * set it; the MMU is per-AS. Log written vs readback once to check that
		 * THREAD_PRI(8) (0x80000) is not being dropped by the hardware. */
		uint32_t cfg = JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE |
		    JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE |
		    JS_CONFIG_THREAD_PRI(8) |
		    GPU_AS_NUMBER;
		wr(js + JS_CONFIG_NEXT, cfg);
		if (!cfg_logged) {
			cfg_logged = 1;
			printf("mali: submit slot %u: JS_CONFIG_NEXT=%#x JS_CONFIG(readback)=%#x\n",
			    slot, cfg, rd(js + JS_CONFIG));
		}
	}
	while (rd(js + JS_COMMAND_NEXT) != 0) {
		/* the slot is busy with an older job */
	}
	asm volatile("dsb sy\n" ::: "memory");
	wr(js + JS_COMMAND_NEXT, JS_COMMAND_START);
	lck_mtx_unlock(&mali_lock);
	return mach_absolute_time();
}

static uint32_t
mali_finish_slot(uint32_t slot, uint64_t tGpu)
{
	uint64_t js = mali + JS_BASE(slot);
	uint64_t gpuNs, us;
	uint32_t raw, status = 0;
	bool finished;

	finished = mali_wait_slot(slot, 1000, &raw);
	lck_mtx_lock(&mali_lock);
	raw |= rd(mali + JOB_IRQ_RAWSTAT);
	if (!finished || (raw & JOB_IRQ_FAILED(slot))) {
		uint32_t as_base = AS_BASE(GPU_AS_NUMBER);
		uint32_t fault_stat = rd(mali + as_base + AS_FAULTSTATUS);
		uint64_t fault_addr = ((uint64_t)rd(mali + as_base + AS_FAULTADDRESS_HI) << 32) |
		                      rd(mali + as_base + AS_FAULTADDRESS_LO);
		uint32_t js_stat = rd(js + JS_STATUS);
		uint32_t mmu_stat = rd(mali + MMU_IRQ_RAWSTAT);

		status = (!finished && !(raw & JOB_IRQ_FAILED(slot))) ? 0xff : js_stat;
		gMaliFailed++;

		printf("mali: job failure slot %u: int %#x, status %#x, MMU %#x, fault %#x at %#llx\n",
		    slot, raw, js_stat, mmu_stat, fault_stat, fault_addr);
		{
			uint32_t head_lo = rd(js + JS_HEAD_LO), head_hi = rd(js + JS_HEAD_HI);
			uint32_t tail_lo = rd(js + JS_TAIL_LO), tail_hi = rd(js + JS_TAIL_HI);
			uint32_t js_cfg = rd(js + JS_CONFIG);
			uint32_t js_cmd = rd(js + JS_COMMAND);
			printf("mali:   slot %u regs: head=%#x:%#x tail=%#x:%#x cfg=%#x cmd=%#x\n",
			    slot, head_hi, head_lo, tail_hi, tail_lo, js_cfg, js_cmd);
		}

		if (fault_addr != 0) {
			uint32_t f0 = (fault_addr >> 39) & (GPU_TABLE_ENTRIES - 1);
			uint32_t f1 = (fault_addr >> 30) & (GPU_TABLE_ENTRIES - 1);
			uint32_t f2 = (fault_addr >> 21) & (GPU_TABLE_ENTRIES - 1);
			uint32_t f3 = (fault_addr >> 12) & (GPU_TABLE_ENTRIES - 1);
			uint64_t e0 = pt_top ? pt_top[f0] : 0;
			uint64_t * l1 = (e0 & GPU_ENTRY_TYPE_MASK) ? pt_lookup(e0 & 0x0000fffffffff000ull) : NULL;
			uint64_t e1 = l1 ? l1[f1] : 0;
			uint64_t * l2 = (e1 & GPU_ENTRY_TYPE_MASK) ? pt_lookup(e1 & 0x0000fffffffff000ull) : NULL;
			uint64_t e2 = l2 ? l2[f2] : 0;
			uint64_t * l3 = (e2 & GPU_ENTRY_TYPE_MASK) ? pt_lookup(e2 & 0x0000fffffffff000ull) : NULL;
			uint64_t e3 = l3 ? l3[f3] : 0;

			printf("mali:   walk: L0[%u]=%#llx L1[%u]=%#llx L2[%u]=%#llx L3[%u]=%#llx (l3=%p)\n",
			    f0, e0, f1, e1, f2, e2, f3, e3, l3);

			for (uint32_t i = 0; i < gMaliBuffers && i < 32; i++) {
				printf("mali:   bo[%u] %#llx..%#llx (size %llu, asked %u, flags %#x)%s\n",
				    i + 1, gMaliMade[i].address, gMaliMade[i].address + gMaliMade[i].size,
				    gMaliMade[i].size, gMaliMade[i].asked, gMaliMade[i].flags,
				    (fault_addr >= gMaliMade[i].address && fault_addr < gMaliMade[i].address + gMaliMade[i].size) ? " <- FAULT IN BO" : "");
			}
		}

		if (mmu_stat) {
			wr(mali + as_base + AS_COMMAND, AS_COMMAND_UNLOCK);
			wr(mali + as_base + AS_COMMAND, AS_COMMAND_FLUSH_PT);
			wr(mali + MMU_IRQ_CLEAR, mmu_stat);
		}
	}
	wr(mali + JOB_IRQ_CLEAR, raw & (JOB_IRQ_DONE(slot) | JOB_IRQ_FAILED(slot)));
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_slot_lock[slot]);
	absolutetime_to_nanoseconds(mach_absolute_time() - tGpu, &gpuNs);
	us = gpuNs / 1000;
	__atomic_fetch_add(&gMaliGpuUs, us, __ATOMIC_RELAXED);
	if (slot == 0) {
		__atomic_fetch_add(&gMaliJs0Us, us, __ATOMIC_RELAXED);
		__atomic_fetch_add(&gMaliJs0Count, 1, __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_add(&gMaliJs1Us, us, __ATOMIC_RELAXED);
		__atomic_fetch_add(&gMaliJs1Count, 1, __ATOMIC_RELAXED);
	}
	return status;
}

uint32_t
hakios_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0 : 1;
	return mali_finish_slot(slot, mali_start_slot(slot, chain));
}

/*
 * Async job queue: submits return a monotonic sequence number immediately while
 * mali_worker() drains the chain in exact FIFO order via JS0/JS1 + JOB_IRQ.
 * When consecutive paired batches use distinct tiler heaps (heap_tag), JS1 of
 * batch N+1 overlaps on slot 1 while JS0 of batch N runs on slot 0.
 */
#define MALI_QUEUE      64
struct mali_queue_item {
	uint64_t chain, chain2, seq;
	uint32_t heap_tag;
	bool fragment;
};
static struct mali_queue_item mali_queue[MALI_QUEUE];
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lck_grp);

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	bool have_prefetched = false;
	struct mali_queue_item prefetched = { 0 };
	uint64_t prefetched_tGpu = 0;

	for (;;) {
		struct mali_queue_item cur;

		if (have_prefetched) {
			cur = prefetched;
			have_prefetched = false;
			(void)mali_finish_slot(1, prefetched_tGpu);
		} else {
			lck_mtx_lock(&mali_queue_lock);
			while (mali_queue_count == 0) {
				lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_count, THREAD_UNINT);
			}
			cur = mali_queue[mali_queue_head];
			lck_mtx_unlock(&mali_queue_lock);

			if (cur.chain2 != 0) {
				(void)mali_finish_slot(1, mali_start_slot(1, cur.chain));
			} else {
				(void)hakios_mali_submit(cur.chain, cur.fragment);
				goto retire;
			}
		}

		{
			uint64_t tFrag = mali_start_slot(0, cur.chain2);

			if (gMaliPipe && cur.heap_tag != 0) {
				lck_mtx_lock(&mali_queue_lock);
				if (mali_queue_count >= 2) {
					unsigned next_idx = (mali_queue_head + 1) % MALI_QUEUE;
					struct mali_queue_item next = mali_queue[next_idx];

					if (next.chain2 != 0 && next.heap_tag != 0 && next.heap_tag != cur.heap_tag) {
						prefetched = next;
						have_prefetched = true;
						__atomic_fetch_add(&gMaliPipelined, 1, __ATOMIC_RELAXED);
					}
				}
				lck_mtx_unlock(&mali_queue_lock);
				if (have_prefetched) {
					prefetched_tGpu = mali_start_slot(1, prefetched.chain);
				}
			}
			(void)mali_finish_slot(0, tFrag);
		}

retire:
		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1) % MALI_QUEUE;
		mali_queue_count--;
		__atomic_store_n(&mali_done_seq, cur.seq, __ATOMIC_RELEASE);
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);
	}
}

static uint64_t
mali_enqueue_common(uint64_t chain, uint64_t chain2, uint32_t heap_tag, bool fragment)
{
	uint64_t seq;

	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter,
		    integer_t priority, thread_t *new_thread);
		thread_t thread;

		if (kernel_thread_start_priority(mali_worker, NULL, 93, &thread) == KERN_SUCCESS) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_queue_head, THREAD_UNINT);
	}
	seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;

	mali_queue[at].chain = chain;
	mali_queue[at].chain2 = chain2;
	mali_queue[at].heap_tag = heap_tag;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq = seq;
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

uint64_t hakios_mali_enqueue(uint64_t chain, bool fragment);
uint64_t
hakios_mali_enqueue(uint64_t chain, bool fragment)
{
	return mali_enqueue_common(chain, 0, 0, fragment);
}

uint64_t hakios_mali_enqueue_pair(uint64_t chain, uint64_t chain2, uint32_t heap_tag);
uint64_t
hakios_mali_enqueue_pair(uint64_t chain, uint64_t chain2, uint32_t heap_tag)
{
	return mali_enqueue_common(chain, chain2, heap_tag, false);
}

bool hakios_mali_wait(uint64_t seq, uint64_t timeout_ns);
bool
hakios_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	uint64_t deadline;
	bool done;

	if (seq <= __atomic_load_n(&mali_done_seq, __ATOMIC_ACQUIRE)) {
		return true;
	}
	if (timeout_ns == 0) {
		return false;
	}
	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;
	}
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();
	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	done = mali_done_seq >= seq;
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

uint64_t hakios_mali_done_seq(void);
uint64_t
hakios_mali_done_seq(void)
{
	return __atomic_load_n(&mali_done_seq, __ATOMIC_ACQUIRE);
}

/* ---- the bring-up ---- */

/*
 * The GPU is read: what it is, and how much of it there is.
 */
static void
gpu_read(void)
{
	uint32_t id = rd(mali + GPU_ID);

	gpu_id = id;
	shader_mask = rd(mali + SHADER_PRESENT_LO);
	printf("mali: GPU_ID %#x", id);
	if ((id & GPU_ID_ARCH_MAJOR) == (9u << 28)) {
		printf(": Mali-G57 MC2 (Valhall)");
	} else {
		printf(": not the G57 this driver is for");
	}
	printf(", %u shader cores (present %#x), tiler %#x, L2 %#x\n", __builtin_popcount(shader_mask),
	    shader_mask, rd(mali + TILER_PRESENT_LO), rd(mali + L2_PRESENT_LO));
}

/*
 * The GPU's cores, tiler and L2 are powered, and it is reset.
 */
static int
gpu_start(void)
{
	wr(mali + GPU_IRQ_CLEAR, 0xffffffffu);
	wr(mali + GPU_COMMAND, GPU_COMMAND_SOFT_RESET);
	if (!mali_wait(mali + GPU_IRQ_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100)) {
		printf("mali: the GPU did not come out of its reset\n");
		return 0;
	}

	/* G57 (0x9093) quirks, from mainline panfrost + the working viva G57 driver:
	 * SC_VAR_ALGORITHM fixes the varying algorithm (HW_ISSUE_TTRX_2968_TTRX_3162),
	 * missing on G77 which is why our X6739-derived driver never set it. Without this
	 * the G57 rejects jobs with DATA_INVALID_FAULT 0x58. */
	wr(mali + GPU_SHADER_CONFIG, rd(mali + GPU_SHADER_CONFIG) | SC_VAR_ALGORITHM);
	wr(mali + GPU_JM_CONFIG, rd(mali + GPU_JM_CONFIG) | JM_IDVS_GROUP_SIZE(0xf));
	wr(mali + GPU_COHERENCY_ENABLE, COHERENCY_NONE);
	printf("mali: G57 quirks: SHADER_CFG=%#x JM_CFG=%#x COHERENCY=%#x\n",
	    rd(mali + GPU_SHADER_CONFIG), rd(mali + GPU_JM_CONFIG), rd(mali + GPU_COHERENCY_ENABLE));

	/*
	 * Power the cores in MediaTek's order (open-mapple MappleGPU.cpp:1954-1966):
	 * L2 first and wait for it, then the core stacks, then tiler and shaders.
	 * The old code wrote STACK_READY_LO (0 after SOFT_RESET) to STACK_PWRON_LO,
	 * so the stacks were never powered on. Log both registers so pstore confirms it.
	 */
	{
		uint32_t l2present = rd(mali + L2_PRESENT_LO);
		uint32_t stackpresent = rd(mali + STACK_PRESENT_LO);

		printf("mali: power-on: L2_PRESENT=%#x STACK_PRESENT=%#x STACK_READY(before)=%#x\n",
		    l2present, stackpresent, rd(mali + STACK_READY_LO));
		wr(mali + L2_PWRON_LO, l2present);
		if (!mali_wait(mali + L2_READY_LO, l2present, l2present, 50)) {
			printf("mali: L2 did not power up (ready=%#x want=%#x)\n", rd(mali + L2_READY_LO), l2present);
			return 0;
		}
		if (stackpresent != 0) {
			wr(mali + STACK_PWRON_LO, stackpresent);
			/* open-mapple MappleGPU.cpp:1960-1963 polls STACK_READY but DISCARDS
			 * the result: on this G57 STACK_READY_LO can stay 0 even when the
			 * stacks are usable (the old driver never wrote STACK_PWRON_LO at
			 * all and still submitted jobs). Never fatal — log it only. */
			(void)mali_wait(mali + STACK_READY_LO, stackpresent, stackpresent, 20);
		}
		printf("mali: power-on: L2_READY=%#x STACK_READY=%#x\n",
		    rd(mali + L2_READY_LO), rd(mali + STACK_READY_LO));
	}
	wr(mali + TILER_PWRON_LO, rd(mali + TILER_PRESENT_LO));
	wr(mali + SHADER_PWRON_LO, rd(mali + SHADER_PRESENT_LO));
	if (!mali_wait(mali + SHADER_READY_LO, rd(mali + SHADER_PRESENT_LO), rd(mali + SHADER_PRESENT_LO), 50)) {
		printf("mali: the shader cores did not come up\n");
		return 0;
	}
	if (!mali_wait(mali + TILER_READY_LO, rd(mali + TILER_PRESENT_LO), rd(mali + TILER_PRESENT_LO), 50)) {
		printf("mali: the tiler did not come up\n");
		return 0;
	}
	if (!mali_wait(mali + L2_READY_LO, rd(mali + L2_PRESENT_LO), rd(mali + L2_PRESENT_LO), 50)) {
		printf("mali: the L2 did not come up\n");
		return 0;
	}
	/* After the waits: this is the settled ready state, not the transient one. */
	printf("mali: power-on: after SHADER_PWRON: STACK_READY=%#x SHADER_READY=%#x TILER_READY=%#x L2_READY=%#x\n",
	    rd(mali + STACK_READY_LO), rd(mali + SHADER_READY_LO), rd(mali + TILER_READY_LO), rd(mali + L2_READY_LO));
	return 1;
}

/*
 * A line from the kernel, kept and said again every minute (the tree's own convention).
 */
static uint32_t kept_next;
static char kept_lines[96][200];
static lck_mtx_t kept_lock;

void
hakios_mali_keep(const char * format, ...) __printflike(1, 2);

void
hakios_mali_keep(const char * format, ...)
{
	va_list ap;
	char line[200];

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("%s\n", line);
	lck_mtx_lock(&kept_lock);
	strlcpy(kept_lines[kept_next % 96], line, sizeof(kept_lines[0]));
	kept_next++;
	lck_mtx_unlock(&kept_lock);
}

/*
 * The GPU comes up: the clock, the power, the bus, the block's own gate, then the GPU itself.
 */
void
hakios_mali_init(void)
{
	uint32_t clk_cfg4;
	uint32_t stage;
	int up = 0;

	if (!PE_parse_boot_argn("mali", &last_step, sizeof(last_step))) {
		return;                               /* (mali is not asked for: the GPU is left alone) */
	}
	if (last_step == 0 || last_step > 6) {
		return;
	}
	(void)PE_parse_boot_argn("mali_lkrails", &lkrails, sizeof(lkrails));
	(void)PE_parse_boot_argn("mali_clk", &mali_clk_mode, sizeof(mali_clk_mode));
	(void)PE_parse_boot_argn("mali_irq", &mali_irq_enable, sizeof(mali_irq_enable));
	(void)PE_parse_boot_argn("mali_pipe", &gMaliPipe, sizeof(gMaliPipe));
	lck_mtx_init(&mali_lock, &mali_lck_grp, LCK_ATTR_NULL);
	lck_mtx_init(&mali_slot_lock[0], &mali_lck_grp, LCK_ATTR_NULL);
	lck_mtx_init(&mali_slot_lock[1], &mali_lck_grp, LCK_ATTR_NULL);
	lck_mtx_init(&kept_lock, &mali_lck_grp, LCK_ATTR_NULL);

	topckgen = ml_io_map(TOPCKGEN_BASE, 0x1000);
	infracfg = ml_io_map(INFRACFG_BASE, 0x1000);
	spm = ml_io_map(SCPSYS_BASE, 0x1000);
	mfgcfg = ml_io_map(MFGCFG_BASE, 0x1000);
	mali = ml_io_map(MALI_BASE, 0x10000);
	apmixed = ml_io_map(APMIXEDSYS_BASE, 0x1000);
	if (topckgen == 0 || infracfg == 0 || spm == 0 || mfgcfg == 0 || mali == 0 || apmixed == 0) {
		printf("mali: the GPU's registers are not there to be read\n");
		return;
	}
	if (lkrails) {
		/* Leave the clock, the rails, the bus and the gate as LK set them. */
		printf("mali: lkrails: leaving LK's clock, rails and gate as they are\n");
		reached = 4;
		clk_cfg4 = rd(topckgen + CLK_CFG_4);
	} else {
		clk_cfg4 = rd(topckgen + CLK_CFG_4);
		/* Log what LK actually left the GPU at BEFORE we touch anything. */
		printf("mali: clk: BEFORE CLK_CFG_4=%#x (mfg_ref=%u, mfg_pll_sel=%u) "
		    "MFGPLL CON0=%#x CON1=%#x CON3=%#x\n",
		    clk_cfg4, (clk_cfg4 & MFG_REF_FIELD_MASK) >> MFG_REF_LSB,
		    (clk_cfg4 & MFG_PLL_SEL) ? 1 : 0,
		    rd(apmixed + MFGPLL_CON0), rd(apmixed + MFGPLL_CON1), rd(apmixed + MFGPLL_CON3));

		/* Always start on the ref clock (mfg_pll_sel=0) at 364 MHz, a clean
		 * glitch-free divider, so a running GPU is never without a clock
		 * while we program MFGPLL. */
		wr(topckgen + CLK_CFG_4_CLR, MFG_PLL_SEL);
		wr(topckgen + CLK_CFG_4_CLR, MFG_REF_FIELD_MASK | MFG_REF_PDN);
		wr(topckgen + CLK_CFG_4_SET, MFG_REF_MAINPLL_D6 << MFG_REF_LSB);
		wr(topckgen + CLK_CFG_UPDATE, rd(topckgen + CLK_CFG_UPDATE) | CLK_CFG_UPDATE_MFG);

		if (mali_clk_mode >= 2) {
			/* MFGPLL 1000 MHz @ stock OPP[08] 750 mV (LK rail is 850 mV — no voltage
			 * change): power/ungate, program PCW+POSDIV+PCW_CHG, enable, lock,
			 * then switch mfg_pll_sel to the PLL. */
			wr(apmixed + MFGPLL_CON3, rd(apmixed + MFGPLL_CON3) | MFGPLL_PWR_ON);
			delay_for_interval(2, NSEC_PER_USEC);
			wr(apmixed + MFGPLL_CON3, rd(apmixed + MFGPLL_CON3) & ~MFGPLL_ISO_EN);
			delay_for_interval(2, NSEC_PER_USEC);
			wr(apmixed + MFGPLL_CON1, MFGPLL_1000MHZ_CON1);
			wr(apmixed + MFGPLL_CON0, rd(apmixed + MFGPLL_CON0) | MFGPLL_EN);
			delay_for_interval(50, NSEC_PER_USEC);
			wr(topckgen + CLK_CFG_4_SET, MFG_PLL_SEL);
		} else {
			uint32_t parent = (mali_clk_mode == 1) ? MFG_REF_MAINPLL_D6 : MFG_REF_MAINPLL_D5_D2;

			wr(topckgen + CLK_CFG_4_CLR, MFG_REF_FIELD_MASK | MFG_REF_PDN);
			wr(topckgen + CLK_CFG_4_SET, parent << MFG_REF_LSB);
			wr(topckgen + CLK_CFG_UPDATE, rd(topckgen + CLK_CFG_UPDATE) | CLK_CFG_UPDATE_MFG);
		}
		reached = 1;
		printf("mali: clk: mali_clk=%d -> %s; CLK_CFG_4=%#x (mfg_ref=%u, mfg_pll_sel=%u) "
		    "MFGPLL CON0=%#x CON1=%#x CON3=%#x\n",
		    mali_clk_mode,
		    mali_clk_mode >= 2 ? "MFGPLL 1000MHz" :
		        (mali_clk_mode == 1 ? "ref 364MHz" : "ref 218.4MHz"),
		    rd(topckgen + CLK_CFG_4),
		    (rd(topckgen + CLK_CFG_4) & MFG_REF_FIELD_MASK) >> MFG_REF_LSB,
		    (rd(topckgen + CLK_CFG_4) & MFG_PLL_SEL) ? 1 : 0,
		    rd(apmixed + MFGPLL_CON0), rd(apmixed + MFGPLL_CON1), rd(apmixed + MFGPLL_CON3));
		if (last_step < 2) {
			goto done;
		}

		/* the seven power domains */
		for (up = 0; up < 7; up++) {
			const struct mali_domain * d = &mali_domains[up];

			if (!mali_domain_on(d, &stage)) {
				printf("mali: %s stalled at step %d; its control %#x, status %#x\n", d->name, stage,
				    rd(spm + d->con), rd(spm + GPU_SPM_PWR_STATUS));
				reached = 0;
				goto off;
			}
			reached = 2;
			if (up == 1) {
				if (!mali_bus_open()) {
					printf("mali: the bus protection did not open\n");
					reached = 0;
					goto off;
				}
				reached = 3;
			}
		}
		if (last_step < 3) {
			goto done;
		}

		/* the block's own clock gate: a set bit in the status means the clock is held off */
		wr(mfgcfg + MFG_CG_CLR, MFG_CG_BG3D);
		reached = 4;
	}
	if (last_step < 4) {
		goto done;
	}

	if (!gpu_start()) {
		reached = 0;
		goto off;
	}
	reached = 5;
	gpu_read();
	if (last_step < 5) {
		goto done;
	}

	/*
	 * The address space: the tables are made, and the MMU is told about them, so a job can be
	 * run through them. This is what the loader proved on the phone.
	 */
	if (pt_top == NULL) {
		pt_top = pt_page(&pt_top_phys);
		if (pt_top != NULL) {
			pt_insert(pt_top_phys, pt_top);
		}
	}
	if (pt_top == NULL) {
		printf("mali: no memory for the GPU's page tables\n");
		reached = 0;
		goto off;
	}
	pt_set_count(pt_top, 1);
	FlushPoC_DcacheRegion((vm_offset_t)pt_top, PAGE_SIZE);
	as_program(pt_top_phys);
	if (mali_irq_enable) {
		wr(mali + JOB_IRQ_CLEAR, 0xffffffffu);
		wr(mali + JOB_IRQ_MASK, MALI_JOB_IRQ_MASK);
		kern_return_t kr = gic_v3_spi_attach(MALI_JOB_SPI, false, mali_job_irq, NULL);
		mali_irq_attached = (kr == KERN_SUCCESS);
		printf("mali: JOB_IRQ SPI %u attach %s (kr=%d, mask=%#x)\n",
		    MALI_JOB_SPI, mali_irq_attached ? "OK" : "FAILED", kr, rd(mali + JOB_IRQ_MASK));
	}
	reached = 6;

done:
	printf("mali: the GPU is up; the service HakiosPanfrostGPU is for programs to draw with\n");
	HakiosMaliStart();
	/* vivagpu=N publishes the vgpu UAPI service for the prebuilt VivaMTLDriver.bundle */
	{
		int vivagpu = 0;
		if (PE_parse_boot_argn("vivagpu", &vivagpu, sizeof(vivagpu)) && vivagpu != 0) {
			HakiosVivaGPUStart();
		}
	}
	return;

off:
	/* everything we brought up goes down again; with lkrails nothing here was ours to take down */
	if (!lkrails) {
		for (int i = up - 1; i >= 0; i--) {
			(void)mali_domain_off(&mali_domains[i]);
		}
		wr(mfgcfg + MFG_CG_SET, MFG_CG_BG3D);
		mali_bus_close();
		/* Leave the PLL before restoring the ref mux. */
		wr(topckgen + CLK_CFG_4_CLR, MFG_PLL_SEL);
		wr(topckgen + CLK_CFG_4_CLR, MFG_REF_FIELD_MASK | MFG_REF_PDN);
		wr(topckgen + CLK_CFG_4_SET, clk_cfg4 & MFG_REF_FIELD_MASK);
		wr(topckgen + CLK_CFG_UPDATE, rd(topckgen + CLK_CFG_UPDATE) | CLK_CFG_UPDATE_MFG);
	}
	printf("mali: the GPU did not come up; the system draws with its software renderer\n");
}
