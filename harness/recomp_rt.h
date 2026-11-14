/* Runtime support for recompiled HuC6280 code. */
#ifndef RECOMP_RT_H
#define RECOMP_RT_H
#include <stdint.h>
#include "cpu6280.h"

extern uint32_t g_ovl_epoch;   /* bumped whenever a bank is reloaded */

static inline uint8_t zr_(cpu6280 *c, uint8_t z) { return c->read(c->ctx, ((uint32_t)c->mpr[1] << 13) | z); }
static inline void zw_(cpu6280 *c, uint8_t z, uint8_t v) { c->write(c->ctx, ((uint32_t)c->mpr[1] << 13) | z, v); }
#define ZR(z)     zr_(c, (uint8_t)(z))
#define ZW(z, v)  zw_(c, (uint8_t)(z), (uint8_t)(v))
#define ZR16(z)   (uint16_t)(ZR(z) | (ZR((uint8_t)((z) + 1)) << 8))
#define MR(a)     cpu_read(c, (uint16_t)(a))
#define MW(a, v)  cpu_write(c, (uint16_t)(a), (uint8_t)(v))
#define MR16(a)   (uint16_t)(MR(a) | (MR((uint16_t)((a) + 1)) << 8))
#define NZ(v)     (c->p = (c->p & ~(F_N | F_Z)) | ((v) & 0x80) | (((uint8_t)(v)) ? 0 : F_Z))

typedef struct native_ovl {
    uint8_t bank; uint32_t hash;
    int (*fn)(cpu6280 *c, int budget);
    const uint8_t *codemap;    /* bitmap of translated code bytes (self-modifying code detection) */
} native_ovl_t;
extern const native_ovl_t g_native_table[];
extern const int g_native_count;

#endif
