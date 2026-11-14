"""HuC6280 -> C static recompiler.

Input: a directory of overlay snapshots + executed-instruction bitmaps written by the trace harness
(bank_BB_HHHHHHHH.bin / cov_BB_HHHHHHHH.bin). Every executed instruction start becomes a resume point
(case label). Control transfers set c->pc and go back to `top`, which re-checks the cycle budget,
pending interrupts, the code window and the overlay epoch before dispatching again.

Usage: recomp.py OVL_DIR OUT_DIR
"""
import sys, os, glob
sys.path.insert(0, os.path.dirname(__file__))
from dis6280 import OP, M

SKIP_BANKS = {0x00, 0xF8}    # HLE BIOS stubs and the one-shot IPL loader in RAM stay interpreted

# cycle counts (must match cpu6280.c)
def cycles(mn, mode):
    if mode == 'imm' or mode in ('imp', 'acc'):
        base = {'PHA':3,'PHX':3,'PHY':3,'PHP':3,'PLA':4,'PLX':4,'PLY':4,'PLP':4,'RTS':7,'RTI':7,
                'SAX':3,'SAY':3,'SXY':3,'CSL':3,'CSH':3,'TMA':4,'TAM':5,'ST0':5,'ST1':5,'ST2':5,'BRK':8}.get(mn, 2)
        return base
    rmw = mn in ('ASL','LSR','ROL','ROR','INC','DEC')
    if mn in ('TSB','TRB'): return 6 if mode == 'zp' else 7
    if mode in ('zp','zpx','zpy'): return 6 if rmw else 4
    if mode in ('abs','abx','aby'):
        if mn == 'JMP': return 4
        if mn == 'JSR': return 7
        return 7 if rmw else 5
    if mode in ('izx','izy','izp'): return 7
    if mode in ('ind','iax'): return 7
    return 2

ZPMODES = ('zp', 'zpx', 'zpy')

def ea_expr(mode, b):
    o = b[1] if len(b) > 1 else 0
    w = (b[1] | (b[2] << 8)) if len(b) > 2 else 0
    return {
        'zp':  f'0x{o:02X}',
        'zpx': f'(uint8_t)(0x{o:02X} + c->x)',
        'zpy': f'(uint8_t)(0x{o:02X} + c->y)',
        'abs': f'0x{w:04X}',
        'abx': f'(uint16_t)(0x{w:04X} + c->x)',
        'aby': f'(uint16_t)(0x{w:04X} + c->y)',
        'izx': f'ZR16((uint8_t)(0x{o:02X} + c->x))',
        'izy': f'(uint16_t)(ZR16(0x{o:02X}) + c->y)',
        'izp': f'ZR16(0x{o:02X})',
    }[mode]

def rd(mode, e):  return f'ZR({e})' if mode in ZPMODES else f'MR({e})'
def wr(mode, e, v): return f'ZW({e}, {v})' if mode in ZPMODES else f'MW({e}, {v})'

class Bail(Exception):
    pass

