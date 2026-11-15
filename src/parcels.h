/*
 * Parcels: how "Mac OS ROM" files from Mac OS 9.1 on hold their contents. A
 * chain of records (magic 'prcl'), each of a type and with blobs (stored or
 * LZSS-compressed):
 *
 *   'rom '  the 4 MB Toolbox image
 *   'node'  a new device-tree node: AAPL,CodePrepare and AAPL,CodeRegister
 *           hold libraries the ROM prepares and registers at startup
 *           (MotherBoardHAL, TimeManagerLib, StartLib, ...), which earlier
 *           ROMs had inside the image
 *   'prop'  properties for existing nodes that match: drivers and libraries
 *           for each machine's devices
 *   'psum'  which nodes and properties go into a checksum of the tree
 *
 * The Trampoline puts the nodes and properties in the device tree before it
 * starts the ROM; parcels_apply() does the same with ours. Layout and flags
 * after tbxi (github.com/elliotnunn/tbxi).
 */
#ifndef BLUE_PARCELS_H
#define BLUE_PARCELS_H

#include <stddef.h>
#include <stdint.h>

#include "devtree.h"

/* Parcel flags */
#define PARCEL_SPECIAL 0xF0000u         /* ('node') the number blobs refer to it by */
#define PARCEL_BOOT_DISK_ONLY 0x200u    /* only for a node the boot disk needs */
#define PARCEL_ONCE 0x10u               /* only the first node that matches */
#define PARCEL_MATCH_TYPE 0x8u          /* match: device_type is b, and ... */
#define PARCEL_MATCH_COMPATIBLE 0x4u    /* ... compatible lists a, */
#define PARCEL_MATCH_PARENT 0x2u        /* ... or the parent's name is a, */
#define PARCEL_MATCH_NAME 0x1u          /* ... or the name is a */

/* Blob flags */
#define BLOB_SPECIAL 0xF0000u           /* with BLOB_IN_SPECIAL: the special node it goes in */
#define BLOB_DEBUG_ONLY 0x100u          /* for debugging startup only */
#define BLOB_IN_SPECIAL 0x80u
#define BLOB_DELETE 0x40u               /* remove the property instead */
#define BLOB_KEEP_EXISTING 0x20u        /* don't replace one already there */
#define BLOB_ONCE 0x10u

struct parcel_blob {
	char type[5];
	uint32_t flags;
	char name[33];                  /* the property's name */
	uint8_t *data;                  /* decompressed */
	uint32_t size;
};

struct parcel {
	char type[5];
	uint32_t flags;
	char a[33], b[33];              /* what it matches ('prop') */
	struct parcel_blob *blobs;
	unsigned count;
};

struct parcels {
	struct parcel *list;
	unsigned count;
};

/* Whether data starts like a parcel chain. */
int parcels_detect(const uint8_t *data, size_t size);

/* Parses and decompresses a chain; returns 0, or -1 with a message in err. */
int parcels_parse(const uint8_t *data, size_t size, struct parcels *out, char *err, size_t errlen);
void parcels_free(struct parcels *p);

/* The first blob of type blob_type in a parcel of type parcel_type, or NULL. */
const struct parcel_blob *parcels_find(const struct parcels *p, const char *parcel_type, const char *blob_type);

/* Adds the 'node' parcels' nodes (under root) and the 'prop' parcels' properties to the tree;
 * returns how many properties it set or removed. */
unsigned parcels_apply(const struct parcels *p, struct dt_node *root);

#endif
