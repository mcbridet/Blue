#include "bootinfo.h"

#include <stdio.h>
#include <string.h>

#include "be.h"
#include "devtree.h"
#include "nvram.h"
#include "ohci.h"

/* Our layout inside the boot info area */
#define DEVICE_TREE_OFFSET 0x40000u
#define DEVICE_TREE_SIZE 0x10000u
#define PRODUCT_INFO_OFFSET 0x50000u
#define VECTOR_MASK_OFFSET 0x51000u
#define VECTOR_LOOKUP_OFFSET 0x51040u
/* (a tree that doesn't fit, with the libraries and drivers of a ROM file's parcels, goes here) */
#define BIG_DEVICE_TREE_OFFSET 0x100000u
#define BIG_DEVICE_TREE_SIZE 0x200000u

/* Dynamic ProductInfo; the 68k ROM merges its built-in NewWorld template into it. */
enum {
	PI_DECODER_INFO_PTR = 0x00,     /* offset of DecoderInfo from ProductInfo */
	PI_HW_CFG_WORD = 0x10,
	PI_PRODUCT_KIND = 0x12,
	PI_DECODER_KIND = 0x13,
	PI_VERSION = 0x17,
	PI_BASES_VALID = 0x18,          /* bit n: DecoderInfo long n is valid */
	PI_EXT_VALID = 0x1C,            /* bit n: DecoderInfo long 32+n is valid */
	PI_FLAGS_24 = 0x24,
	PI_FLAGS_28 = 0x28,
	PI_SIZE = 0xA0,                 /* DecoderInfo follows immediately */
};

/* DecoderInfo: one base address per long */
enum {
	DI_ROM = 0x00,
	DI_VIA1 = 0x08,
	DI_SCC_READ = 0x0C,
	DI_SCC_WRITE = 0x10,
	DI_AWACS = 0x9C,                /* long 39: the sound chip (Gestalt 'snhw' is 'awgc' with Mac I/O) */
	DI_MACIO = 0xE4,                /* long 57 */
	DI_FLAGS_F8 = 0xF8,             /* long 62 */
	DI_SIZE = 0x100,
};

#define VECTOR_NONE 0x0800u
#define VECTOR_BIT_NONE 0x0040u

/* HWInfo offsets of the RTAS token fields, in the order of enum rtas_token */
static const uint8_t rtas_token_field[RTAS_TOKEN_COUNT] = {
	[RTAS_NVRAM_FETCH] = 0x40,
	[RTAS_NVRAM_STORE] = 0x44,
	[RTAS_GET_TIME_OF_DAY] = 0x48,
	[RTAS_SET_TIME_OF_DAY] = 0x4C,
	[RTAS_SYSTEM_REBOOT] = 0x50,
	[RTAS_POWER_OFF] = 0x54,
	[RTAS_SET_TIME_FOR_POWER_ON] = 0x58,
	[RTAS_EVENT_SCAN] = 0x5C,
	[RTAS_CHECK_EXCEPTION] = 0x60,
	[RTAS_READ_PCI_CONFIG] = 0x64,
	[RTAS_WRITE_PCI_CONFIG] = 0x68,
	[RTAS_GET_TIME_FOR_POWER_ON] = 0x98,
};

/* PCI address cells: phys.hi (space code, bus, devfn, register), phys.mid, phys.lo */
#define PCI_SPACE_CONFIG 0x00000000u
#define PCI_SPACE_IO 0x01000000u
#define PCI_SPACE_MEM32 0x02000000u
#define PCI_NON_RELOCATABLE 0x80000000u
#define PCI_BAR0 0x10u

/*
 * Asks for a Driver Services interrupt set for a device's interrupts (up to
 * 3), as its driver-ist: the ROM's InterruptTreeNew builds one from every
 * node with AAPL,interrupt-priorities, whose parallel AAPL,interrupts (here
 * the sources), AAPL,interrupt-priorities (68k levels) and
 * AAPL,interrupt-vectors (sources) it reads 3 at most.
 */
