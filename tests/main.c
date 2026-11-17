#include <string.h>

#include "../src/env.h"
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
		{ "scc", test_scc },
		{ "modem", test_modem },
		{ "burgundy", test_burgundy },
		{ "audio", test_audio },
		{ "mic", test_mic },
		{ "cdimage", test_cdimage },
		{ "cdaudio", test_cdaudio },
		{ "cdformats", test_cdformats },
		{ "dmg", test_dmg },
		{ "ohci", test_ohci },
		{ "usb", test_usb },
		{ "usbhost", test_usbhost },
		{ "usbremote", test_usbremote },
		{ "drivers", test_drivers },
		{ "net", test_net },
		{ "hfs", test_hfs },
		{ "hfsread", test_hfsread },
		{ "floppy", test_floppy },
		{ "drives", test_drives },
		{ "chooser", test_chooser },
		{ "install", test_install },
		{ "share", test_share },
		{ "tools", test_tools },
		{ "pippin", test_pippin },
		{ "pad", test_pad },
		{ "settings", test_settings },
		{ "wheel", test_wheel },
		{ "isp", test_isp },
		{ "clip", test_clip },
		{ "raster", test_raster },
		{ "rave", test_rave },
		{ "gpu", test_gpu },
		{ "glfront", test_glfront },
		{ "opengl", test_opengl },
		{ "display", test_display },
		{ "video", test_video },
		{ "jit", test_jit },
		{ "mp", test_mp },
		{ "nvram", test_nvram },
		{ "startup", test_startup },
		{ "priv", test_priv },
		{ "welcome", test_welcome },
	};

	env_renamed("blue-tests");
	for (size_t s = 0; s < sizeof suites / sizeof suites[0]; s++) {
		if (argc > 1 && strcmp(argv[1], suites[s].name) != 0)
			continue;
		int before = test_failures;
		suites[s].fn();
		printf("%-6s %s\n", suites[s].name, test_failures == before ? "ok" : "FAILED");
	}
	return test_failures ? 1 : 0;
}