def translate_insn(off, b, after_set, next_off):
    """Return C statements for one instruction. `next_off` = offset of the following instruction."""
    op = b[0]
    mn, mode = OP.get(op, ('NOP', 'imp'))
    cyc = cycles(mn, mode)
    nxt = f'(uint16_t)(wb + 0x{next_off:04X})'
    s = []
    val = None
    if mode == 'imm': val = f'0x{b[1]:02X}'
    elif mode in ('zp','zpx','zpy','abs','abx','aby','izx','izy','izp') and mn not in ('JMP','JSR'):
        val = None  # computed below
    ex = None  # control transfer code (string) or None for fallthrough

    def read_val():
        if mode == 'imm': return val
        return rd(mode, ea_expr(mode, b))

    if mn in ('LDA','LDX','LDY'):
        r = {'LDA':'a','LDX':'x','LDY':'y'}[mn]
        s.append(f'c->{r} = {read_val()}; NZ(c->{r});')
    elif mn in ('STA','STX','STY','STZ'):
        v = {'STA':'c->a','STX':'c->x','STY':'c->y','STZ':'0'}[mn]
        s.append(wr(mode, ea_expr(mode, b), v) + ';')
    elif mn in ('ORA','AND','EOR','ADC'):
        o = {'ORA':'|=','AND':'&=','EOR':'^='}.get(mn)
        def body(acc):
            if mn == 'ADC': return f'cpu_adc(c, v_, &{acc});'
            return f'{acc} {o} v_; NZ({acc});'
        if after_set:
            s.append(f'{{ uint8_t v_ = {read_val()}; if (T) {{ uint8_t t_ = ZR(c->x); {body("t_")} ZW(c->x, t_); cyc += 3; }} else {{ {body("c->a")} }} }}')
        else:
            s.append(f'{{ uint8_t v_ = {read_val()}; {body("c->a")} }}')
    elif mn == 'SBC':
        s.append(f'cpu_sbc(c, {read_val()});')
    elif mn in ('CMP','CPX','CPY'):
        r = {'CMP':'a','CPX':'x','CPY':'y'}[mn]
        s.append(f'cpu_cmp(c, c->{r}, {read_val()});')
    elif mn in ('ASL','LSR','ROL','ROR','INC','DEC'):
        fn = {
            'ASL': 'c->p = (c->p & ~F_C) | (v_ >> 7); v_ <<= 1;',
            'LSR': 'c->p = (c->p & ~F_C) | (v_ & 1); v_ >>= 1;',
            'ROL': '{ uint8_t o_ = v_; v_ = (o_ << 1) | (c->p & F_C); c->p = (c->p & ~F_C) | (o_ >> 7); }',
            'ROR': '{ uint8_t o_ = v_; v_ = (o_ >> 1) | ((c->p & F_C) << 7); c->p = (c->p & ~F_C) | (o_ & 1); }',
            'INC': 'v_++;', 'DEC': 'v_--;'}[mn]
        if mode == 'acc':
            s.append(f'{{ uint8_t v_ = c->a; {fn} c->a = v_; NZ(v_); }}')
        else:
            e = ea_expr(mode, b)
            et = 'uint8_t' if mode in ZPMODES else 'uint16_t'
            s.append(f'{{ {et} e_ = {e}; uint8_t v_ = {rd(mode, "e_")}; {fn} {wr(mode, "e_", "v_")}; NZ(v_); }}')
    elif mn in ('INX','DEX','INY','DEY'):
        r = mn[2].lower(); d = '++' if mn[0] == 'I' else '--'
        s.append(f'c->{r}{d}; NZ(c->{r});')
    elif mn == 'BIT':
        s.append(f'{{ uint8_t v_ = {read_val()}; c->p = (c->p & ~(F_N|F_V|F_Z)) | (v_ & 0xC0) | ((v_ & c->a) ? 0 : F_Z); }}')
    elif mn in ('TSB','TRB'):
        e = ea_expr(mode, b); et = 'uint8_t' if mode in ZPMODES else 'uint16_t'
        nv = 'v_ | c->a' if mn == 'TSB' else 'v_ & ~c->a'
        s.append(f'{{ {et} e_ = {e}; uint8_t v_ = {rd(mode, "e_")}; c->p = (c->p & ~(F_N|F_V|F_Z)) | (v_ & 0xC0) | ((v_ & c->a) ? 0 : F_Z); {wr(mode, "e_", "(uint8_t)(" + nv + ")")}; }}')
    elif mn == 'TST':
        imm = b[1]
        if mode in ('timm_zp', 'timm_zpx'):
            e = f'0x{b[2]:02X}' if mode == 'timm_zp' else f'(uint8_t)(0x{b[2]:02X} + c->x)'
            r = f'ZR({e})'; cyc = 7
        else:
            w = b[2] | (b[3] << 8)
            e = f'0x{w:04X}' if mode == 'timm_abs' else f'(uint16_t)(0x{w:04X} + c->x)'
            r = f'MR({e})'; cyc = 8
        s.append(f'{{ uint8_t v_ = {r}; c->p = (c->p & ~(F_N|F_V|F_Z)) | (v_ & 0xC0) | ((v_ & 0x{imm:02X}) ? 0 : F_Z); }}')
    elif mode == 'rel' and mn in ('BPL','BMI','BVC','BVS','BCC','BCS','BNE','BEQ','BRA'):
        d = b[1] - 256 if b[1] > 127 else b[1]
        tgt = off + 2 + d
        cond = {'BPL':'!(c->p & F_N)','BMI':'(c->p & F_N)','BVC':'!(c->p & F_V)','BVS':'(c->p & F_V)',
                'BCC':'!(c->p & F_C)','BCS':'(c->p & F_C)','BNE':'!(c->p & F_Z)','BEQ':'(c->p & F_Z)','BRA':'1'}[mn]
        s.append(f'if ({cond}) {{ cyc += 2; c->pc = (uint16_t)(wb + {tgt}); goto top; }}')
    elif mn == 'BSR':
        d = b[1] - 256 if b[1] > 127 else b[1]
        tgt = off + 2 + d
        s.append(f'{{ uint16_t r_ = (uint16_t)(wb + 0x{off + 1:04X}); cpu_push(c, r_ >> 8); cpu_push(c, r_ & 0xFF); }}')
        ex = f'c->pc = (uint16_t)(wb + {tgt}); goto top;'; cyc = 8
    elif mode == 'zrel':
        bit = int(mn[3]); d = b[2] - 256 if b[2] > 127 else b[2]
        tgt = off + 3 + d
        test = f'(ZR(0x{b[1]:02X}) >> {bit}) & 1'
        cond = test if mn.startswith('BBS') else f'!({test})'
        cyc = 6
        s.append(f'if ({cond}) {{ cyc += 2; c->pc = (uint16_t)(wb + {tgt}); goto top; }}')
    elif mn.startswith('RMB') or mn.startswith('SMB'):
        bit = int(mn[3]); z = f'0x{b[1]:02X}'
        if mn.startswith('SMB'): s.append(f'ZW({z}, ZR({z}) | 0x{1 << bit:02X});')
        else: s.append(f'ZW({z}, ZR({z}) & 0x{(~(1 << bit)) & 0xFF:02X});')
        cyc = 7
    elif mn == 'JMP':
        if mode == 'abs': ex = f'c->pc = 0x{b[1] | b[2] << 8:04X}; goto top;'
        elif mode == 'ind': ex = f'c->pc = MR16(0x{b[1] | b[2] << 8:04X}); goto top;'
        else: ex = f'c->pc = MR16((uint16_t)(0x{b[1] | b[2] << 8:04X} + c->x)); goto top;'
    elif mn == 'JSR':
        s.append(f'{{ uint16_t r_ = (uint16_t)(wb + 0x{off + 2:04X}); cpu_push(c, r_ >> 8); cpu_push(c, r_ & 0xFF); }}')
        ex = f'c->pc = 0x{b[1] | b[2] << 8:04X}; goto top;'
    elif mn == 'RTS':
        ex = '{ uint16_t l_ = cpu_pull(c); c->pc = (uint16_t)((l_ | (cpu_pull(c) << 8)) + 1); } goto top;'
    elif mn == 'RTI':
        ex = '{ c->p = cpu_pull(c); uint16_t l_ = cpu_pull(c); c->pc = l_ | (cpu_pull(c) << 8); } goto top;'
    elif mn in ('PHA','PHX','PHY'):
        s.append(f'cpu_push(c, c->{mn[2].lower()});')
    elif mn == 'PHP':
        s.append('cpu_push(c, c->p | F_B);')
    elif mn in ('PLA','PLX','PLY'):
        r = mn[2].lower(); s.append(f'c->{r} = cpu_pull(c); NZ(c->{r});')
    elif mn == 'PLP':
        s.append('c->p = cpu_pull(c);'); ex = f'c->pc = {nxt}; goto top;'
    elif mn in ('TAX','TXA','TAY','TYA','TSX'):
        src, dst = {'TAX':('a','x'),'TXA':('x','a'),'TAY':('a','y'),'TYA':('y','a'),'TSX':('s','x')}[mn]
        s.append(f'c->{dst} = c->{src}; NZ(c->{dst});')
    elif mn == 'TXS': s.append('c->s = c->x;')
    elif mn in ('SAX','SAY','SXY'):
        r1, r2 = {'SAX':('a','x'),'SAY':('a','y'),'SXY':('x','y')}[mn]
        s.append(f'{{ uint8_t t_ = c->{r1}; c->{r1} = c->{r2}; c->{r2} = t_; }}')
    elif mn in ('CLA','CLX','CLY'): s.append(f'c->{mn[2].lower()} = 0;')
    elif mn == 'CLC': s.append('c->p &= ~F_C;')
    elif mn == 'SEC': s.append('c->p |= F_C;')
    elif mn == 'CLV': s.append('c->p &= ~F_V;')
    elif mn == 'CLD': s.append('c->p &= ~F_D;')
    elif mn == 'SED': s.append('c->p |= F_D;')
    elif mn == 'SEI': s.append('c->p |= F_I;')
    elif mn == 'CLI': s.append('c->p &= ~F_I;'); ex = f'c->pc = {nxt}; goto top;'
    elif mn == 'NOP': pass
    elif mn in ('ST0','ST1','ST2'):
        a = {'ST0':'0x1FE000','ST1':'0x1FE002','ST2':'0x1FE003'}[mn]
        s.append(f'c->write(c->ctx, {a}, 0x{b[1]:02X});')
    elif mn == 'TAM':
        mk = b[1]
        for i in range(8):
            if mk & (1 << i): s.append(f'c->mpr[{i}] = c->a;')
        ex = f'c->pc = {nxt}; goto top;'
    elif mn == 'TMA':
        mk = b[1]
        for i in range(8):
            if mk & (1 << i): s.append(f'c->a = c->mpr[{i}];'); break
    elif mn in ('CSL','CSH'):
        s.append(f'c->speed = {1 if mn == "CSH" else 0};')
        ex = f'c->pc = {nxt}; c->ninstr += 1; return cyc + {cyc};'
    elif mn == 'SET':
        s.append('T = 1;')
        return s, cyc, 'SET'
    elif mn in ('TII','TDD','TIN','TIA','TAI'):
        sa = b[1] | b[2] << 8; da = b[3] | b[4] << 8; n = b[5] | b[6] << 8
        s.append(f'cyc += cpu_block(c, 0x{op:02X}, 0x{sa:04X}, 0x{da:04X}, 0x{n:04X});')
        cyc = 0
    else:
        raise Bail(mn)
    return s, cyc, ex