static void interrupt_set(struct dt_node *n, const uint32_t *sources, unsigned count)
{
	uint32_t masks[2][8], levels[3] = { 0 };
	bootinfo_level_masks(masks);
	for (unsigned k = 0; k < count && k < 3; k++)
		for (unsigned l = 1; l < 8; l++)
			if (masks[sources[k] / 32][l] & 0x80000000u >> (sources[k] % 32))
				levels[k] = l;
	dt_prop_cells(n, "AAPL,interrupts", sources, count);
	dt_prop_cells(n, "AAPL,interrupt-priorities", levels, count);
	dt_prop_cells(n, "AAPL,interrupt-vectors", sources, count);
}

static void pci_ids(struct dt_node *n, uint32_t vendor, uint32_t device, uint32_t class_code, uint32_t rev)
{
	dt_prop_u32(n, "vendor-id", vendor);
	dt_prop_u32(n, "device-id", device);
	dt_prop_u32(n, "revision-id", rev);
	dt_prop_u32(n, "class-code", class_code);
	dt_prop_u32(n, "min-grant", 0);
	dt_prop_u32(n, "max-latency", 0);
	dt_prop_u32(n, "devsel-speed", 0);
	dt_prop_u32(n, "cache-line-size", 0);
}

/* "reg" and "assigned-addresses" for a function with one 32-bit memory BAR. */
static void pci_bar0(struct dt_node *n, uint32_t devfn, uint32_t base, uint32_t size)
{
	const uint32_t reg[10] = {
		PCI_SPACE_CONFIG | devfn << 8, 0, 0, 0, 0,
		PCI_SPACE_MEM32 | devfn << 8 | PCI_BAR0, 0, 0, 0, size,
	};
	const uint32_t assigned[5] = { PCI_NON_RELOCATABLE | PCI_SPACE_MEM32 | devfn << 8 | PCI_BAR0, 0, base, 0, size };
	dt_prop_cells(n, "reg", reg, 10);
	dt_prop_cells(n, "assigned-addresses", assigned, 5);
	dt_prop_u32(n, "AAPL,address", base);
}

static struct dt_node *build_tree(const struct bootinfo_machine *hw, unsigned *parcel_props)
{
	static const char compatible[] = "iMac,1\0MacRISC\0Power Macintosh";
	struct dt_node *root = dt_add_node(NULL, "device-tree");
	dt_prop_str(root, "model", "iMac,1");
	dt_prop(root, "compatible", compatible, sizeof compatible);
	dt_prop_u32(root, "#address-cells", 1);
	dt_prop_u32(root, "#size-cells", 1);
	dt_prop_u32(root, "clock-frequency", hw->bus_hz);
	dt_prop_str(root, "copyright", "Copyright 1983-1998 Apple Computer, Inc. THIS MESSAGE FOR COMPATIBILITY ONLY");

	struct dt_node *cpus = dt_add_node(root, "cpus");
	dt_prop_u32(cpus, "#address-cells", 1);
	dt_prop_u32(cpus, "#size-cells", 0);
	struct dt_node *cpu = dt_add_node(cpus, "PowerPC,750");
	dt_prop_str(cpu, "device_type", "cpu");
	dt_prop_u32(cpu, "reg", 0);
	dt_prop_u32(cpu, "cpu-version", hw->pvr);
	dt_prop_u32(cpu, "clock-frequency", hw->cpu_hz);
	dt_prop_u32(cpu, "bus-frequency", hw->bus_hz);
	dt_prop_u32(cpu, "timebase-frequency", hw->timebase_hz);
	dt_prop_u32(cpu, "d-cache-size", 0x8000);
	dt_prop_u32(cpu, "i-cache-size", 0x8000);
	dt_prop_u32(cpu, "d-cache-sets", 128);
	dt_prop_u32(cpu, "i-cache-sets", 128);
	dt_prop_u32(cpu, "d-cache-block-size", 32);
	dt_prop_u32(cpu, "i-cache-block-size", 32);
	dt_prop_u32(cpu, "tlb-sets", 64);
	dt_prop_u32(cpu, "tlb-size", 128);
	dt_prop_u32(cpu, "reservation-granule-size", 32);
	dt_prop_str(cpu, "state", "running");

