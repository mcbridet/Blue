#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/nk.h"
#include "../src/rom.h"
#include "test.h"

/* Uses a real ROM if one is available: $SYSHM_TEST_ROM, or ./MOS.rom. */
void test_rom(void)
{
	const char *path = getenv("SYSHM_TEST_ROM");
	if (!path)
		path = "MOS.rom";
	if (access(path, R_OK) != 0) {
		printf("rom    skipped (no %s)\n", path);
		return;
	}

	struct rom rom;
	char err[256];
	if (rom_load(path, &rom, err, sizeof err)) {
		fprintf(stderr, "%s\n", err);
		CHECK(!"rom_load failed");
		return;
	}

	CHECK_EQ(rom_stored_checksum(&rom), rom_computed_checksum(&rom));
	CHECK_EQ(rom_version(&rom), 0x077D);

	char nk[40];
	rom_nanokernel_id(&rom, nk, sizeof nk);
	CHECK(nk[0] != '\0');
	if (rom.format == ROM_FORMAT_BOOTINFO || rom.format == ROM_FORMAT_PARCELS) {
		CHECK(strncmp(nk, "NewWorld", 8) == 0);
		CHECK(rom.compatible[0] != '\0');
		CHECK(rom.elf_entry != 0);
	}

	struct nk_config ci;
	CHECK(nk_config_parse(&rom, &ci, err, sizeof err) == 0);
	CHECK_EQ(ci.la_kernel_data, 0x68FFE000);
	CHECK_EQ(ci.la_emulator_data, 0x68FFF000);
	CHECK_EQ(ci.la_emulator_code, 0x68060000);
	CHECK_EQ(ci.la_info_record, 0x5FFFE000);

	struct nk_region regions[NK_MAX_REGIONS];
	int n = nk_user_regions(&rom, &ci, regions, NK_MAX_REGIONS);
	bool rom_at_top = false, emulator_data = false;
	for (int k = 0; k < n; k++) {
		rom_at_top |= regions[k].kind == NK_REGION_ROM && regions[k].logical == ROM_BASE &&
			      regions[k].size == ROM_IMAGE_SIZE && regions[k].rom_offset == 0;
		emulator_data |= regions[k].kind == NK_REGION_EMULATOR_DATA &&
				 regions[k].logical == ci.la_emulator_data;
	}
	CHECK(rom_at_top);
	CHECK(emulator_data);

	rom_free(&rom);
}
