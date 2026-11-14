/*
 * The OHCI USB controller with nothing plugged in, and the PCI configuration
 * space Mac OS finds it through (Grackle's CONFIG_ADDR/CONFIG_DATA, RTAS).
 */
#include <string.h>

#include "../src/bootinfo.h"
#include "../src/mem.h"
#include "../src/ohci.h"
#include "../src/pci.h"
#include "../src/rtas.h"
#include "test.h"

#define RAM 0x100000u
#define HCCA 0x2000u
#define RTAS_BLOCK 0x3000u
#define FRAME 12000u                    /* bit times per frame (FmInterval 11999) */

/* Operational registers */
enum {
	HC_REVISION = 0x00, HC_CONTROL = 0x04, HC_COMMAND_STATUS = 0x08, HC_INT_STATUS = 0x0C,
	HC_INT_ENABLE = 0x10, HC_INT_DISABLE = 0x14, HC_HCCA = 0x18, HC_DONE_HEAD = 0x30,
	HC_FM_INTERVAL = 0x34, HC_FM_REMAINING = 0x38, HC_FM_NUMBER = 0x3C, HC_RH_A = 0x48,
	HC_RH_STATUS = 0x50, HC_RH_PORT1 = 0x54, HC_RH_PORT2 = 0x58,
};

static struct mem mem;
static struct ohci usb;
static struct pci_bus bus;

/* Registers are little-endian: as the driver sees them through lwbrx/stwbrx */
static uint32_t rd(uint32_t reg)
{
	return __builtin_bswap32(ohci_mmio_ops.read(&usb, reg, 4));
}

static void wr(uint32_t reg, uint32_t v)
{
	ohci_mmio_ops.write(&usb, reg, 4, __builtin_bswap32(v));
}

static uint16_t hcca_frame(void)
{
	return (uint16_t)(mem_r8(&mem, HCCA + 0x80) | mem_r8(&mem, HCCA + 0x81) << 8);
}

static void test_registers(void)
{
	ohci_init(&usb, &mem);
	CHECK_EQ(rd(HC_REVISION), 0x10);
	CHECK_EQ(rd(HC_CONTROL) & 0xC0, 0x00);                  /* USBRESET */
	CHECK_EQ(rd(HC_FM_INTERVAL), 0x2EDF);
	CHECK_EQ(rd(HC_RH_A), 0x01001202);                      /* 2 ports, always powered, no overcurrent */
	CHECK_EQ(rd(HC_RH_PORT1), 0x100);                       /* powered, nothing connected */
	CHECK_EQ(rd(HC_RH_PORT2), 0x100);

	/* narrower accesses see the register's bytes in little-endian order */
	CHECK_EQ(ohci_mmio_ops.read(&usb, HC_RH_A, 1), 0x02);
	CHECK_EQ(ohci_mmio_ops.read(&usb, HC_RH_A + 2, 2), 0x0001);
	ohci_mmio_ops.write(&usb, HC_FM_INTERVAL + 2, 2, 0x7427);       /* FSLargestDataPacket, little-endian */
	CHECK_EQ(rd(HC_FM_INTERVAL), 0x27742EDF);

	/* read-only registers stay put; the HCCA is 256-byte aligned */
	wr(HC_REVISION, 0x55);
	wr(HC_DONE_HEAD, 0x1234);
	CHECK_EQ(rd(HC_REVISION), 0x10);
	CHECK_EQ(rd(HC_DONE_HEAD), 0);
	wr(HC_HCCA, HCCA | 0xFF);
	CHECK_EQ(rd(HC_HCCA), HCCA);

	/* HostControllerReset: registers to their defaults, then USBSUSPEND */
	wr(HC_INT_ENABLE, 0x80000002);
	wr(HC_COMMAND_STATUS, 1);
	CHECK_EQ(rd(HC_CONTROL) & 0xC0, 0xC0);
	CHECK_EQ(rd(HC_HCCA), 0);
	CHECK_EQ(rd(HC_INT_ENABLE), 0);
	CHECK_EQ(rd(HC_FM_INTERVAL), 0x2EDF);
	CHECK_EQ(rd(HC_COMMAND_STATUS), 0);
}