	struct dt_node *memory = dt_add_node(root, "memory");
	const uint32_t mem_reg[2] = { 0, hw->ram_size };
	dt_prop_str(memory, "device_type", "memory");
	dt_prop_cells(memory, "reg", mem_reg, 2);

	/* Host bridge: Grackle (MPC106), as in the iMac and the beige/B&W G3 */
	struct dt_node *pci = dt_add_node(root, "pci");
	pci_ids(pci, PCI_VENDOR_MOTOROLA, PCI_DEVICE_GRACKLE, PCI_CLASS_HOST_BRIDGE, 0x40);
	dt_prop_str(pci, "device_type", "pci");
	dt_prop_str(pci, "model", "MOT,MPC106");
	dt_prop_str(pci, "compatible", "grackle");
	dt_prop_u32(pci, "#address-cells", 3);
	dt_prop_u32(pci, "#size-cells", 2);
	dt_prop_u32(pci, "#interrupt-cells", 1);
	const uint32_t pci_reg[2] = { 0x80000000u, 0x7F000000u };
	const uint32_t pci_ranges[12] = {
		PCI_SPACE_IO, 0, 0, 0xFE000000u, 0, 0x00800000u,
		PCI_SPACE_MEM32, 0, 0x80000000u, 0x80000000u, 0, 0x7D000000u,
	};
	const uint32_t bus_range[2] = { 0, 0 };
	dt_prop_cells(pci, "reg", pci_reg, 2);
	dt_prop_cells(pci, "ranges", pci_ranges, 12);
	dt_prop_cells(pci, "bus-range", bus_range, 2);
	dt_prop_u32(pci, "clock-frequency", 33333333);

	/* The display: a plain framebuffer whose Mac OS driver is in its node */
	struct dt_node *disp = dt_add_node(pci, "display");
	pci_ids(disp, PCI_VENDOR_SYSHM, PCI_DEVICE_DISPLAY, PCI_CLASS_DISPLAY, 1);
	dt_prop_str(disp, "device_type", "display");
	dt_prop_str(disp, "model", "syshm framebuffer");
	dt_prop_str(disp, "compatible", "syshm,display");
	pci_bar0(disp, PCI_DEVFN_DISPLAY, hw->fb_base, hw->fb_size);
	dt_prop_u32(disp, "width", hw->fb_width);
	dt_prop_u32(disp, "height", hw->fb_height);
	dt_prop_u32(disp, "depth", hw->fb_depth);
	dt_prop_u32(disp, "linebytes", hw->fb_row_bytes);
	dt_prop_u32(disp, "address", hw->fb_base);
	if (hw->display_ndrv_size)
		dt_prop(disp, "driver,AAPL,MacOS,PowerPC", hw->display_ndrv, (uint32_t)hw->display_ndrv_size);

	/* Block storage backed by host files */
	for (unsigned k = 0; k < hw->disks && k < BOOTINFO_MAX_DISKS; k++) {
		struct dt_node *disk = dt_add_node(pci, "syshm,disk");
		const uint32_t reg[5] = { PCI_SPACE_CONFIG | (PCI_DEVFN_DISK + 8 * k) << 8, 0, 0, 0, 0 };
		pci_ids(disk, PCI_VENDOR_SYSHM, PCI_DEVICE_DISK, PCI_CLASS_DISK, 1);
		dt_prop_str(disk, "device_type", "block");
		dt_prop_str(disk, "compatible", "syshm,disk");
		dt_prop_cells(disk, "reg", reg, 5);
		dt_prop(disk, "driver,AAPL,MacOS,PowerPC", hw->disk_ndrv[k], (uint32_t)hw->disk_ndrv_size[k]);
	}
	if (hw->floppy_ndrv_size) {
		struct dt_node *floppy = dt_add_node(pci, "syshm,floppy");
		const uint32_t reg[5] = { PCI_SPACE_CONFIG | PCI_DEVFN_FLOPPY << 8, 0, 0, 0, 0 };
		pci_ids(floppy, PCI_VENDOR_SYSHM, PCI_DEVICE_FLOPPY, PCI_CLASS_DISK, 1);
		dt_prop_str(floppy, "device_type", "block");
		dt_prop_str(floppy, "compatible", "syshm,floppy");
		dt_prop_cells(floppy, "reg", reg, 5);
		dt_prop(floppy, "driver,AAPL,MacOS,PowerPC", hw->floppy_ndrv, (uint32_t)hw->floppy_ndrv_size);
	}

