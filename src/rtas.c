#include "rtas.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bootinfo.h"

#define RTAS_SUCCESS 0u
#define RTAS_HW_ERROR 0xFFFFFFFFu       /* -1 */
#define RTAS_PARAM_ERROR 0xFFFFFFFDu    /* -3 */

void rtas_init(struct rtas *r)
{
	memset(r, 0, sizeof *r);
}

int rtas_nvram_load(struct rtas *r, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return errno == ENOENT ? 1 : -1;
	size_t n = fread(r->nvram, 1, sizeof r->nvram, f);
	fclose(f);
	return n == sizeof r->nvram ? 0 : -1;
}

int rtas_nvram_save(const struct rtas *r, const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	size_t n = fwrite(r->nvram, 1, sizeof r->nvram, f);
	return (fclose(f) != 0 || n != sizeof r->nvram) ? -1 : 0;
}

const char *rtas_name(uint32_t token)
{
	switch (token) {
	case RTAS_NVRAM_FETCH:            return "nvram-fetch";
	case RTAS_NVRAM_STORE:            return "nvram-store";
	case RTAS_GET_TIME_OF_DAY:        return "get-time-of-day";
	case RTAS_SET_TIME_OF_DAY:        return "set-time-of-day";
	case RTAS_SYSTEM_REBOOT:          return "system-reboot";
	case RTAS_POWER_OFF:              return "power-off";
	case RTAS_SET_TIME_FOR_POWER_ON:  return "set-time-for-power-on";
	case RTAS_EVENT_SCAN:             return "event-scan";
	case RTAS_CHECK_EXCEPTION:        return "check-exception";
	case RTAS_READ_PCI_CONFIG:        return "read-pci-config";
	case RTAS_WRITE_PCI_CONFIG:       return "write-pci-config";
	case RTAS_GET_TIME_FOR_POWER_ON:  return "get-time-for-power-on";
	}
	return "unknown";
}

static uint32_t nvram_transfer(struct rtas *r, struct mem *mem, const uint32_t *arg, uint32_t nargs,
			       bool store, uint32_t *count)
{
	*count = 0;
	if (nargs < 3)
		return RTAS_PARAM_ERROR;
	uint32_t index = arg[0], buf = arg[1], len = arg[2];
	if (index > RTAS_NVRAM_SIZE || len > RTAS_NVRAM_SIZE - index)
		return RTAS_PARAM_ERROR;
	for (uint32_t k = 0; k < len; k++) {
		if (!mem_ok(mem, buf + k, 1, store ? MEM_R : MEM_W))
			return RTAS_HW_ERROR;
		if (store)
			r->nvram[index + k] = mem_r8(mem, buf + k);
		else
			mem_w8(mem, buf + k, r->nvram[index + k]);
	}
	*count = len;
	return RTAS_SUCCESS;
}

bool rtas_call(struct rtas *r, struct mem *mem, uint32_t block)
{
	if (!mem_ok(mem, block, 12, MEM_RW))
		return false;
	uint32_t token = mem_r32(mem, block);
	uint32_t nargs = mem_r16(mem, block + 6);
	uint32_t nret = mem_r32(mem, block + 8);
	if (nargs > 16 || nret > 16 || !mem_ok(mem, block, 12 + 4 * (nargs + nret), MEM_RW))
		return false;

	/* Pointer arguments are already guest addresses here; just consume the marker. */
	mem_w16(mem, block + 4, 0);

	uint32_t arg[16] = { 0 }, ret[16] = { 0 };
	for (uint32_t k = 0; k < nargs; k++)
		arg[k] = mem_r32(mem, block + 12 + 4 * k);

	r->calls[token < 16 ? token : 0]++;

	switch (token) {
	case RTAS_NVRAM_FETCH:
	case RTAS_NVRAM_STORE:
		ret[0] = nvram_transfer(r, mem, arg, nargs, token == RTAS_NVRAM_STORE, &ret[1]);
		break;
	case RTAS_GET_TIME_OF_DAY: {
		struct tm tm;
		long nsec;
		time_t now = guest_clock_now(r->clock, &nsec);
		localtime_r(&now, &tm);
		ret[0] = RTAS_SUCCESS;
		ret[1] = (uint32_t)tm.tm_year + 1900;
		ret[2] = (uint32_t)tm.tm_mon + 1;
		ret[3] = (uint32_t)tm.tm_mday;
		ret[4] = (uint32_t)tm.tm_hour;
		ret[5] = (uint32_t)tm.tm_min;
		ret[6] = (uint32_t)tm.tm_sec;
		ret[7] = (uint32_t)nsec;
		break;
	}
	case RTAS_SET_TIME_OF_DAY:          /* the host clock stays authoritative */
	case RTAS_SET_TIME_FOR_POWER_ON:
	case RTAS_EVENT_SCAN:
	case RTAS_CHECK_EXCEPTION:
		ret[0] = RTAS_SUCCESS;
		break;
	case RTAS_GET_TIME_FOR_POWER_ON:
		ret[0] = RTAS_SUCCESS;
		break;
	case RTAS_SYSTEM_REBOOT:
		r->reboot = true;
		ret[0] = RTAS_SUCCESS;
		break;
	case RTAS_POWER_OFF:
		r->power_off = true;
		ret[0] = RTAS_SUCCESS;
		break;
	case RTAS_READ_PCI_CONFIG:          /* (config_addr: bus << 16 | devfn << 8 | register, size) */
	case RTAS_WRITE_PCI_CONFIG: {       /* (..., value) */
		uint32_t a = arg[0], size = nargs > 1 ? arg[1] : 0;
		if ((size != 1 && size != 2 && size != 4) || (a & (size - 1)) || nargs < 2 + (token == RTAS_WRITE_PCI_CONFIG)) {
			ret[0] = RTAS_PARAM_ERROR;
			break;
		}
		ret[0] = RTAS_SUCCESS;
		if (token == RTAS_WRITE_PCI_CONFIG) {
			if (r->pci)
				pci_config_write(r->pci, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, size, arg[2]);
		} else {
			ret[1] = r->pci ? pci_config_read(r->pci, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF, size)
					: size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
		}
		break;
	}
	default:
		fprintf(stderr, "rtas: unknown token %u\n", token);
		ret[0] = RTAS_PARAM_ERROR;
		break;
	}

	for (uint32_t k = 0; k < nret; k++)
		mem_w32(mem, block + 12 + 4 * (nargs + k), ret[k]);
	return true;
}