static void test_frames(void)
{
	ohci_init(&usb, &mem);
	mem_w16(&mem, HCCA + 0x80, 0xFFFF);
	ohci_set_time(&usb, 1000);
	wr(HC_HCCA, HCCA);
	CHECK_EQ(rd(HC_FM_REMAINING), 0);                       /* (not running) */

	/* going operational starts a frame */
	wr(HC_CONTROL, 0x80);
	CHECK_EQ(rd(HC_FM_NUMBER), 1);
	CHECK_EQ(hcca_frame(), 1);
	CHECK_EQ(mem_r16(&mem, HCCA + 0x82), 0);                /* HccaPad1 */
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_SF);
	CHECK_EQ(rd(HC_FM_REMAINING) & 0x3FFF, 11999);
	ohci_set_time(&usb, 1000 + 2000);
	CHECK_EQ(rd(HC_FM_REMAINING) & 0x3FFF, 11999 - 2000);

	/* time passes in frames of FmInterval + 1 bit times */
	ohci_set_time(&usb, 1000 + 5 * FRAME + 10);
	CHECK_EQ(rd(HC_FM_NUMBER), 6);
	CHECK_EQ(hcca_frame(), 6);
	CHECK_EQ(usb.frames, 6);
	ohci_set_time(&usb, 1000);                              /* (time doesn't go back) */
	CHECK_EQ(rd(HC_FM_NUMBER), 6);

	/* frame number overflow: bit 15 changes */
	wr(HC_INT_STATUS, OHCI_INT_SF | OHCI_INT_FNO);
	CHECK_EQ(rd(HC_INT_STATUS) & (OHCI_INT_SF | OHCI_INT_FNO), 0);
	ohci_set_time(&usb, 1000 + 0x8000ull * FRAME);
	CHECK_EQ(rd(HC_FM_NUMBER), 0x8001);
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_FNO);

	/* leaving the operational state stops the frames */
	wr(HC_CONTROL, 0xC0);
	ohci_set_time(&usb, 1000 + 0x9000ull * FRAME);
	CHECK_EQ(rd(HC_FM_NUMBER), 0x8001);
}

static void test_interrupts(void)
{
	ohci_init(&usb, &mem);
	wr(HC_CONTROL, 0x80);
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_SF);
	CHECK(!ohci_irq(&usb));                                 /* nothing enabled */
	CHECK_EQ(ohci_next_event(&usb), UINT64_MAX);

	wr(HC_INT_ENABLE, OHCI_INT_SF);
	CHECK(!ohci_irq(&usb));                                 /* not without MasterInterruptEnable */
	wr(HC_INT_ENABLE, OHCI_INT_MIE);
	CHECK_EQ(rd(HC_INT_ENABLE), OHCI_INT_MIE | OHCI_INT_SF);
	CHECK(ohci_irq(&usb));
	CHECK_EQ(ohci_next_event(&usb), FRAME);                 /* the next frame's start */

	wr(HC_INT_STATUS, OHCI_INT_SF);                         /* write 1 to clear */
	CHECK(!ohci_irq(&usb));
	ohci_set_time(&usb, FRAME);
	CHECK(ohci_irq(&usb));

	wr(HC_INT_DISABLE, OHCI_INT_SF);
	CHECK_EQ(rd(HC_INT_ENABLE), OHCI_INT_MIE);
	CHECK(!ohci_irq(&usb));
	CHECK_EQ(ohci_next_event(&usb), UINT64_MAX);
}

static void test_root_hub(void)
{
	ohci_init(&usb, &mem);
	/* enabling or resetting an empty port only reports a connect status change */
	wr(HC_RH_PORT1, 0x10);                                  /* SetPortReset */
	CHECK_EQ(rd(HC_RH_PORT1), 0x10100);                     /* CSC, PPS; not connected or enabled */
	CHECK(rd(HC_INT_STATUS) & OHCI_INT_RHSC);
	wr(HC_RH_PORT1, 0x10000);                               /* clear CSC */
	CHECK_EQ(rd(HC_RH_PORT1), 0x100);
	wr(HC_RH_PORT2, 0x200);                                 /* ClearPortPower: always powered */
	CHECK_EQ(rd(HC_RH_PORT2), 0x100);

	wr(HC_RH_STATUS, 0x8000);                               /* SetRemoteWakeupEnable */
	CHECK_EQ(rd(HC_RH_STATUS), 0x8000);
	wr(HC_RH_STATUS, 0x80000000);                           /* ClearRemoteWakeupEnable */
	CHECK_EQ(rd(HC_RH_STATUS), 0);
}

/* CONFIG_ADDR and CONFIG_DATA are little-endian too */
static void cfg_addr(uint32_t a)
{
	pci_addr_mmio_ops.write(&bus, 0, 4, __builtin_bswap32(a));
}

static uint32_t cfg_rd32(void)
{
	return __builtin_bswap32(pci_data_mmio_ops.read(&bus, 0, 4));
}

static void cfg_wr32(uint32_t v)
{
	pci_data_mmio_ops.write(&bus, 0, 4, __builtin_bswap32(v));
}