	/* USB: an OHCI controller with nothing plugged in, which the ROM drives itself (see ohci.h) */
	static const char usb_compatible[] = "pci1045,c861\0pciclass,0c0310";
	struct dt_node *usb = dt_add_node(pci, "usb");
	pci_ids(usb, OHCI_PCI_VENDOR, OHCI_PCI_DEVICE, OHCI_PCI_CLASS, OHCI_PCI_REVISION);
	dt_prop_str(usb, "device_type", "usb");
	dt_prop(usb, "compatible", usb_compatible, sizeof usb_compatible);
	pci_bar0(usb, PCI_DEVFN_USB, USB_BASE, OHCI_SIZE);
	const uint32_t usb_irq = IRQ_USB;
	dt_prop_u32(usb, "interrupts", IRQ_USB);
	interrupt_set(usb, &usb_irq, 1);
	dt_prop_u32(usb, "#address-cells", 1);
	dt_prop_u32(usb, "#size-cells", 0);

	/* Mac I/O (Heathrow-compatible) and the devices inside it */
	struct dt_node *mio = dt_add_node(pci, "mac-io");
	pci_ids(mio, PCI_VENDOR_APPLE, PCI_DEVICE_HEATHROW, PCI_CLASS_MACIO, 0);
	dt_prop_str(mio, "device_type", "mac-io");
	dt_prop_str(mio, "model", "AAPL,343S1201");
	dt_prop_str(mio, "compatible", "heathrow");
	dt_prop_u32(mio, "#address-cells", 1);
	dt_prop_u32(mio, "#size-cells", 1);
	dt_prop_u32(mio, "#interrupt-cells", 1);
	pci_bar0(mio, PCI_DEVFN_MACIO, MACIO_BASE, MACIO_SIZE);
	const uint32_t mio_ranges[5] = { 0, PCI_SPACE_MEM32 | PCI_DEVFN_MACIO << 8 | PCI_BAR0, 0, MACIO_BASE, MACIO_SIZE };
	dt_prop_cells(mio, "ranges", mio_ranges, 5);

	struct dt_node *ic = dt_add_node(mio, "interrupt-controller");
	const uint32_t ic_reg[2] = { MACIO_INTCTL - MACIO_BASE, 0x20 };
	dt_prop_str(ic, "device_type", "interrupt-controller");
	dt_prop_str(ic, "compatible", "heathrow");
	dt_prop_cells(ic, "reg", ic_reg, 2);
	dt_prop_empty(ic, "interrupt-controller");
	dt_prop_u32(ic, "#interrupt-cells", 1);

