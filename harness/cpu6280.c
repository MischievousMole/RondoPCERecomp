/* HuC6280 interpreter core. Cycle counts follow the Hudson documentation. */
#include "cpu6280.h"

uint32_t cpu_phys(const cpu6280 *c, uint16_t a) {
    return ((uint32_t)c->mpr[a >> 13] << 13) | (a & 0x1FFF);
}
uint8_t cpu_read(cpu6280 *c, uint16_t a) { return c->read(c->ctx, cpu_phys(c, a)); }
void cpu_write(cpu6280 *c, uint16_t a, uint8_t v) { c->write(c->ctx, cpu_phys(c, a), v); }

/* zero page and stack always live in the page mapped by MPR1 */
static inline uint8_t zr(cpu6280 *c, uint8_t z) { return c->read(c->ctx, ((uint32_t)c->mpr[1] << 13) | z); }
static inline void zw(cpu6280 *c, uint8_t z, uint8_t v) { c->write(c->ctx, ((uint32_t)c->mpr[1] << 13) | z, v); }
static inline uint16_t zr16(cpu6280 *c, uint8_t z) { return zr(c, z) | (zr(c, (uint8_t)(z + 1)) << 8); }
void cpu_push(cpu6280 *c, uint8_t v) { c->write(c->ctx, ((uint32_t)c->mpr[1] << 13) | 0x100 | c->s, v); c->s--; }
uint8_t cpu_pull(cpu6280 *c) { c->s++; return c->read(c->ctx, ((uint32_t)c->mpr[1] << 13) | 0x100 | c->s); }

static inline uint8_t fetch(cpu6280 *c) { return cpu_read(c, c->pc++); }
static inline uint16_t fetch16(cpu6280 *c) { uint16_t l = fetch(c); return l | (fetch(c) << 8); }
static inline uint16_t rd16(cpu6280 *c, uint16_t a) { return cpu_read(c, a) | (cpu_read(c, (uint16_t)(a + 1)) << 8); }

#define SETNZ(v) do { c->p = (c->p & ~(F_N|F_Z)) | ((v) & 0x80) | ((v) ? 0 : F_Z); } while (0)

void cpu_reset(cpu6280 *c) {
    c->mpr[7] = 0x00;
    c->p = F_I; c->s = 0xFF; c->speed = 0;
    c->irq_mask = 0; c->irq_pending = 0; c->halted = 0;
    c->pc = rd16(c, 0xFFFE);
}

void cpu_interrupt(cpu6280 *c, uint16_t vec, int brk) {
    cpu_push(c, c->pc >> 8); cpu_push(c, c->pc & 0xFF);
    cpu_push(c, (c->p & ~(F_B | F_T)) | (brk ? F_B : 0));
    c->p = (c->p & ~(F_D | F_T)) | F_I;
    c->pc = rd16(c, vec);
}

/* ALU helpers; the T flag redirects ADC/AND/EOR/ORA to zero page (X) */
void cpu_adc(cpu6280 *c, uint8_t m, uint8_t *acc) {
    uint8_t a = *acc; unsigned r;
    if (c->p & F_D) {
        unsigned lo = (a & 0x0F) + (m & 0x0F) + (c->p & F_C);
        unsigned hi = (a >> 4) + (m >> 4);
        if (lo > 9) { lo += 6; hi++; }
        if (hi > 9) hi += 6;
        r = ((hi << 4) | (lo & 0x0F)) & 0xFF;
        c->p = (c->p & ~F_C) | (hi > 15 ? F_C : 0);
        SETNZ(r); *acc = r;
        return;
    }
    r = a + m + (c->p & F_C);
    c->p = (c->p & ~(F_C | F_V)) | (r > 0xFF ? F_C : 0) | ((~(a ^ m) & (a ^ r) & 0x80) ? F_V : 0);
    *acc = r & 0xFF; SETNZ(*acc);
}
void cpu_sbc(cpu6280 *c, uint8_t m) {
    uint8_t a = c->a;
    if (c->p & F_D) {
        int lo = (a & 0x0F) - (m & 0x0F) - !(c->p & F_C);
        int hi = (a >> 4) - (m >> 4);
        if (lo < 0) { lo -= 6; hi--; }
        int borrow = hi < 0;
        if (hi < 0) hi -= 6;
        c->a = ((hi << 4) | (lo & 0x0F)) & 0xFF;
        c->p = (c->p & ~F_C) | (borrow ? 0 : F_C);
        SETNZ(c->a);
        return;
    }
    unsigned r = a - m - !(c->p & F_C);
    c->p = (c->p & ~(F_C | F_V)) | (r < 0x100 ? F_C : 0) | (((a ^ m) & (a ^ r) & 0x80) ? F_V : 0);
    c->a = r & 0xFF; SETNZ(c->a);
}
void cpu_cmp(cpu6280 *c, uint8_t r, uint8_t m) {
    unsigned d = r - m;
    c->p = (c->p & ~F_C) | (r >= m ? F_C : 0);
    SETNZ(d & 0xFF);
}

