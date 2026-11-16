#ifndef BLUE_TEST_H
#define BLUE_TEST_H

#include <stdio.h>

extern int test_failures;

#define CHECK(cond)                                                                     \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
			test_failures++;                                                \
		}                                                                       \
	} while (0)

#define CHECK_EQ(actual, expected)                                                      \
	do {                                                                            \
		unsigned long long a_ = (unsigned long long)(actual);                   \
		unsigned long long e_ = (unsigned long long)(expected);                 \
		if (a_ != e_) {                                                         \
			fprintf(stderr, "%s:%d: %s = 0x%llx, expected 0x%llx\n",        \
				__FILE__, __LINE__, #actual, a_, e_);                   \
			test_failures++;                                                \
		}                                                                       \
	} while (0)

void test_lzss(void);
void test_parcels(void);
void test_ppc(void);
void test_rom(void);
void test_cuda(void);
void test_mmio(void);
void test_dbdma(void);
void test_awacs(void);
void test_audio(void);
void test_cdimage(void);
void test_cdaudio(void);
void test_drivers(void);
void test_net(void);
void test_hfs(void);
void test_hfsread(void);
void test_floppy(void);
void test_drives(void);
void test_chooser(void);
void test_install(void);
void test_share(void);
void test_tools(void);
void test_pippin(void);
void test_pad(void);
void test_clip(void);
void test_raster(void);
void test_rave(void);
void test_gpu(void);
void test_glfront(void);
void test_opengl(void);
void test_display(void);
void test_video(void);
void test_jit(void);
void test_ohci(void);
void test_mp(void);
void test_nvram(void);
void test_startup(void);
void test_priv(void);
void test_welcome(void);

#endif