	struct dt_node *cuda = dt_add_node(mio, "via-cuda");
	const uint32_t cuda_reg[2] = { MACIO_VIA1 - MACIO_BASE, 0x2000 };
	dt_prop_str(cuda, "device_type", "via-cuda");
	dt_prop_str(cuda, "compatible", "cuda");
	dt_prop_cells(cuda, "reg", cuda_reg, 2);
	dt_prop_u32(cuda, "AAPL,interrupts", IRQ_VIA1);
	dt_prop_u32(cuda, "interrupts", IRQ_VIA1);
	dt_prop_u32(cuda, "#address-cells", 1);
	dt_prop_u32(cuda, "#size-cells", 0);
	struct dt_node *adb = dt_add_node(cuda, "adb");
	dt_prop_str(adb, "device_type", "adb");
	dt_prop_str(adb, "compatible", "adb");
	dt_prop_u32(adb, "#address-cells", 1);
	dt_prop_u32(adb, "#size-cells", 0);
	struct dt_node *rtc = dt_add_node(cuda, "rtc");
	dt_prop_str(rtc, "device_type", "rtc");
	dt_prop_str(rtc, "compatible", "rtc");
	struct dt_node *pm = dt_add_node(cuda, "power-mgt");
	dt_prop_str(pm, "device_type", "power-mgt");
	dt_prop_str(pm, "compatible", "cuda");
	dt_prop_str(pm, "mgt-kind", "min-consumption-pwm-led");

	/*
	 * Sound: the codec's registers and its two DMA channels, found by Mac OS's
	 * .DAVAudio driver. Mac OS 8.5's takes the interrupt sources from
	 * AAPL,interrupt-vectors; Mac OS 9's sound plug-in wants an interrupt set.
	 */
	struct dt_node *dav = dt_add_node(mio, "davbus");
	const uint32_t dav_reg[6] = { MACIO_SOUND - MACIO_BASE, 0x1000,
				      MACIO_DMA_SOUND_OUT - MACIO_BASE, 0x100, MACIO_DMA_SOUND_IN - MACIO_BASE, 0x100 };
	const uint32_t dav_addr[3] = { MACIO_SOUND, MACIO_DMA_SOUND_OUT, MACIO_DMA_SOUND_IN };
	const uint32_t dav_irq[3] = { IRQ_SOUND, IRQ_DMA_SOUND_OUT, IRQ_DMA_SOUND_IN };
	dt_prop_str(dav, "device_type", "soundbus");
	dt_prop_str(dav, "compatible", "davbus");
	dt_prop_cells(dav, "reg", dav_reg, 6);
	dt_prop_cells(dav, "interrupts", dav_irq, 3);
	dt_prop_cells(dav, "AAPL,address", dav_addr, 3);
	interrupt_set(dav, dav_irq, 3);
	dt_prop_u32(dav, "#address-cells", 1);
	dt_prop_u32(dav, "#size-cells", 0);
	struct dt_node *sound = dt_add_node(dav, "sound");
	dt_prop_str(sound, "device_type", "sound");
	dt_prop_str(sound, "compatible", "awacs");
	dt_prop_u32(sound, "sub-frame", 0);
	dt_prop_u32(sound, "vendor-id", 0x106B);        /* Apple's sound port layout 1 (Mac OS's sound */
	dt_prop_u32(sound, "device-id", 1);             /* component picks its ports by these) */
	/*
	 * Output only (bit 0 would be input, which only records silence here).
	 * Mac OS 9's AWACS input component claims chips with input, and its close
	 * after the Component Manager's register call corrupts the system heap
	 * whenever the heap is laid out a certain way: it unlocks its globals, then
	 * passes a pointer into them to a call that allocates memory (which can
	 * move them) and writes through it afterwards. Startup then crashes or hangs.
	 */
	dt_prop_u32(sound, "hardware-types", 2);

