"""Minimal HuC6280 disassembler (65C02 + Rockwell bit ops + HuC6280 extensions)."""
import sys

# addressing modes -> (operand bytes, format)
M = {
 'imp':(0,''), 'acc':(0,'A'), 'imm':(1,'#${:02X}'), 'zp':(1,'${:02X}'), 'zpx':(1,'${:02X},X'),
 'zpy':(1,'${:02X},Y'), 'izp':(1,'(${:02X})'), 'izx':(1,'(${:02X},X)'), 'izy':(1,'(${:02X}),Y'),
 'abs':(2,'${:04X}'), 'abx':(2,'${:04X},X'), 'aby':(2,'${:04X},Y'), 'ind':(2,'(${:04X})'),
 'iax':(2,'(${:04X},X)'), 'rel':(1,'rel'), 'zrel':(2,'zrel'),
 'blk':(6,'blk'), 'timm_zp':(2,'tzp'), 'timm_zpx':(2,'tzpx'), 'timm_abs':(3,'tabs'), 'timm_abx':(3,'tabx'),
}
OP = {}
def d(op, mn, mode): OP[op] = (mn, mode)

# Standard 65C02 table
tbl = """
00 BRK imp|01 ORA izx|02 SXY imp|03 ST0 imm|04 TSB zp|05 ORA zp|06 ASL zp|08 PHP imp|09 ORA imm|0A ASL acc|0C TSB abs|0D ORA abs|0E ASL abs
10 BPL rel|11 ORA izy|12 ORA izp|13 ST1 imm|14 TRB zp|15 ORA zpx|16 ASL zpx|18 CLC imp|19 ORA aby|1A INC acc|1C TRB abs|1D ORA abx|1E ASL abx
20 JSR abs|21 AND izx|22 SAX imp|23 ST2 imm|24 BIT zp|25 AND zp|26 ROL zp|28 PLP imp|29 AND imm|2A ROL acc|2C BIT abs|2D AND abs|2E ROL abs
30 BMI rel|31 AND izy|32 AND izp|34 BIT zpx|35 AND zpx|36 ROL zpx|38 SEC imp|39 AND aby|3A DEC acc|3C BIT abx|3D AND abx|3E ROL abx
40 RTI imp|41 EOR izx|42 SAY imp|43 TMA imm|44 BSR rel|45 EOR zp|46 LSR zp|48 PHA imp|49 EOR imm|4A LSR acc|4C JMP abs|4D EOR abs|4E LSR abs
50 BVC rel|51 EOR izy|52 EOR izp|53 TAM imm|54 CSL imp|55 EOR zpx|56 LSR zpx|58 CLI imp|59 EOR aby|5A PHY imp|5D EOR abx|5E LSR abx
60 RTS imp|61 ADC izx|62 CLA imp|64 STZ zp|65 ADC zp|66 ROR zp|68 PLA imp|69 ADC imm|6A ROR acc|6C JMP ind|6D ADC abs|6E ROR abs
70 BVS rel|71 ADC izy|72 ADC izp|73 TII blk|74 STZ zpx|75 ADC zpx|76 ROR zpx|78 SEI imp|79 ADC aby|7A PLY imp|7C JMP iax|7D ADC abx|7E ROR abx
80 BRA rel|81 STA izx|82 CLX imp|83 TST timm_zp|84 STY zp|85 STA zp|86 STX zp|88 DEY imp|89 BIT imm|8A TXA imp|8C STY abs|8D STA abs|8E STX abs
90 BCC rel|91 STA izy|92 STA izp|93 TST timm_abs|94 STY zpx|95 STA zpx|96 STX zpy|98 TYA imp|99 STA aby|9A TXS imp|9C STZ abs|9D STA abx|9E STZ abx
A0 LDY imm|A1 LDA izx|A2 LDX imm|A3 TST timm_zpx|A4 LDY zp|A5 LDA zp|A6 LDX zp|A8 TAY imp|A9 LDA imm|AA TAX imp|AC LDY abs|AD LDA abs|AE LDX abs
B0 BCS rel|B1 LDA izy|B2 LDA izp|B3 TST timm_abx|B4 LDY zpx|B5 LDA zpx|B6 LDX zpy|B8 CLV imp|B9 LDA aby|BA TSX imp|BC LDY abx|BD LDA abx|BE LDX aby
C0 CPY imm|C1 CMP izx|C2 CLY imp|C3 TDD blk|C4 CPY zp|C5 CMP zp|C6 DEC zp|C8 INY imp|C9 CMP imm|CA DEX imp|CC CPY abs|CD CMP abs|CE DEC abs
D0 BNE rel|D1 CMP izy|D2 CMP izp|D3 TIN blk|D4 CSH imp|D5 CMP zpx|D6 DEC zpx|D8 CLD imp|D9 CMP aby|DA PHX imp|DD CMP abx|DE DEC abx
E0 CPX imm|E1 SBC izx|E3 TIA blk|E4 CPX zp|E5 SBC zp|E6 INC zp|E8 INX imp|E9 SBC imm|EA NOP imp|EC CPX abs|ED SBC abs|EE INC abs
F0 BEQ rel|F1 SBC izy|F2 SBC izp|F3 TAI blk|F4 SET imp|F5 SBC zpx|F6 INC zpx|F8 SED imp|F9 SBC aby|FA PLX imp|FD SBC abx|FE INC abx
"""
for line in tbl.strip().splitlines():
    for e in line.split('|'):
        op, mn, mode = e.split()
        d(int(op, 16), mn, mode)
