/*
 * PCI configuration space of the virtual machine's bus 0, reached the way
 * Grackle (MPC106) host bridges do it, which the ROM uses: the address goes
 * to CONFIG_ADDR (0x80000000 | bus << 16 | device << 11 | function << 8 |
 * register) and the data through CONFIG_DATA, both little-endian. RTAS's
 * read-pci-config and write-pci-config reach the same functions.
 *
 * Each function's header holds its IDs, class, command register and one
 * memory BAR that sizes like real hardware but stays where the device tree
 * put it. Absent functions read all ones.
 */
#ifndef SYSHM_PCI_H
#define SYSHM_PCI_H

#include <stdbool.h>
#include <stdint.h>

#include "mmio.h"

#define PCI_CONFIG_ADDR 0xFEC00000u
#define PCI_CONFIG_DATA 0xFEE00000u
#define PCI_CONFIG_SIZE 0x1000u         /* (each register's page) */
#define PCI_MAX_FUNCTIONS 12

struct pci_function {
	uint8_t devfn;
	uint8_t config[256];            /* the header (little-endian) */
	uint32_t bar_mask;              /* BAR0's writable bits (0: none) */
};

struct pci_bus {
	uint32_t address;               /* CONFIG_ADDR, as a number */
	struct pci_function fn[PCI_MAX_FUNCTIONS];
	unsigned count;
	bool log;                       /* print configuration accesses */
};

void pci_init(struct pci_bus *b);

/* Adds a function with one 32-bit memory BAR of `bar_size` bytes at `bar` (none if 0). */
void pci_add(struct pci_bus *b, uint8_t devfn, uint16_t vendor, uint16_t device, uint32_t class_code,
	     uint8_t revision, uint32_t bar, uint32_t bar_size);

/* Configuration accesses (`size` 1, 2 or 4 bytes, not crossing a dword; values as numbers). */
uint32_t pci_config_read(struct pci_bus *b, unsigned bus, unsigned devfn, unsigned reg, unsigned size);
void pci_config_write(struct pci_bus *b, unsigned bus, unsigned devfn, unsigned reg, unsigned size, uint32_t value);

/* CONFIG_ADDR and CONFIG_DATA */
extern const struct mmio_ops pci_addr_mmio_ops, pci_data_mmio_ops;

#endif