	/*
	 * Ethernet, as on the iMac: Paddington's "bmac+", with the PowerPC code
	 * fragment Mac OS's driver needs for it (lanLib; see net.h), which New
	 * World Macs' firmware supplies.
	 */
	if (hw->lanlib_size) {
		struct dt_node *enet = dt_add_node(mio, "ethernet");
		const uint32_t enet_reg[6] = { MACIO_ENET - MACIO_BASE, 0x1000, MACIO_DMA_ENET_TX - MACIO_BASE, 0x100,
					       MACIO_DMA_ENET_RX - MACIO_BASE, 0x100 };
		const uint32_t enet_addr[3] = { MACIO_ENET, MACIO_DMA_ENET_TX, MACIO_DMA_ENET_RX };
		const uint32_t enet_irq[3] = { IRQ_ENET, IRQ_DMA_ENET_TX, IRQ_DMA_ENET_RX };
		/*
		 * An interrupt set with just the receive DMA channel's (lanLib's;
		 * see net.h). One for all three made Open Transport stall for half
		 * a minute or more in some startups of Mac OS 9; why isn't known.
		 */
		const uint32_t enet_rx_irq = IRQ_DMA_ENET_RX;
		dt_prop_str(enet, "device_type", "network");
		dt_prop_str(enet, "compatible", "bmac+");
		dt_prop_cells(enet, "reg", enet_reg, 6);
		dt_prop_cells(enet, "interrupts", enet_irq, 3);
		dt_prop_cells(enet, "AAPL,address", enet_addr, 3);
		interrupt_set(enet, &enet_rx_irq, 1);
		dt_prop(enet, "local-mac-address", hw->mac, 6);
		dt_prop(enet, "lanLib,AAPL,MacOS,PowerPC", hw->lanlib, (uint32_t)hw->lanlib_size);
	}

	/*
	 * NVRAM, as the iMac has it, for ROM files with parcels: their NVRAM
	 * services drive the chip (with the parcel's driver for an nvram under
	 * mac-io, which takes its address from AAPL,address). Earlier ROMs reach
	 * NVRAM through RTAS.
	 */
	if (hw->parcels) {
		struct dt_node *nvram = dt_add_node(mio, "nvram");
		const uint32_t nvram_reg[2] = { MACIO_NVRAM - MACIO_BASE, MACIO_NVRAM_SIZE };
		dt_prop_str(nvram, "device_type", "nvram");
		dt_prop_str(nvram, "compatible", "nvram");
		dt_prop_cells(nvram, "reg", nvram_reg, 2);
		dt_prop_u32(nvram, "#bytes", MACIO_NVRAM_SIZE / NVRAM_STRIDE);
		dt_prop_u32(nvram, "AAPL,address", MACIO_NVRAM);
	}

	dt_add_node(root, "rtas");

	/*
	 * Where Open Firmware booted from: the device, the partition (its map
	 * entry) and the file (the blessed folder's of type tbxi, the ROM file).
	 * Until the internal disk's wait time (PRAM; 25 s if unset) has passed,
	 * Mac OS 9.1's Start Manager starts up only from the drive StartLib
	 * finds for it; the disk driver says which that is (see disk.c).
	 */
	if (hw->parcels && hw->boot && hw->boot_disk < hw->disks) {
		char path[64];
		unsigned dev = (PCI_DEVFN_DISK + 8 * hw->boot_disk) >> 3;
		if (hw->boot_map_entry)
			snprintf(path, sizeof path, "/pci/syshm,disk@%x:%u,\\\\:tbxi", dev, hw->boot_map_entry);
		else
			snprintf(path, sizeof path, "/pci/syshm,disk@%x:,\\\\:tbxi", dev);
		dt_prop_str(dt_add_node(root, "chosen"), "bootpath", path);
	}

	/*
	 * A ROM file with parcels: the /rom/macos node its boot script makes, and
	 * the nodes and properties the Trampoline adds from the parcels.
	 */
	if (hw->parcels) {
		dt_add_node(dt_add_node(root, "rom"), "macos");
		*parcel_props = parcels_apply(hw->parcels, root);
	}
	return root;
}

