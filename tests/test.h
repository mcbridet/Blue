#ifndef SYSHM_TEST_H
#define SYSHM_TEST_H

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
void test_drivers(void);
void test_net(void);
void test_hfs(void);
void test_hfsread(void);
void test_share(void);
void test_tools(void);
void test_raster(void);
void test_rave(void);
void test_gpu(void);
void test_display(void);
void test_jit(void);
void test_ohci(void);
void test_mp(void);
void test_nvram(void);

#endif