int cpu_block(cpu6280 *c, uint8_t op, uint16_t s, uint16_t d, uint16_t n) {
    unsigned len = n ? n : 0x10000;
    cpu_push(c, c->y); cpu_push(c, c->a); cpu_push(c, c->x);
    for (unsigned i = 0; i < len; i++) {
        uint8_t v = cpu_read(c, s);
        cpu_write(c, d, v);
        switch (op) {
        case 0x73: s++; d++; break;                         /* TII */
        case 0xC3: s--; d--; break;                         /* TDD */
        case 0xD3: s++; break;                              /* TIN */
        case 0xE3: s++; d += (i & 1) ? -1 : 1; break;       /* TIA */
        case 0xF3: d++; s += (i & 1) ? -1 : 1; break;       /* TAI */
        }
    }
    c->x = cpu_pull(c); c->a = cpu_pull(c); c->y = cpu_pull(c);
    return 17 + 6 * len;
}

int cpu_step(cpu6280 *c) {
    /* interrupts */
    if (!(c->p & F_I)) {
        uint8_t live = c->irq_pending & ~c->irq_mask;
        if (live) {
            uint16_t vec = (live & IRQ_TIMER) ? 0xFFFA : (live & IRQ_IRQ1) ? 0xFFF8 : 0xFFF6;
            cpu_interrupt(c, vec, 0);
            c->halted = 0;
            return 8;
        }
    }
    if (c->halted) return 4;

    int t = (c->p & F_T) != 0;
    c->ninstr++;
    uint8_t op = fetch(c);
    uint16_t ea = 0; uint8_t m, zp; int cyc = 2;
    c->p &= ~F_T;

#define EA_ZP   (zp = fetch(c), ea = zp)
#define EA_ZPX  (zp = fetch(c) + c->x, ea = zp)
#define EA_ZPY  (zp = fetch(c) + c->y, ea = zp)
#define EA_ABS  (ea = fetch16(c))
#define EA_ABX  (ea = fetch16(c) + c->x)
#define EA_ABY  (ea = fetch16(c) + c->y)
#define EA_IZX  (ea = zr16(c, (uint8_t)(fetch(c) + c->x)))
#define EA_IZY  (ea = zr16(c, fetch(c)) + c->y)
#define EA_IZP  (ea = zr16(c, fetch(c)))
#define RZ()    zr(c, (uint8_t)ea)
#define WZ(v)   zw(c, (uint8_t)ea, (v))
#define RM()    cpu_read(c, ea)
#define WM(v)   cpu_write(c, ea, (v))

/* accumulator-type ops honouring T */
#define TACC(body) do { if (t) { uint8_t acc = zr(c, c->x); body; zw(c, c->x, acc); cyc += 3; } \
                        else { uint8_t acc = c->a; body; c->a = acc; } } while (0)
#define OP_ORA(v) TACC(acc |= (v); SETNZ(acc))
#define OP_AND(v) TACC(acc &= (v); SETNZ(acc))
#define OP_EOR(v) TACC(acc ^= (v); SETNZ(acc))
#define OP_ADC(v) TACC(cpu_adc(c, (v), &acc))