int bootinfo_build(struct mem *mem, struct bootinfo *bi, const struct bootinfo_machine *hw)
{
	bi->device_tree = BOOTINFO_BASE + DEVICE_TREE_OFFSET;
	bi->product_info = BOOTINFO_BASE + PRODUCT_INFO_OFFSET;
	bi->vector_mask_table = BOOTINFO_BASE + VECTOR_MASK_OFFSET;
	bi->vector_lookup_table = BOOTINFO_BASE + VECTOR_LOOKUP_OFFSET;
	bi->parcel_props = 0;

	struct dt_node *tree = build_tree(hw, &bi->parcel_props);
	bi->device_tree_size = (uint32_t)dt_flatten(tree, mem_ptr(mem, bi->device_tree), DEVICE_TREE_SIZE);
	if (!bi->device_tree_size) {
		bi->device_tree = BOOTINFO_BASE + BIG_DEVICE_TREE_OFFSET;
		bi->device_tree_size = (uint32_t)dt_flatten(tree, mem_ptr(mem, bi->device_tree), BIG_DEVICE_TREE_SIZE);
	}
	dt_free(tree);
	if (!bi->device_tree_size) {
		fprintf(stderr, "bootinfo: device tree doesn't fit in %u bytes\n", BIG_DEVICE_TREE_SIZE);
		return -1;
	}

	uint8_t *pi = mem_ptr(mem, bi->product_info);
	uint8_t *di = pi + PI_SIZE;
	memset(pi, 0, PI_SIZE + DI_SIZE);

	const uint32_t bases_valid = 1u << (DI_VIA1 / 4) | 1u << (DI_SCC_READ / 4) | 1u << (DI_SCC_WRITE / 4);
	const uint32_t ext_valid = 1u << (DI_AWACS / 4 - 32) | 1u << (DI_MACIO / 4 - 32) | 1u << (DI_FLAGS_F8 / 4 - 32);

	/*
	 * Flags at +0x24: bits 24-26 select the ADB/power manager (3 = Cuda via VIA1).
	 * We present none; the 1.x ROMs' clock, NVRAM and power code goes through RTAS
	 * kernel calls instead. (In a ROM file with parcels, the clock and NVRAM code
	 * calls libraries, which use drivers for devices in the tree: the Cuda's RTC and
	 * the nvram node.)
	 */
	const uint32_t flags24 = 0x00000006;

	put_be32(pi + PI_DECODER_INFO_PTR, PI_SIZE);
	put_be16(pi + PI_HW_CFG_WORD, 0x0480);
	pi[PI_PRODUCT_KIND] = 0x00;
	pi[PI_DECODER_KIND] = 0x1A;
	pi[PI_VERSION] = 2;
	put_be32(pi + PI_BASES_VALID, bases_valid);
	put_be32(pi + PI_EXT_VALID, ext_valid);
	put_be32(pi + PI_FLAGS_24, flags24);
	/*
	 * (bit 26: USB keyboards and mice reach the ADB Manager through the ROM's
	 * USB shims, as on every New World Mac; without it the shims, loaded once
	 * the USB controller is found, call a hook the ADB Manager never set.
	 * Bit 19: restart through RTAS system-reboot; the ROM's alternative
	 * restarts in place, which devices left running defeat.)
	 */
	put_be32(pi + PI_FLAGS_28, 0x0788080E);

	/* Private copy of the flags just below DecoderInfo (the ROM ORs into DI-0x1C..-0x14) */
	put_be32(di - 0x28, bases_valid);
	put_be32(di - 0x24, ext_valid);
	put_be32(di - 0x1C, flags24);
	put_be32(di - 0x18, 0x0180000E);
	di[-8] = 0x1A;                  /* decoder kind again, and a flag the ROM tests before */
	di[-7] = 0x01;                  /* installing SCSI Manager 4.3 (as the real Trampoline sets) */

	put_be32(di + DI_ROM, 0xFFC00000u);
	put_be32(di + DI_VIA1, MACIO_VIA1);
	put_be32(di + DI_SCC_READ, MACIO_SCC);
	put_be32(di + DI_SCC_WRITE, MACIO_SCC);
	put_be32(di + DI_AWACS, MACIO_SOUND);
	put_be32(di + DI_MACIO, MACIO_BASE);
	put_be32(di + DI_FLAGS_F8, 0x00010100);

	/* Interrupt tables: per-level source masks (bank 0 words, then bank 1),
	 * and 64 dispatch slots -> source numbers */
	uint32_t masks[2][8];
	bootinfo_level_masks(masks);
	for (uint32_t k = 0; k < 8; k++) {
		mem_w32(mem, bi->vector_mask_table + 4 * k, masks[0][k]);
		mem_w32(mem, bi->vector_mask_table + 0x20 + 4 * k, masks[1][k]);
	}
	for (uint32_t k = 0; k < 64; k++)
		mem_w16(mem, bi->vector_lookup_table + 2 * k, (uint16_t)k);
	return 0;
}

