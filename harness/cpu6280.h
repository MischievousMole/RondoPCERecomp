/* HuC6280 interpreter core (65C02 + Rockwell bit ops + Hudson extensions). */
#ifndef CPU6280_H
#define CPU6280_H
#include <stdint.h>

#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_B 0x10
#define F_T 0x20
#define F_V 0x40
#define F_N 0x80

/* interrupt lines (IRQ disable register bits on the real chip) */
#define IRQ_IRQ2  0x01   /* CD / external, vector $FFF6 */
#define IRQ_IRQ1  0x02   /* VDC, vector $FFF8 */
#define IRQ_TIMER 0x04   /* timer, vector $FFFA */

typedef struct cpu6280 {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint8_t mpr[8];
    uint8_t speed;        /* 0 = 1.79 MHz, 1 = 7.16 MHz */
    uint8_t irq_mask;     /* disable bits, $1402 */
    uint8_t irq_pending;  /* request bits, set by devices */
    int64_t cycles;       /* master cycles (7.16 MHz ticks... counted in CPU cycles x speed factor) */
    uint8_t halted;
    uint32_t ninstr;      /* instructions executed (both cores) */
    /* host hooks */
    uint8_t (*read)(void *ctx, uint32_t phys);
    void    (*write)(void *ctx, uint32_t phys, uint8_t v);
    /* called for the reserved opcode $FB (HLE trap); return nonzero to stop */
    int     (*trap)(void *ctx, struct cpu6280 *c);
    void *ctx;
} cpu6280;

void cpu_reset(cpu6280 *c);
/* execute one instruction (or take an interrupt); returns CPU cycles used */
int  cpu_step(cpu6280 *c);
uint32_t cpu_phys(const cpu6280 *c, uint16_t a);
uint8_t cpu_read(cpu6280 *c, uint16_t a);
void cpu_write(cpu6280 *c, uint16_t a, uint8_t v);
void cpu_push(cpu6280 *c, uint8_t v);
uint8_t cpu_pull(cpu6280 *c);
void cpu_adc(cpu6280 *c, uint8_t m, uint8_t *acc);
void cpu_sbc(cpu6280 *c, uint8_t m);
void cpu_cmp(cpu6280 *c, uint8_t r, uint8_t m);
void cpu_interrupt(cpu6280 *c, uint16_t vec, int brk);
int cpu_block(cpu6280 *c, uint8_t op, uint16_t s, uint16_t d, uint16_t n);

#endif
