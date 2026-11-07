#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "internal.h"
#include <tudor/log.h>

/* Implements kernel32!GetSystemFirmwareTable for the HP "110" driver, which the
 * Synaptics adapter calls (RSMB / raw SMBIOS) to derive a stable host identity
 * for secure pairing. The Linux DMI tables are root-only (0400), and the CLI
 * drops privileges after USB setup, so we read + cache them in a load-time
 * constructor (runs as root, before the privilege drop). */

static uint8_t *smbios_blob = NULL;
static size_t   smbios_blob_len = 0;

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if(sz <= 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t*) malloc((size_t) sz);
    if(!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t) sz, f);
    fclose(f);
    if(rd != (size_t) sz) { free(buf); return NULL; }
    *out_len = (size_t) sz;
    return buf;
}

__constr static void init_smbios_table() {
    size_t ep_len = 0, dmi_len = 0;
    uint8_t *ep  = read_file("/sys/firmware/dmi/tables/smbios_entry_point", &ep_len);
    uint8_t *dmi = read_file("/sys/firmware/dmi/tables/DMI", &dmi_len);
    if(!dmi || dmi_len == 0) { free(ep); free(dmi); return; }

    uint8_t major = 2, minor = 0, dmirev = 0;
    if(ep && ep_len >= 10 && memcmp(ep, "_SM3_", 5) == 0) {
        major = ep[7]; minor = ep[8]; dmirev = ep[9];
    } else if(ep && ep_len >= 8 && memcmp(ep, "_SM_", 4) == 0) {
        major = ep[6]; minor = ep[7];
    }

    /* RawSMBIOSData: Used20CallingMethod(1) Major(1) Minor(1) DmiRevision(1) Length(u32 LE) Data[] */
    smbios_blob_len = 8 + dmi_len;
    smbios_blob = (uint8_t*) malloc(smbios_blob_len);
    if(!smbios_blob) { smbios_blob_len = 0; free(ep); free(dmi); return; }
    smbios_blob[0] = 0;
    smbios_blob[1] = major;
    smbios_blob[2] = minor;
    smbios_blob[3] = dmirev;
    smbios_blob[4] = (uint8_t)(dmi_len & 0xff);
    smbios_blob[5] = (uint8_t)((dmi_len >> 8) & 0xff);
    smbios_blob[6] = (uint8_t)((dmi_len >> 16) & 0xff);
    smbios_blob[7] = (uint8_t)((dmi_len >> 24) & 0xff);
    memcpy(smbios_blob + 8, dmi, dmi_len);
    log_info("Cached SMBIOS firmware table for GetSystemFirmwareTable (%zu bytes, v%u.%u)",
             smbios_blob_len, major, minor);
    free(ep); free(dmi);
}

__winfnc UINT GetSystemFirmwareTable(DWORD sig, DWORD table_id, void *buf, DWORD buf_size) {
    (void) table_id;
    if(sig != 0x52534D42 /* 'RSMB' */ && sig != 0x424D5352) {
        log_warn("GetSystemFirmwareTable: unsupported provider 0x%08x", (unsigned) sig);
        return 0;
    }
    if(!smbios_blob || smbios_blob_len == 0) {
        log_warn("GetSystemFirmwareTable: SMBIOS table not available");
        return 0;
    }
    if(buf && buf_size >= smbios_blob_len) memcpy(buf, smbios_blob, smbios_blob_len);
    return (UINT) smbios_blob_len;
}
WINAPI(GetSystemFirmwareTable)

/* powrprof!CallNtPowerInformation — the driver queries power info during init.
 * Report success with a zeroed output buffer (no special power capabilities). */
__winfnc NTSTATUS CallNtPowerInformation(DWORD level, void *in_buf, DWORD in_len, void *out_buf, DWORD out_len) {
    (void) level; (void) in_buf; (void) in_len;
    if(out_buf && out_len) memset(out_buf, 0, out_len);
    return 0; /* STATUS_SUCCESS */
}
WINAPI(CallNtPowerInformation)
