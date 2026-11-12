/* NSO0 parsing and mapping at a fixed guest address. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "debug_log.h"
#include "guest.h"

int som_lz4_decompress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_cap);

#define PAGE 0x1000u

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static int parse_layout(const uint8_t *h, NsoModule *m) {
    if (memcmp(h, "NSO0", 4) != 0) return -1;
    m->text_off = rd32(h + 0x14);
    m->text_size = rd32(h + 0x18);
    m->ro_off = rd32(h + 0x24);
    m->ro_size = rd32(h + 0x28);
    m->data_off = rd32(h + 0x34);
    m->data_size = rd32(h + 0x38);
    m->bss_size = rd32(h + 0x3C);
    uint32_t end = m->data_off + m->data_size + m->bss_size;
    m->image_size = (end + PAGE - 1) & ~(PAGE - 1);
    memcpy(m->build_id, h + 0x40, 32);
    return 0;
}

int nso_peek_size(const char *path, uint32_t *image_size) {
    uint8_t h[0x100];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    NsoModule m;
    if (n != sizeof h || parse_layout(h, &m) != 0) return -1;
    *image_size = m.image_size;
    return 0;
}

int nso_load(const char *path, const char *name, uint64_t base, NsoModule *out) {
    size_t fsize;
    uint8_t *raw = read_file(path, &fsize);
    if (!raw || fsize < 0x100) {
        som_log("nso: cannot read %s", path);
        free(raw);
        return -1;
    }
    NsoModule m;
    memset(&m, 0, sizeof m);
    if (parse_layout(raw, &m) != 0) {
        som_log("nso: %s is not an NSO", path);
        free(raw);
        return -1;
    }
    snprintf(m.name, sizeof m.name, "%s", name);
    m.base = base;

    void *p = mmap((void *)base, m.image_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || (uint64_t)p != base) {
        som_log("nso: mmap of %s at 0x%llx failed: %s", name, (unsigned long long)base, strerror(errno));
        free(raw);
        return -1;
    }

    uint32_t flags = rd32(raw + 0x0C);
    for (int i = 0; i < 3; i++) {
        uint32_t file_off = rd32(raw + 0x10 + i * 0x10);
        uint32_t mem_off = rd32(raw + 0x14 + i * 0x10);
        uint32_t dec_size = rd32(raw + 0x18 + i * 0x10);
        uint32_t comp_size = rd32(raw + 0x60 + i * 4);
        if ((uint64_t)file_off + comp_size > fsize) {
            som_log("nso: %s segment %d is truncated", name, i);
            free(raw);
            return -1;
        }
        uint8_t *dst = (uint8_t *)(base + mem_off);
        if (flags & (1u << i)) {
            int n = som_lz4_decompress(raw + file_off, comp_size, dst, dec_size);
            if (n != (int)dec_size) {
                som_log("nso: %s segment %d lz4 failed (%d, expected %u)", name, i, n, dec_size);
                free(raw);
                return -1;
            }
        } else {
            memcpy(dst, raw + file_off, dec_size < comp_size ? dec_size : comp_size);
        }
    }
    free(raw);
    *out = m;
    som_log("nso: %-8s base 0x%llx size 0x%x text 0x%x+0x%x ro 0x%x+0x%x data 0x%x+0x%x bss 0x%x", name,
            (unsigned long long)base, m.image_size, m.text_off, m.text_size, m.ro_off, m.ro_size, m.data_off,
            m.data_size, m.bss_size);
    return 0;
}