#define GROUP(base, OPM) \
    case base+0x01: EA_IZX; OPM(RM()); cyc = 7; break; \
    case base+0x05: EA_ZP;  OPM(RZ()); cyc = 4; break; \
    case base+0x09: OPM(fetch(c)); cyc = 2; break; \
    case base+0x0D: EA_ABS; OPM(RM()); cyc = 5; break; \
    case base+0x11: EA_IZY; OPM(RM()); cyc = 7; break; \
    case base+0x12: EA_IZP; OPM(RM()); cyc = 7; break; \
    case base+0x15: EA_ZPX; OPM(RZ()); cyc = 4; break; \
    case base+0x19: EA_ABY; OPM(RM()); cyc = 5; break; \
    case base+0x1D: EA_ABX; OPM(RM()); cyc = 5; break;

#define OP_SBC(v) cpu_sbc(c, (v))
#define OP_CMP(v) cpu_cmp(c, c->a, (v))
#define OP_LDA(v) do { c->a = (v); SETNZ(c->a); } while (0)

/* read-modify-write */
#define ASL_(v) (c->p = (c->p & ~F_C) | ((v) >> 7), (uint8_t)((v) << 1))
#define LSR_(v) (c->p = (c->p & ~F_C) | ((v) & 1), (uint8_t)((v) >> 1))
#define ROL_(v) ({ uint8_t _o = (v); uint8_t _r = (_o << 1) | (c->p & F_C); c->p = (c->p & ~F_C) | (_o >> 7); _r; })
#define ROR_(v) ({ uint8_t _o = (v); uint8_t _r = (_o >> 1) | ((c->p & F_C) << 7); c->p = (c->p & ~F_C) | (_o & 1); _r; })
#define RMW_GROUP(base, FN) \
    case base+0x06: { EA_ZP;  uint8_t v = RZ(); v = FN(v); WZ(v); SETNZ(v); cyc = 6; break; } \
    case base+0x0A: { uint8_t v = c->a; v = FN(v); c->a = v; SETNZ(v); cyc = 2; break; } \
    case base+0x0E: { EA_ABS; uint8_t v = RM(); v = FN(v); WM(v); SETNZ(v); cyc = 7; break; } \
    case base+0x16: { EA_ZPX; uint8_t v = RZ(); v = FN(v); WZ(v); SETNZ(v); cyc = 6; break; } \
    case base+0x1E: { EA_ABX; uint8_t v = RM(); v = FN(v); WM(v); SETNZ(v); cyc = 7; break; }