VOLATILE = {}   # bank -> set of byte offsets the game patches at runtime (tools/volatile.txt)

def translate_overlay(bank, h, code, cov):
    starts = [i for i in range(0x2000) if cov[i >> 3] >> (i & 7) & 1]
    lines = []
    fn = f'N_{bank:02X}_{h:08X}'
    labels = []
    insn = {}
    for off in starts:
        op = code[off]
        mn, mode = OP.get(op, ('NOP', 'imp'))
        n = 1 + M[mode][0]
        if off + n > 0x2000 or op in (0xFB, 0x00):
            continue
        if any(i in VOLATILE.get(bank, ()) for i in range(off, off + n)):
            continue   # patched at runtime: interpret
        insn[off] = bytes(code[off:off + n])
    codemap = bytearray(1024)
    body = []
    bails = 0
    prev_set = set()
    for off in sorted(insn):
        b = insn[off]; nxt = off + len(b)
        # instruction directly after a SET?
        after_set = (off - 1) in insn and insn[off - 1][0] == 0xF4
        try:
            st, cyc, ex = translate_insn(off, b, after_set, nxt)
        except Bail as e:
            bails += 1
            continue
        for i in range(off, nxt): codemap[i >> 3] |= 1 << (i & 7)
        labels.append(off)
        body.append(f'L{off:04X}: /* {b.hex()} */')
        body.append(f'  c->ninstr++; cyc += {cyc};' + ('' if (after_set or ex == 'SET') else ' T = 0;'))
        for x in st: body.append('  ' + x)
        if after_set and ex != 'SET': body.append('  T = 0;')
        if ex == 'SET':
            if nxt in insn and nxt < 0x2000: continue
            body.append(f'  c->p |= F_T; c->pc = (uint16_t)(wb + 0x{nxt:04X}); return cyc;')
        elif ex:
            body.append('  ' + ex)
        else:
            if not (nxt in insn and nxt < 0x2000):
                body.append(f'  c->pc = (uint16_t)(wb + 0x{nxt:04X}); goto top;')
    out = []
    out.append(f'int {fn}(cpu6280 *c, int budget) {{')
    out.append('  int cyc = 0; int T = (c->p & F_T) != 0; c->p &= ~F_T;')
    out.append('  const uint16_t wb = c->pc & 0xE000; const uint32_t epoch = g_ovl_epoch;')
    out.append('top:')
    out.append(f'  if (T) {{ c->p |= F_T; return cyc; }}')
    out.append(f'  if (cyc >= budget || g_ovl_epoch != epoch || c->mpr[wb >> 13] != 0x{bank:02X} || (c->pc & 0xE000) != wb) return cyc;')
    out.append('  if (!(c->p & F_I) && (c->irq_pending & ~c->irq_mask)) return cyc;')
    out.append('  switch (c->pc & 0x1FFF) {')
    for off in labels: out.append(f'  case 0x{off:04X}: goto L{off:04X};')
    out.append('  default: return cyc;')
    out.append('  }')
    out += body
    out.append('}')
    return fn, '\n'.join(out), codemap, len(labels), bails