static void test_pci(void)
{
	pci_init(&bus);
	pci_add(&bus, PCI_DEVFN_USB, OHCI_PCI_VENDOR, OHCI_PCI_DEVICE, OHCI_PCI_CLASS, OHCI_PCI_REVISION,
		USB_BASE, OHCI_SIZE);
	pci_add(&bus, 0x00, 0x1057, 0x0002, 0x060000, 0x40, 0, 0);

	cfg_addr(0x80000000u | PCI_DEVFN_USB << 8);
	CHECK_EQ(__builtin_bswap32(pci_addr_mmio_ops.read(&bus, 0, 4)), 0x80000000u | PCI_DEVFN_USB << 8);
	CHECK_EQ(cfg_rd32(), 0xC8611045);
	CHECK_EQ(__builtin_bswap16((uint16_t)pci_data_mmio_ops.read(&bus, 2, 2)), 0xC861);     /* the device ID */
	cfg_addr(0x80000008u | PCI_DEVFN_USB << 8);
	CHECK_EQ(cfg_rd32(), 0x0C031010);                       /* OHCI, revision 0x10 */

	/* BAR0 sizes as 4 KB of memory but stays where the device tree put it */
	cfg_addr(0x80000010u | PCI_DEVFN_USB << 8);
	CHECK_EQ(cfg_rd32(), USB_BASE);
	cfg_wr32(0xFFFFFFFF);
	CHECK_EQ(cfg_rd32(), 0xFFFFF000);
	cfg_wr32(USB_BASE);
	CHECK_EQ(cfg_rd32(), USB_BASE);

	/* the command register keeps its writable bits only; IDs are read-only */
	cfg_addr(0x80000004u | PCI_DEVFN_USB << 8);
	cfg_wr32(0xFFFF);
	CHECK_EQ(cfg_rd32() & 0xFFFF, 0x0157);
	cfg_addr(0x80000000u | PCI_DEVFN_USB << 8);
	cfg_wr32(0);
	CHECK_EQ(cfg_rd32(), 0xC8611045);

	/* absent functions, other buses, and accesses without the enable bit read all ones */
	cfg_addr(0x80000000u | 0x08 << 8);
	CHECK_EQ(cfg_rd32(), 0xFFFFFFFF);
	cfg_addr(0x80010000u | PCI_DEVFN_USB << 8);
	CHECK_EQ(cfg_rd32(), 0xFFFFFFFF);
	cfg_addr(PCI_DEVFN_USB << 8);
	CHECK_EQ(cfg_rd32(), 0xFFFFFFFF);
	CHECK_EQ(pci_config_read(&bus, 0, 0x08, 0, 2), 0xFFFF);

	/* RTAS read-pci-config and write-pci-config reach the same functions */
	struct rtas r;
	rtas_init(&r);
	r.pci = &bus;
	memset(mem_ptr(&mem, RTAS_BLOCK), 0, 64);
	mem_w32(&mem, RTAS_BLOCK, RTAS_READ_PCI_CONFIG);
	mem_w16(&mem, RTAS_BLOCK + 6, 2);                       /* (config_addr, size) -> (status, value) */
	mem_w32(&mem, RTAS_BLOCK + 8, 2);
	mem_w32(&mem, RTAS_BLOCK + 12, PCI_DEVFN_USB << 8 | 0x02);
	mem_w32(&mem, RTAS_BLOCK + 16, 2);
	CHECK(rtas_call(&r, &mem, RTAS_BLOCK));
	CHECK_EQ(mem_r32(&mem, RTAS_BLOCK + 20), 0);
	CHECK_EQ(mem_r32(&mem, RTAS_BLOCK + 24), 0xC861);
	mem_w32(&mem, RTAS_BLOCK + 16, 3);                      /* (bad size) */
	CHECK(rtas_call(&r, &mem, RTAS_BLOCK));
	CHECK(mem_r32(&mem, RTAS_BLOCK + 20) != 0);

	mem_w32(&mem, RTAS_BLOCK, RTAS_WRITE_PCI_CONFIG);
	mem_w16(&mem, RTAS_BLOCK + 6, 3);                       /* (config_addr, size, value) -> (status) */
	mem_w32(&mem, RTAS_BLOCK + 8, 1);
	mem_w32(&mem, RTAS_BLOCK + 12, PCI_DEVFN_USB << 8 | 0x0D);
	mem_w32(&mem, RTAS_BLOCK + 16, 1);
	mem_w32(&mem, RTAS_BLOCK + 20, 0x40);
	CHECK(rtas_call(&r, &mem, RTAS_BLOCK));
	CHECK_EQ(mem_r32(&mem, RTAS_BLOCK + 24), 0);
	CHECK_EQ(pci_config_read(&bus, 0, PCI_DEVFN_USB, 0x0D, 1), 0x40);      /* latency timer */
}

void test_ohci(void)
{
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, RAM, MEM_RWX);
	test_registers();
	test_frames();
	test_interrupts();
	test_root_hub();
	test_pci();

	uint32_t masks[2][8];
	bootinfo_level_masks(masks);
	CHECK(masks[0][2] & 0x80000000u >> IRQ_USB);            /* USB interrupts at 68k level 2 */
	mem_free(&mem);
}