void bootinfo_level_masks(uint32_t masks[2][8])
{
	memset(masks, 0, sizeof(uint32_t[2][8]));
	masks[0][1] |= 0x80000000u >> IRQ_VIA1;                 /* VIA1 (Cuda, timers): level 1 */
	masks[0][2] |= 0x80000000u >> IRQ_SOUND | 0x80000000u >> IRQ_DMA_SOUND_OUT |
		       0x80000000u >> IRQ_DMA_SOUND_IN;        /* sound: level 2 */
	masks[0][2] |= 0x80000000u >> IRQ_ENET | 0x80000000u >> IRQ_DMA_ENET_TX |
		       0x80000000u >> IRQ_DMA_ENET_RX;         /* Ethernet: level 2 */
	masks[0][2] |= 0x80000000u >> IRQ_USB;                  /* USB: level 2 */
	masks[0][4] |= 0x80000000u >> IRQ_SCC_A | 0x80000000u >> IRQ_SCC_B;    /* SCC: level 4 */
}

void bootinfo_hwinfo(const struct bootinfo *bi, uint8_t *hw)
{
	memset(hw, 0, 0xA0);
	put_be32(hw + 0x00, ROM_PHYS_BASE);             /* MacROM_Base */
	put_be32(hw + 0x04, bi->device_tree);           /* DeviceTreeBase */
	put_be32(hw + 0x08, bi->product_info);          /* UniversalInfoTableBase */
	put_be32(hw + 0x0C, 0x00003000);                /* ConfigInfoTableBase (physical) */
	put_be32(hw + 0x10, bi->vector_lookup_table);   /* VectorLookupTable */
	put_be32(hw + 0x14, bi->vector_mask_table);     /* VectorMaskTable */
	put_be32(hw + 0x3C, 0x1400);                    /* MacOS_NVRAM_Offset */
	put_be16(hw + 0x6C, VECTOR_NONE);               /* SIOIntVect */
	put_be16(hw + 0x6E, VECTOR_BIT_NONE);           /* SIOIntBit */
	put_be32(hw + 0x70, 0x486E666F);                /* Signature 'Hnfo' */
	put_be16(hw + 0x74, 0);                         /* SpuriousIntVect */
	put_be16(hw + 0x76, 0);                         /* CPU_ID */
	put_be16(hw + 0x78, IRQ_SCC_A);                 /* SCCAIntVect */
	put_be16(hw + 0x7A, IRQ_SCC_B);                 /* SCCBIntVect */
	put_be16(hw + 0x7C, VECTOR_NONE);               /* SCSIIntVect */
	put_be16(hw + 0x7E, VECTOR_NONE);               /* SCSIDMAIntVect */
	put_be16(hw + 0x80, IRQ_VIA1);                  /* VIAIntVect */
	put_be16(hw + 0x82, VECTOR_BIT_NONE);           /* VIAIntBit */
	put_be16(hw + 0x84, VECTOR_NONE);               /* ADBIntVect */
	put_be16(hw + 0x86, VECTOR_NONE);               /* NMIIntVect */
	put_be16(hw + 0x88, VECTOR_BIT_NONE);           /* NMIIntBit */
	put_be32(hw + 0x94, 4);                         /* HardwareInfoFlags: NoCopySWTLB */

	/* RTAS tokens: services the native kernel provides through KCall 6 */
	for (int t = 1; t < RTAS_TOKEN_COUNT; t++)
		put_be32(hw + rtas_token_field[t], (uint32_t)t);
}