def main():
    ovl_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    vol = os.path.join(os.path.dirname(__file__), 'volatile.txt')
    for line in open(vol):
        line = line.split('#')[0].strip()
        if not line: continue
        b, rest = line.split(':'); o, n = (rest.split('+') + ['1'])[:2]
        VOLATILE.setdefault(int(b, 16), set()).update(range(int(o, 16), int(o, 16) + int(n)))
    table = []
    total = 0; tb = 0
    for covf in sorted(glob.glob(os.path.join(ovl_dir, 'cov_*.bin'))):
        key = os.path.basename(covf)[4:-4]
        bank = int(key[:2], 16); h = int(key[3:], 16)
        if bank in SKIP_BANKS: continue
        code = open(os.path.join(ovl_dir, f'bank_{key}.bin'), 'rb').read()
        cov = open(covf, 'rb').read()
        fn, src, codemap, n, bails = translate_overlay(bank, h, code, cov)
        total += n; tb += bails
        with open(os.path.join(out_dir, f'{fn}.c'), 'w') as f:
            f.write('#include "../recomp_rt.h"\n\n' + src + '\n')
        table.append((bank, h, fn, codemap))
    with open(os.path.join(out_dir, 'native_table.c'), 'w') as f:
        f.write('#include "../recomp_rt.h"\n\n')
        for bank, h, fn, _ in table: f.write(f'int {fn}(cpu6280 *c, int budget);\n')
        f.write('\nconst native_ovl_t g_native_table[] = {\n')
        for bank, h, fn, cm in table:
            f.write(f'  {{ 0x{bank:02X}, 0x{h:08X}u, {fn}, (const uint8_t[]){{' + ','.join(str(x) for x in cm) + '} },\n')
        f.write('};\n')
        f.write(f'const int g_native_count = {len(table)};\n')
        f.write('const volatile_ent_t g_volatile[] = {\n')
        for line in open(os.path.join(os.path.dirname(__file__), 'volatile.txt')):
            line = line.split('#')[0].strip()
            if not line: continue
            b, rest = line.split(':'); o, n = (rest.split('+') + ['1'])[:2]
            f.write(f'  {{ 0x{b}, 0x{o}, {n} }},\n')
        f.write('  { 0, 0, 0 }\n};\n')
    print(f'{len(table)} overlays, {total} instructions translated, {tb} bails')

if __name__ == '__main__':
    main()
