#include <string.h>

#include "test.h"

int test_failures;

int main(int argc, char **argv)
{
	struct {
		const char *name;
		void (*fn)(void);
	} suites[] = {
		{ "lzss", test_lzss },
		{ "parcels", test_parcels },
		{ "ppc", test_ppc },
		{ "rom", test_rom },
		{ "cuda", test_cuda },
		{ "mmio", test_mmio },
		{ "dbdma", test_dbdma },
		{ "awacs", test_awacs },
		{ "cdimage", test_cdimage },
		{ "cdaudio", test_cdaudio },
		{ "ohci", test_ohci },
		{ "drivers", test_drivers },
		{ "net", test_net },
		{ "hfs", test_hfs },
		{ "hfsread", test_hfsread },
		{ "floppy", test_floppy },
		{ "install", test_install },
		{ "share", test_share },
		{ "tools", test_tools },
		{ "clip", test_clip },
		{ "raster", test_raster },
		{ "rave", test_rave },
		{ "gpu", test_gpu },
		{ "display", test_display },
		{ "video", test_video },
		{ "jit", test_jit },
		{ "mp", test_mp },
		{ "nvram", test_nvram },
	};

	for (size_t s = 0; s < sizeof suites / sizeof suites[0]; s++) {
		if (argc > 1 && strcmp(argv[1], suites[s].name) != 0)
			continue;
		int before = test_failures;
		suites[s].fn();
		printf("%-6s %s\n", suites[s].name, test_failures == before ? "ok" : "FAILED");
	}
	return test_failures ? 1 : 0;
}
