/*
 * Open Firmware's settings in NVRAM, as New World Macs keep them: NVRAM is
 * cut into CHRP partitions, each a 16-byte header (signature, checksum,
 * length in 16-byte blocks counting the header, a 12-byte name) and its
 * data; "common" (signature 0x70) holds Open Firmware's variables as
 * "name=value" strings, each ended by a NUL. Mac OS 9.1 and later write
 * there the properties of the device tree's /options they keep in NVRAM:
 * boot-device, which the Startup Disk control panel sets (StartLib's
 * SetSelectedStartupDevice), such as "/pci@80000000/blue,disk@3@3:8,\\:tbxi".
 */
#ifndef BLUE_OFNVRAM_H
#define BLUE_OFNVRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OFNVRAM_COMMON_SIG 0x70u
#define OFNVRAM_COMMON_SIZE 0x800u      /* (Blue's: NVRAM's first 2 KB, which Mac OS leaves alone) */
#define OFNVRAM_VALUE_MAX 256u

/* The "common" partition's data: its offset and length. False if there's none (or the partitions are broken). */
bool ofnvram_common(const uint8_t *nv, size_t size, size_t *data, size_t *len);

/*
 * Makes an empty "common" partition at the start of NVRAM if there's none
 * and those bytes are blank (all zero), as before Open Firmware ever wrote
 * them. Returns whether it did.
 */
bool ofnvram_format(uint8_t *nv, size_t size);

/* A variable's value (NUL-ended, cut to `len`); false if it isn't set */
bool ofnvram_get(const uint8_t *nv, size_t size, const char *name, char *out, size_t len);

/*
 * An Open Firmware path to one of Blue's disks ("/pci@80000000/blue,disk@3@3:8,\\:tbxi",
 * as Mac OS writes boot-device, or "/pci/blue,disk@3:8,\\:tbxi", as Blue
 * gives /chosen's bootpath): the disk's PCI device number and the partition
 * (its map entry; 0 when the path names none). False for any other path.
 */
bool ofnvram_disk_path(const char *path, unsigned *pci_dev, unsigned *map_entry);

#endif