for i in range(8):
    d(0x07 + i*0x10, f'RMB{i}', 'zp')
    d(0x87 + i*0x10, f'SMB{i}', 'zp')
    d(0x0F + i*0x10, f'BBR{i}', 'zrel')
    d(0x8F + i*0x10, f'BBS{i}', 'zrel')

def length(op):
    if op not in OP: return 1
    return 1 + M[OP[op][1]][0]

def dis1(mem, pc, base):
    """mem: bytes, pc: address of instruction, base: address of mem[0]"""
    i = pc - base
    op = mem[i]
    if op not in OP:
        return 1, f'.db ${op:02X}', None
    mn, mode = OP[op]
    n, fmt = M[mode]
    b = mem[i+1:i+1+n]
    if len(b) < n: return 1, f'.db ${op:02X}', None
    tgt = None
    if mode == 'rel':
        tgt = (pc + 2 + (b[0] - 256 if b[0] > 127 else b[0])) & 0xFFFF; s = f'${tgt:04X}'
    elif mode == 'zrel':
        tgt = (pc + 3 + (b[1] - 256 if b[1] > 127 else b[1])) & 0xFFFF; s = f'${b[0]:02X},${tgt:04X}'
    elif mode == 'blk':
        s = '${:04X},${:04X},${:04X}'.format(b[0]|b[1]<<8, b[2]|b[3]<<8, b[4]|b[5]<<8)
    elif mode.startswith('timm'):
        if mode in ('timm_zp','timm_zpx'):
            s = f'#${b[0]:02X},${b[1]:02X}' + (',X' if mode=='timm_zpx' else '')
        else:
            s = f'#${b[0]:02X},${b[1]|b[2]<<8:04X}' + (',X' if mode=='timm_abx' else '')
    elif n == 2:
        v = b[0] | b[1] << 8; s = fmt.format(v)
        if mn in ('JSR','JMP') and mode == 'abs': tgt = v
    elif n == 1:
        s = fmt.format(b[0])
    else:
        s = fmt
    return 1 + n, f'{mn} {s}'.rstrip(), tgt

def listing(mem, base, start, end):
    pc = start
    out = []
    while pc < end:
        n, txt, _ = dis1(mem, pc, base)
        raw = ' '.join(f'{x:02X}' for x in mem[pc-base:pc-base+n])
        out.append(f'{pc:04X}  {raw:<20} {txt}')
        pc += n
    return '\n'.join(out)

if __name__ == '__main__':
    f, base, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), int(sys.argv[4], 16)
    off = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0
    data = open(f, 'rb').read()[off:]
    print(listing(data, base, start, end))