#define BRANCH(cond) do { int8_t d = (int8_t)fetch(c); cyc = 2; if (cond) { c->pc += d; cyc = 4; } } while (0)
#define BITF(v, mm) do { c->p = (c->p & ~(F_N|F_V|F_Z)) | ((v) & 0xC0) | (((v) & (mm)) ? 0 : F_Z); } while (0)
#define TSTF(imm, v) do { c->p = (c->p & ~(F_N|F_V|F_Z)) | ((v) & 0xC0) | (((v) & (imm)) ? 0 : F_Z); } while (0)

    switch (op) {
    GROUP(0x00, OP_ORA)
    GROUP(0x20, OP_AND)
    GROUP(0x40, OP_EOR)
    GROUP(0x60, OP_ADC)
    GROUP(0xE0, OP_SBC)
    GROUP(0xC0, OP_CMP)
    /* LDA (0xA0 group) */
    case 0xA1: EA_IZX; OP_LDA(RM()); cyc = 7; break;
    case 0xA5: EA_ZP;  OP_LDA(RZ()); cyc = 4; break;
    case 0xA9: OP_LDA(fetch(c)); break;
    case 0xAD: EA_ABS; OP_LDA(RM()); cyc = 5; break;
    case 0xB1: EA_IZY; OP_LDA(RM()); cyc = 7; break;
    case 0xB2: EA_IZP; OP_LDA(RM()); cyc = 7; break;
    case 0xB5: EA_ZPX; OP_LDA(RZ()); cyc = 4; break;
    case 0xB9: EA_ABY; OP_LDA(RM()); cyc = 5; break;
    case 0xBD: EA_ABX; OP_LDA(RM()); cyc = 5; break;
    /* STA */
    case 0x81: EA_IZX; WM(c->a); cyc = 7; break;
    case 0x85: EA_ZP;  WZ(c->a); cyc = 4; break;
    case 0x8D: EA_ABS; WM(c->a); cyc = 5; break;
    case 0x91: EA_IZY; WM(c->a); cyc = 7; break;
    case 0x92: EA_IZP; WM(c->a); cyc = 7; break;
    case 0x95: EA_ZPX; WZ(c->a); cyc = 4; break;
    case 0x99: EA_ABY; WM(c->a); cyc = 5; break;
    case 0x9D: EA_ABX; WM(c->a); cyc = 5; break;
    RMW_GROUP(0x00, ASL_)
    RMW_GROUP(0x20, ROL_)
    RMW_GROUP(0x40, LSR_)
    RMW_GROUP(0x60, ROR_)
    /* INC / DEC */
    case 0x1A: c->a++; SETNZ(c->a); break;
    case 0x3A: c->a--; SETNZ(c->a); break;
    case 0xE6: { EA_ZP;  uint8_t v = RZ() + 1; WZ(v); SETNZ(v); cyc = 6; break; }
    case 0xF6: { EA_ZPX; uint8_t v = RZ() + 1; WZ(v); SETNZ(v); cyc = 6; break; }
    case 0xEE: { EA_ABS; uint8_t v = RM() + 1; WM(v); SETNZ(v); cyc = 7; break; }
    case 0xFE: { EA_ABX; uint8_t v = RM() + 1; WM(v); SETNZ(v); cyc = 7; break; }
    case 0xC6: { EA_ZP;  uint8_t v = RZ() - 1; WZ(v); SETNZ(v); cyc = 6; break; }
    case 0xD6: { EA_ZPX; uint8_t v = RZ() - 1; WZ(v); SETNZ(v); cyc = 6; break; }
    case 0xCE: { EA_ABS; uint8_t v = RM() - 1; WM(v); SETNZ(v); cyc = 7; break; }
    case 0xDE: { EA_ABX; uint8_t v = RM() - 1; WM(v); SETNZ(v); cyc = 7; break; }
    case 0xE8: c->x++; SETNZ(c->x); break;
    case 0xCA: c->x--; SETNZ(c->x); break;
    case 0xC8: c->y++; SETNZ(c->y); break;
    case 0x88: c->y--; SETNZ(c->y); break;
    /* LDX/LDY/STX/STY/STZ */
    case 0xA2: c->x = fetch(c); SETNZ(c->x); break;
    case 0xA6: EA_ZP;  c->x = RZ(); SETNZ(c->x); cyc = 4; break;
    case 0xB6: EA_ZPY; c->x = RZ(); SETNZ(c->x); cyc = 4; break;
    case 0xAE: EA_ABS; c->x = RM(); SETNZ(c->x); cyc = 5; break;
    case 0xBE: EA_ABY; c->x = RM(); SETNZ(c->x); cyc = 5; break;
    case 0xA0: c->y = fetch(c); SETNZ(c->y); break;
    case 0xA4: EA_ZP;  c->y = RZ(); SETNZ(c->y); cyc = 4; break;
    case 0xB4: EA_ZPX; c->y = RZ(); SETNZ(c->y); cyc = 4; break;
    case 0xAC: EA_ABS; c->y = RM(); SETNZ(c->y); cyc = 5; break;
    case 0xBC: EA_ABX; c->y = RM(); SETNZ(c->y); cyc = 5; break;
    case 0x86: EA_ZP;  WZ(c->x); cyc = 4; break;
    case 0x96: EA_ZPY; WZ(c->x); cyc = 4; break;
    case 0x8E: EA_ABS; WM(c->x); cyc = 5; break;
    case 0x84: EA_ZP;  WZ(c->y); cyc = 4; break;
    case 0x94: EA_ZPX; WZ(c->y); cyc = 4; break;
    case 0x8C: EA_ABS; WM(c->y); cyc = 5; break;
    case 0x64: EA_ZP;  WZ(0); cyc = 4; break;
    case 0x74: EA_ZPX; WZ(0); cyc = 4; break;
    case 0x9C: EA_ABS; WM(0); cyc = 5; break;
    case 0x9E: EA_ABX; WM(0); cyc = 5; break;
    /* compares */
    case 0xE0: cpu_cmp(c, c->x, fetch(c)); break;
    case 0xE4: EA_ZP;  cpu_cmp(c, c->x, RZ()); cyc = 4; break;
    case 0xEC: EA_ABS; cpu_cmp(c, c->x, RM()); cyc = 5; break;
    case 0xC0: cpu_cmp(c, c->y, fetch(c)); break;
    case 0xC4: EA_ZP;  cpu_cmp(c, c->y, RZ()); cyc = 4; break;
    case 0xCC: EA_ABS; cpu_cmp(c, c->y, RM()); cyc = 5; break;
    /* BIT / TSB / TRB / TST */
    case 0x89: { uint8_t v = fetch(c); BITF(v, c->a); break; }
    case 0x24: { EA_ZP;  uint8_t v = RZ(); BITF(v, c->a); cyc = 4; break; }
    case 0x34: { EA_ZPX; uint8_t v = RZ(); BITF(v, c->a); cyc = 4; break; }
    case 0x2C: { EA_ABS; uint8_t v = RM(); BITF(v, c->a); cyc = 5; break; }
    case 0x3C: { EA_ABX; uint8_t v = RM(); BITF(v, c->a); cyc = 5; break; }
    case 0x04: { EA_ZP;  uint8_t v = RZ(); BITF(v, c->a); WZ(v | c->a); cyc = 6; break; }
    case 0x0C: { EA_ABS; uint8_t v = RM(); BITF(v, c->a); WM(v | c->a); cyc = 7; break; }
    case 0x14: { EA_ZP;  uint8_t v = RZ(); BITF(v, c->a); WZ(v & ~c->a); cyc = 6; break; }
    case 0x1C: { EA_ABS; uint8_t v = RM(); BITF(v, c->a); WM(v & ~c->a); cyc = 7; break; }
    case 0x83: { uint8_t i = fetch(c); EA_ZP;  uint8_t v = RZ(); TSTF(i, v); cyc = 7; break; }
    case 0xA3: { uint8_t i = fetch(c); EA_ZPX; uint8_t v = RZ(); TSTF(i, v); cyc = 7; break; }
    case 0x93: { uint8_t i = fetch(c); EA_ABS; uint8_t v = RM(); TSTF(i, v); cyc = 8; break; }
    case 0xB3: { uint8_t i = fetch(c); EA_ABX; uint8_t v = RM(); TSTF(i, v); cyc = 8; break; }
    /* branches */
    case 0x10: BRANCH(!(c->p & F_N)); break;
    case 0x30: BRANCH(c->p & F_N); break;
    case 0x50: BRANCH(!(c->p & F_V)); break;
    case 0x70: BRANCH(c->p & F_V); break;
    case 0x90: BRANCH(!(c->p & F_C)); break;
    case 0xB0: BRANCH(c->p & F_C); break;
    case 0xD0: BRANCH(!(c->p & F_Z)); break;
    case 0xF0: BRANCH(c->p & F_Z); break;
    case 0x80: { int8_t d = (int8_t)fetch(c); c->pc += d; cyc = 4; break; }
    case 0x44: { int8_t d = (int8_t)fetch(c); uint16_t r = c->pc - 1;
                 cpu_push(c, r >> 8); cpu_push(c, r & 0xFF); c->pc += d; cyc = 8; break; }
    /* jumps */
    case 0x4C: c->pc = fetch16(c); cyc = 4; break;
    case 0x6C: { uint16_t a = fetch16(c); c->pc = rd16(c, a); cyc = 7; break; }
    case 0x7C: { uint16_t a = fetch16(c) + c->x; c->pc = rd16(c, a); cyc = 7; break; }
    case 0x20: { uint16_t a = fetch16(c); uint16_t r = c->pc - 1;
                 cpu_push(c, r >> 8); cpu_push(c, r & 0xFF); c->pc = a; cyc = 7; break; }
    case 0x60: { uint16_t l = cpu_pull(c); c->pc = (l | (cpu_pull(c) << 8)) + 1; cyc = 7; break; }
    case 0x40: { c->p = cpu_pull(c); uint16_t l = cpu_pull(c); c->pc = l | (cpu_pull(c) << 8); cyc = 7; break; }
    case 0x00: c->pc++; cpu_interrupt(c, 0xFFF6, 1); cyc = 8; break;
    /* stack */
    case 0x48: cpu_push(c, c->a); cyc = 3; break;
    case 0xDA: cpu_push(c, c->x); cyc = 3; break;
    case 0x5A: cpu_push(c, c->y); cyc = 3; break;
    case 0x08: cpu_push(c, c->p | F_B); cyc = 3; break;
    case 0x68: c->a = cpu_pull(c); SETNZ(c->a); cyc = 4; break;
    case 0xFA: c->x = cpu_pull(c); SETNZ(c->x); cyc = 4; break;
    case 0x7A: c->y = cpu_pull(c); SETNZ(c->y); cyc = 4; break;
    case 0x28: c->p = cpu_pull(c); cyc = 4; break;
    /* transfers */
    case 0xAA: c->x = c->a; SETNZ(c->x); break;
    case 0x8A: c->a = c->x; SETNZ(c->a); break;
    case 0xA8: c->y = c->a; SETNZ(c->y); break;
    case 0x98: c->a = c->y; SETNZ(c->a); break;
    case 0xBA: c->x = c->s; SETNZ(c->x); break;
    case 0x9A: c->s = c->x; break;
    case 0x22: { uint8_t v = c->a; c->a = c->x; c->x = v; cyc = 3; break; }
    case 0x42: { uint8_t v = c->a; c->a = c->y; c->y = v; cyc = 3; break; }
    case 0x02: { uint8_t v = c->x; c->x = c->y; c->y = v; cyc = 3; break; }
    case 0x62: c->a = 0; break;
    case 0x82: c->x = 0; break;
    case 0xC2: c->y = 0; break;
    /* flags */
    case 0x18: c->p &= ~F_C; break;
    case 0x38: c->p |= F_C; break;
    case 0x58: c->p &= ~F_I; break;
    case 0x78: c->p |= F_I; break;
    case 0xB8: c->p &= ~F_V; break;
    case 0xD8: c->p &= ~F_D; break;
    case 0xF8: c->p |= F_D; break;
    case 0xF4: c->p |= F_T; cyc = 2; break;
    case 0xEA: break;
    case 0x54: c->speed = 0; cyc = 3; break;
    case 0xD4: c->speed = 1; cyc = 3; break;
    /* Hudson */
    case 0x03: c->write(c->ctx, 0x1FE000, fetch(c)); cyc = 5; break;
    case 0x13: c->write(c->ctx, 0x1FE002, fetch(c)); cyc = 5; break;
    case 0x23: c->write(c->ctx, 0x1FE003, fetch(c)); cyc = 5; break;
    case 0x53: { uint8_t mk = fetch(c); for (int i = 0; i < 8; i++) if (mk & (1 << i)) c->mpr[i] = c->a; cyc = 5; break; }
    case 0x43: { uint8_t mk = fetch(c); for (int i = 0; i < 8; i++) if (mk & (1 << i)) { c->a = c->mpr[i]; break; } cyc = 4; break; }
    case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3: {
        uint16_t s = fetch16(c), d = fetch16(c), n = fetch16(c);
        cyc = cpu_block(c, op, s, d, n); break;
    }
    case 0xFB: /* HLE trap */
        if (c->trap && c->trap(c->ctx, c)) return -1;
        cyc = 2; break;
    default:
        if ((op & 0x0F) == 0x07) { /* RMB/SMB */
            EA_ZP; uint8_t bit = 1 << ((op >> 4) & 7); uint8_t v = RZ();
            WZ(op & 0x80 ? (v | bit) : (v & ~bit)); cyc = 7;
        } else if ((op & 0x0F) == 0x0F) { /* BBR/BBS */
            EA_ZP; uint8_t v = RZ(); int8_t d = (int8_t)fetch(c);
            int set = (v >> ((op >> 4) & 7)) & 1; cyc = 6;
            if ((op & 0x80) ? set : !set) { c->pc += d; cyc = 8; }
        } else {
            cyc = 2; /* undefined opcodes act as NOP on the 6280 */
        }
        break;
    }
    return cyc;
}
