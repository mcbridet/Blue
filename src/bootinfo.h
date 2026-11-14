/*
 * What Apple's Trampoline would leave behind for the NanoKernel and the 68k
 * ROM: the hardware info record, the dynamic universal tables (ProductInfo +
 * DecoderInfo) describing the machine's I/O, and the interrupt vector tables.
 * Layouts and values were taken from the real Trampoline's output in the QEMU
 * reference run (tools/qemu-ref) for a Heathrow/Paddington-class Mac I/O.
 */
#ifndef SYSHM_BOOTINFO_H
#define SYSHM_BOOTINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "parcels.h"

/* The boot info area: the logical "Open Firmware" range in ConfigInfo's map. */
#define BOOTINFO_BASE 0xFF800000u
#define BOOTINFO_SIZE 0x00400000u

/* Virtual machine I/O layout (Mac I/O cell base and the devices inside it) */
#define MACIO_BASE 0x81080000u
#define MACIO_INTCTL (MACIO_BASE + 0x10u)      /* Heathrow-style interrupt controller */
#define MACIO_VIA1 (MACIO_BASE + 0x16000u)
#define MACIO_SCC (MACIO_BASE + 0x12000u)
#define MACIO_DBDMA (MACIO_BASE + 0x8000u)     /* DMA channel registers */
#define MACIO_SOUND (MACIO_BASE + 0x14000u)    /* AWACS-family sound chip ("davbus") */
#define MACIO_ENET (MACIO_BASE + 0x11000u)     /* Ethernet ("bmac+"; lanLib drives it) */
#define MACIO_NVRAM (MACIO_BASE + 0x60000u)    /* NVRAM, a byte every 16 (ROM files with parcels only) */
#define MACIO_NVRAM_SIZE 0x20000u
#define MACIO_DMA_ENET_TX (MACIO_DBDMA + 2 * 0x100u)
#define MACIO_DMA_ENET_RX (MACIO_DBDMA + 3 * 0x100u)
#define MACIO_DMA_SOUND_OUT (MACIO_DBDMA + 8 * 0x100u)
#define MACIO_DMA_SOUND_IN (MACIO_DBDMA + 9 * 0x100u)

/* Interrupt sources (Heathrow bit numbers) */
#define IRQ_DMA_ENET_TX 2u
#define IRQ_DMA_ENET_RX 3u
#define IRQ_ENET 14u
#define IRQ_DMA_SOUND_OUT 8u
#define IRQ_DMA_SOUND_IN 9u
#define IRQ_SCC_A 15u
#define IRQ_SCC_B 16u
#define IRQ_SOUND 17u
#define IRQ_VIA1 18u
#define IRQ_USB 28u

/* The USB controller's registers (PCI memory, below Mac I/O) */
#define USB_BASE 0x81000000u

/* Physical ROM base reported to the system (where the Trampoline copies the ROM). */
#define ROM_PHYS_BASE 0x00C00000u

/* PCI addresses of the virtual devices (bus 0, Grackle host bridge) */
#define PCI_DEVFN_DISPLAY 0x08u         /* device 1 */
#define PCI_DEVFN_DISK 0x10u            /* devices 2..: one per disk */
#define BOOTINFO_MAX_DISKS 4
#define PCI_DEVFN_USB 0x30u             /* device 6 */
#define PCI_DEVFN_FLOPPY 0x38u          /* device 7: the floppy drive, if there's one */
#define PCI_DEVFN_MACIO 0x80u           /* device 16 */
#define PCI_VENDOR_SYSHM 0x1234u        /* the "unassigned" ID virtual hardware commonly uses */
#define PCI_DEVICE_DISPLAY 0x5348u
#define PCI_DEVICE_DISK 0x5349u
#define PCI_DEVICE_FLOPPY 0x534Au
#define PCI_VENDOR_MOTOROLA 0x1057u     /* the host bridge: Grackle (MPC106) */
#define PCI_DEVICE_GRACKLE 0x0002u
#define PCI_VENDOR_APPLE 0x106Bu        /* Mac I/O: Heathrow */
#define PCI_DEVICE_HEATHROW 0x0010u
#define PCI_CLASS_HOST_BRIDGE 0x060000u
#define PCI_CLASS_DISPLAY 0x030000u
#define PCI_CLASS_DISK 0x018000u
#define PCI_CLASS_MACIO 0xFF0000u
#define MACIO_SIZE 0x80000u

/* What the device tree describes about the virtual machine */
struct bootinfo_machine {
	uint32_t ram_size;
	uint32_t pvr, cpu_hz, bus_hz, timebase_hz;
	uint32_t fb_base, fb_size;      /* display: framebuffer and its boot mode */
	uint32_t fb_width, fb_height, fb_depth, fb_row_bytes;
	const uint8_t *display_ndrv;    /* driver,AAPL,MacOS,PowerPC */
	size_t display_ndrv_size;
	unsigned disks;                 /* block devices, each with its own driver */
	const uint8_t *disk_ndrv[BOOTINFO_MAX_DISKS];
	size_t disk_ndrv_size[BOOTINFO_MAX_DISKS];
	const uint8_t *floppy_ndrv;     /* the floppy drive's driver (none: no floppy drive) */
	size_t floppy_ndrv_size;
	const uint8_t *lanlib;          /* Ethernet: lanLib,AAPL,MacOS,PowerPC (none: no Ethernet) */
	size_t lanlib_size;
	uint8_t mac[6];
	const struct parcels *parcels;  /* the ROM file's nodes and properties (Mac OS 9.1 on), or NULL */
	/* With parcels: where Open Firmware booted from, a disk and its partition's map entry (0: no map) */
	bool boot;
	unsigned boot_disk, boot_map_entry;
};

struct bootinfo {
	uint32_t device_tree;           /* DeviceTreeBase */
	uint32_t device_tree_size;      /* bytes used by the flattened tree */
	uint32_t product_info;          /* UniversalInfoTableBase */
	uint32_t vector_mask_table;
	uint32_t vector_lookup_table;
	unsigned parcel_props;          /* properties set (or removed) from the ROM file's parcels */
};

/* RTAS services and the tokens we hand out for them (0 is never a valid token) */
enum rtas_token {
	RTAS_NVRAM_FETCH = 1,
	RTAS_NVRAM_STORE,
	RTAS_GET_TIME_OF_DAY,
	RTAS_SET_TIME_OF_DAY,
	RTAS_SYSTEM_REBOOT,
	RTAS_POWER_OFF,
	RTAS_SET_TIME_FOR_POWER_ON,
	RTAS_EVENT_SCAN,
	RTAS_CHECK_EXCEPTION,
	RTAS_READ_PCI_CONFIG,
	RTAS_WRITE_PCI_CONFIG,
	RTAS_GET_TIME_FOR_POWER_ON,
	RTAS_TOKEN_COUNT,
};

/* Builds the tables in the boot info area (which must be mapped RW). Returns 0 on success. */
int bootinfo_build(struct mem *mem, struct bootinfo *bi, const struct bootinfo_machine *hw);

/*
 * 68k interrupt level of each source: masks[bank][level], bank 0 = sources
 * 0-31, bank 1 = 32-63, source n at bit 0x80000000 >> (n % 32).
 */
void bootinfo_level_masks(uint32_t masks[2][8]);

/* Fills the NKHWInfo record (0xA0 bytes) that the NanoKernel copies from the Trampoline. */
void bootinfo_hwinfo(const struct bootinfo *bi, uint8_t *hw);

#endif
