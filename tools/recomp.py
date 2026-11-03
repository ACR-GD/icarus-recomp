#!/usr/bin/env python3
"""Static recompiler: Icarus / Droiyan ICARUS.EXE (Win32 PE, MSVC 5) -> C.

Model (same as the Quarantine recompiler this grew out of)
  * Registers, flags (lazy) and x87 state live in globals (runtime/rt.h).
  * Guest memory is one flat byte array MEM; the PE image sits at its preferred base 0x400000.
  * Every discovered function entry becomes `void f_XXXXXXXX(void)`. Calls push a return
    address on the guest stack and call the C function; `ret` pops and returns. Jumps to another
    entry are tail calls. Indirect branches go through rt_dispatch().
  * Imported Win32 functions and COM methods are "magic" addresses >= MAGIC_BASE that the
    dispatcher routes to host implementations (runtime/win32*.c).
usage: recomp.py ICARUS.EXE outdir
"""
import sys, os, struct, collections, re
from capstone import *
from capstone.x86 import *

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE

pe = PE(sys.argv[1])
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)
_, CB, _csz, _ = pe.section('.text')
CE = CB + _csz
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
def incode(a): return CB <= a < CE
def raw(a, n): return pe.bytes_at(a, n)

_cache = {}
def dis(a):
    if a in _cache: return _cache[a]
    try: i = next(md.disasm(raw(a, 16), a))
    except StopIteration: i = None
    _cache[a] = i
    return i

# ---------------------------------------------------------------- relocations
code_fix = {}          # position in .text holding an absolute pointer into .text
data_code_ptrs = []    # (pos, target) pointers into .text from other sections
for r in pe.relocs:
    t = pe.u32(r)
    if not incode(t): continue
    if incode(r): code_fix[r] = t
    else: data_code_ptrs.append((r, t))

# ---------------------------------------------------------------- recursive descent
jump_tables = {}
table_bytes = set()
def table_at(tbl):
    out = []; p = tbl
    # a few leading slots may be unused (memcpy's tail tables start at index 1)
    while p < tbl + 16 and p not in code_fix and (p + 4) in code_fix: table_bytes.update(range(p, p + 4)); p += 4
    while p in code_fix and incode(code_fix[p]):
        out.append(code_fix[p]); table_bytes.update(range(p, p + 4)); p += 4
    return out

NORETURN = set()   # filled below with import thunks that never return (ExitProcess...)
seen = set(); direct_calls = set(); branch_targets = set()
def explore(starts):
    work = list(starts)
    while work:
        a = work.pop()
        while incode(a) and a not in seen and a not in table_bytes:
            i = dis(a)
            if i is None: break
            seen.add(a)
            g = set(i.groups); m = i.mnemonic
            if X86_GRP_JUMP in g or X86_GRP_CALL in g:
                op = i.operands[0]
                if op.type == X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if X86_GRP_CALL in g: direct_calls.add(t)
                    else: branch_targets.add(t)
                    work.append(t)
                elif m.endswith('jmp') and op.type == X86_OP_MEM and op.mem.index and op.mem.scale == 4 \
                        and op.mem.base == 0 and incode(op.mem.disp & 0xffffffff):
                    tg = table_at(op.mem.disp & 0xffffffff)
                    jump_tables[a] = tg; branch_targets.update(tg); work.extend(tg)
                elif X86_GRP_CALL in g and op.type == X86_OP_MEM and not op.mem.base and not op.mem.index \
                        and (op.mem.disp & 0xffffffff) in NORETURN:
                    break
                if m.endswith('jmp'): break
            if m in ('ret', 'hlt'): break
            a += i.size

IAT = {a: (dll, name) for a, dll, name in pe.imports}
NORETURN.update(a for a, (d, n) in IAT.items() if n in ('ExitProcess', 'RaiseException', 'TerminateProcess'))
entry = pe.entry
explore([entry] + sorted(t for _, t in data_code_ptrs))
fptr_seeds = set(t for _, t in data_code_ptrs)
# .text relocations: only immediates (push offset f / mov [x], offset f) are code pointers;
# displacements point at data embedded in .text (jump tables, switch index bytes).
def reloc_kind(p):
    for back in range(1, 12):
        a = p - back
        if a in seen:
            i = dis(a)
            if a + i.size <= p: return None
            imm_off = i.imm_offset if hasattr(i, 'imm_offset') else None
            for op in i.operands:
                if op.type == X86_OP_IMM and (op.imm & 0xffffffff) == code_fix[p]: return 'imm'
            return 'disp'
    return None
pending = dict(code_fix)
while True:
    added = []
    for p, t in list(pending.items()):
        if p in table_bytes or t in table_bytes: del pending[p]; continue
        k = reloc_kind(p)
        if k is None: continue
        del pending[p]
        if k == 'imm' and t not in fptr_seeds: fptr_seeds.add(t); added.append(t)
    if not added: break
    explore(sorted(added))
# aligned function starts that nothing visibly references (called through computed pointers, or dead)
gap_seeds = []
for a in range(CB + 16, CE, 16):
    if a in seen or a in table_bytes or raw(a - 1, 1)[0] not in (0xcc, 0x90, 0xc3): continue
    if raw(a, 1)[0] in (0xcc, 0x00): continue
    if any(x in table_bytes for x in range(a, a + 8)): continue
    gap_seeds.append(a)
explore(gap_seeds)
fptr_seeds.update(a for a in gap_seeds if a in seen)
pending = {p: t for p, t in pending.items() if reloc_kind(p) is None and p not in table_bytes}
if pending: print('unclassified .text relocations:', ' '.join('%x->%x' % kv for kv in sorted(pending.items())[:20]))
# second pass: tables found late may have claimed some seeds
fptr_seeds = set(t for t in fptr_seeds if t not in table_bytes)
entries = set([entry]) | direct_calls | fptr_seeds
entries = set(e for e in entries if incode(e) and e in seen)
# incremental-link thunks: an entry that is just `jmp target` makes the target a function too
changed = True
while changed:
    changed = False
    for e in list(entries):
        i = dis(e)
        if i and i.mnemonic == 'jmp' and i.operands[0].type == X86_OP_IMM:
            t = i.operands[0].imm & 0xffffffff
            if incode(t) and t not in entries: entries.add(t); changed = True
smc_targets = set()

# ---------------------------------------------------------------- C emission helpers
REGS32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REGMAP = {}
for k, r in enumerate(REGS32):
    REGMAP[r] = (k, 32)
    REGMAP[r[1:]] = (k, 16)
for k, r in enumerate(['al', 'cl', 'dl', 'bl']): REGMAP[r] = (k, 8)
for k, r in enumerate(['ah', 'ch', 'dh', 'bh']): REGMAP[r] = (k, 'h')
SREGS = {'es': 0, 'cs': 1, 'ss': 2, 'ds': 3, 'fs': 4, 'gs': 5}

class Unsupported(Exception): pass

def regname(i, r): return i.reg_name(r)

def rreg(name):
    if name in SREGS: return 'S[%d]' % SREGS[name]
    if name not in REGMAP: raise Unsupported('reg ' + name)
    k, sz = REGMAP[name]
    if sz == 32: return 'R[%d]' % k
    if sz == 16: return '((uint16_t)R[%d])' % k
    if sz == 8: return '((uint8_t)R[%d])' % k
    return '((uint8_t)(R[%d]>>8))' % k

def wreg(name, v):
    if name in SREGS: return 'set_sreg(%d, (uint16_t)(%s));' % (SREGS[name], v)
    if name not in REGMAP: raise Unsupported('reg ' + name)
    k, sz = REGMAP[name]
    if sz == 32: return 'R[%d] = (uint32_t)(%s);' % (k, v)
    if sz == 16: return 'R[%d] = (R[%d] & 0xffff0000u) | (uint16_t)(%s);' % (k, k, v)
    if sz == 8: return 'R[%d] = (R[%d] & 0xffffff00u) | (uint8_t)(%s);' % (k, k, v)
    return 'R[%d] = (R[%d] & 0xffff00ffu) | ((uint32_t)(uint8_t)(%s) << 8);' % (k, k, v)

def ea(i, op, with_seg=True):
    m = op.mem
    parts = []
    addr16 = False
    if m.base:
        bn = regname(i, m.base)
        if bn in ('eip',): parts.append('0x%xu' % (i.address + i.size))
        else:
            if REGMAP[bn][1] == 16: addr16 = True
            parts.append(rreg(bn))
    if m.index:
        xn = regname(i, m.index)
        if REGMAP[xn][1] == 16: addr16 = True
        parts.append('%s*%d' % (rreg(xn), m.scale) if m.scale != 1 else rreg(xn))
    if m.disp or not parts: parts.append('0x%xu' % (m.disp & 0xffffffff))
    e = '(uint32_t)(' + ' + '.join(parts) + ')'
    if addr16: e = '(uint32_t)(uint16_t)(' + ' + '.join(parts) + ')'
    if not with_seg: return e
    if m.segment and regname(i, m.segment) == 'fs': return '(FS_BASE + %s)' % e
    return e

def opsize(op): return op.size * 8

def rd(i, op, sz=None):
    if op.type == X86_OP_REG: return rreg(regname(i, op.reg))
    if op.type == X86_OP_IMM:
        s = sz or opsize(op)
        return '0x%xu' % (op.imm & ((1 << s) - 1))
    if op.type == X86_OP_MEM:
        s = sz or opsize(op)
        return 'RD%d(%s)' % (s, ea(i, op))
    raise Unsupported('op type')

def wr(i, op, v, sz=None):
    if op.type == X86_OP_REG: return wreg(regname(i, op.reg), v)
    if op.type == X86_OP_MEM:
        s = sz or opsize(op)
        return 'WR%d(%s, %s);' % (s, ea(i, op), v)
    raise Unsupported('write op')

CONDS = {
    'o': 'get_of()', 'no': '!get_of()', 'b': 'cond_b()', 'ae': '!cond_b()', 'e': 'get_zf()', 'ne': '!get_zf()',
    'be': 'cond_be()', 'a': '!cond_be()', 's': 'get_sf()', 'ns': '!get_sf()', 'p': 'get_pf()', 'np': '!get_pf()',
    'l': 'cond_l()', 'ge': '!cond_l()', 'le': 'cond_le()', 'g': '!cond_le()',
}

ALU2 = {'add': 'alu_add', 'or': 'alu_or', 'adc': 'alu_adc', 'sbb': 'alu_sbb', 'and': 'alu_and',
        'sub': 'alu_sub', 'xor': 'alu_xor', 'cmp': 'alu_sub', 'test': 'alu_and'}
SHIFTS = {'shl': 'alu_shl', 'sal': 'alu_shl', 'shr': 'alu_shr', 'sar': 'alu_sar', 'rol': 'alu_rol',
          'ror': 'alu_ror', 'rcl': 'alu_rcl', 'rcr': 'alu_rcr'}

def is_far_indirect(i):
    b = bytes(i.bytes)
    k = 0
    while b[k] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2, 0xf3): k += 1
    return b[k] == 0xff and ((b[k+1] >> 3) & 7) in (3, 5)

# Entry hooks: C inserted at the top of a function, before its first instruction.
HOOKS = {
    # per-character text routine: replaced by runtime/text.c (anti-aliased proportional font)
    0x449f60: 'if (text_glyph_hook()) { RET(8); return; }',
    # sprite blitter: sprites drawn at the mouse position become the host (hardware) cursor
    0x44a7a0: 'if (sprite_cursor_hook()) { RET(0x10); return; }',
    # dialogue typewriter: optional faster text (ICARUS_TEXT_SPEED)
    0x44a0d0: 'text_speed_hook();',
}
# Idle points: heads of loops that only poll for time; the generated code yields there.
IDLE = set()

class Emitter:
    """Emits one function (or SMC region) body."""
    def __init__(self, name, entry_points, is_region=False):
        self.name = name; self.eps = entry_points; self.region = is_region
        self.body = {}; self.out = []; self.targets = set()
        work = list(entry_points)
        while work:
            a = work.pop()
            while incode(a) and a not in self.body and a not in table_bytes:
                if a != self.eps[0] and a in entries and not is_region: break
                i = dis(a)
                if i is None: break
                self.body[a] = i
                m = i.mnemonic; g = set(i.groups)
                if X86_GRP_JUMP in g:
                    op = i.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if self.local(t): self.targets.add(t); work.append(t)
                    elif a in jump_tables:
                        for t in jump_tables[a]:
                            if self.local(t): self.targets.add(t); work.append(t)
                    if m in ('jmp', 'ljmp', 'notrack jmp'): break
                if m in ('ret', 'retf', 'iretd', 'hlt'): break
                a += i.size

    def local(self, t):
        if not incode(t) or t in table_bytes: return False
        if self.region: return True
        return t == self.eps[0] or t not in entries

    def emit(self):
        o = self.out
        addrs = sorted(self.body)
        if self.region:
            o.append('void %s(uint32_t ep) {' % self.name)
            o.append('  switch (ep) {')
            for e in sorted(self.eps): o.append('  case 0x%x: goto L_%x;' % (e, e))
            o.append('  default: rt_fatal("bad region entry %%08x", ep);\n  }')
        else:
            o.append('void %s(void) {' % self.name)
            o.append('  FN_ENTER(0x%x);' % self.eps[0])
            if self.eps[0] in HOOKS: o.append('  ' + HOOKS[self.eps[0]])
        if not self.region and addrs and addrs[0] != self.eps[0]:
            o.append('  goto L_%x;' % self.eps[0])
        for k, a in enumerate(addrs):
            i = self.body[a]
            o.append('L_%x:' % a)
            if a in IDLE: o.append('  rt_idle();')
            if self.region and a in smc_targets:
                o.append('  if (MEM[0x%x] == 0xC3) { ESP += 4; return; }' % a)
            try:
                lines, falls = self.insn(i)
            except (Unsupported, KeyError, IndexError) as ex:
                lines, falls = ['rt_unimpl(0x%x, "%s %s (%s)");' % (a, i.mnemonic, i.op_str, ex)], False
            for l in lines: o.append('  ' + l)
            nxt = a + i.size
            if falls and not (k + 1 < len(addrs) and addrs[k + 1] == nxt):
                o.append('  ' + self.goto(nxt, a))
        o.append('}')
        # labels that are never jumped to are harmless but noisy -> keep (compiler ignores)
        return o

    def goto(self, t, cur):
        if t in self.body and (self.region or t == self.eps[0] or t not in entries):
            if t <= cur: return '{ IRQCHK(); goto L_%x; }' % t
            return 'goto L_%x;' % t
        if incode(t) and (t in entries):
            return '{ f_%08x(); return; }' % t
        if incode(t):
            return 'rt_unimpl(0x%x, "jump outside function to %08x");' % (cur, t)
        return 'rt_unimpl(0x%x, "jump to non-code %08x");' % (cur, t)

    def call_direct(self, t, nxt):
        if incode(t) and t in entries:
            return ['CALLF(f_%08x, 0x%xu);' % (t, nxt)]
        return ['CALLD(0x%xu, 0x%xu);' % (t, nxt)]

    def insn(self, i):
        m = i.mnemonic; ops = i.operands; a = i.address; nxt = a + i.size
        L = []
        if m.startswith('notrack '): m = m[8:]
        if m == 'nop' or m == 'wait' or m == 'fwait': return [], True
        if m == 'mov':
            d, s = ops
            if d.type == X86_OP_REG and regname(i, d.reg) in SREGS:
                return [wr(i, d, rd(i, s, 16))], True
            if s.type == X86_OP_REG and regname(i, s.reg) in SREGS:
                return [wr(i, d, rd(i, s), 16 if d.type == X86_OP_MEM else None)], True
            return [wr(i, d, rd(i, s))], True
        if m in ('movsx', 'movzx'):
            d, s = ops; ss, ds = opsize(s), opsize(d)
            if m == 'movzx': v = rd(i, s)
            else: v = '(uint32_t)(int32_t)(int%d_t)%s' % (ss, rd(i, s))
            return [wr(i, d, v)], True
        if m == 'lea':
            d, s = ops
            return [wr(i, d, ea(i, s, with_seg=False))], True
        if m == 'push':
            s = ops[0]
            if s.type == X86_OP_REG and regname(i, s.reg) in SREGS:
                return ['PUSH32(%s);' % rd(i, s)], True
            if s.type == X86_OP_IMM:
                if s.size == 2: return ['PUSH16(0x%xu);' % (s.imm & 0xffff)], True
                return ['PUSH32(0x%xu);' % (s.imm & 0xffffffff)], True
            if opsize(s) == 16: return ['PUSH16(%s);' % rd(i, s)], True
            if s.type == X86_OP_REG and regname(i, s.reg) == 'esp':
                return ['{ uint32_t t = ESP; PUSH32(t); }'], True
            return ['{ uint32_t t = %s; PUSH32(t); }' % rd(i, s)], True
        if m == 'pop':
            d = ops[0]
            if d.type == X86_OP_REG and regname(i, d.reg) in SREGS:
                return ['{ uint32_t t = POP32(); %s }' % wr(i, d, 't')], True
            if opsize(d) == 16: return ['{ uint32_t t = POP16(); %s }' % wr(i, d, 't')], True
            if d.type == X86_OP_MEM:  # address computed after esp increment
                return ['{ uint32_t t = POP32(); %s }' % wr(i, d, 't')], True
            return ['{ uint32_t t = POP32(); %s }' % wr(i, d, 't')], True
        if m in ALU2:
            d, s = ops; sz = opsize(d)
            fn = ALU2[m]
            sv = rd(i, s, sz) if s.type == X86_OP_IMM else rd(i, s)
            if s.type == X86_OP_IMM:
                sv = '0x%xu' % (s.imm & ((1 << sz) - 1))
            if m in ('cmp', 'test'):
                return ['%s(%s, %s, %d);' % (fn, rd(i, d), sv, sz)], True
            if m == 'xor' and d.type == X86_OP_REG and s.type == X86_OP_REG and d.reg == s.reg:
                return [wr(i, d, '0'), 'alu_xor(0, 0, %d);' % sz], True
            return ['{ uint32_t r = %s(%s, %s, %d); %s }' % (fn, rd(i, d), sv, sz, wr(i, d, 'r'))], True
        if m in ('inc', 'dec', 'neg', 'not'):
            d = ops[0]; sz = opsize(d)
            return ['{ uint32_t r = alu_%s(%s, %d); %s }' % (m, rd(i, d), sz, wr(i, d, 'r'))], True
        if m in SHIFTS:
            d = ops[0]; sz = opsize(d)
            cnt = '1' if len(ops) == 1 else rd(i, ops[1], 8)
            return ['{ uint32_t r = %s(%s, (%s) & 31, %d); %s }' % (SHIFTS[m], rd(i, d), cnt, sz, wr(i, d, 'r'))], True
        if m in ('shld', 'shrd'):
            d, s, c = ops; sz = opsize(d)
            return ['{ uint32_t r = alu_%s(%s, %s, (%s) & 31, %d); %s }' % (m, rd(i, d), rd(i, s), rd(i, c, 8), sz, wr(i, d, 'r'))], True
        if m == 'imul':
            if len(ops) == 1:
                return ['rt_imul1(%s, %d);' % (rd(i, ops[0]), opsize(ops[0]))], True
            if len(ops) == 2:
                d, s = ops; sz = opsize(d)
                return ['{ uint32_t r = alu_imul(%s, %s, %d); %s }' % (rd(i, d), rd(i, s), sz, wr(i, d, 'r'))], True
            d, s, c = ops; sz = opsize(d)
            return ['{ uint32_t r = alu_imul(%s, 0x%xu, %d); %s }' % (rd(i, s), c.imm & 0xffffffff, sz, wr(i, d, 'r'))], True
        if m in ('mul', 'div', 'idiv'):
            return ['rt_%s1(%s, %d);' % (m, rd(i, ops[0]), opsize(ops[0]))], True
        if m == 'cwde': return ['R[0] = (uint32_t)(int32_t)(int16_t)R[0];'], True
        if m == 'cbw': return [wreg('ax', '(uint16_t)(int16_t)(int8_t)R[0]')], True
        if m == 'cwd': return [wreg('dx', '((int16_t)R[0] < 0) ? 0xffff : 0')], True
        if m == 'cdq': return ['R[2] = ((int32_t)R[0] < 0) ? 0xffffffffu : 0;'], True
        if m == 'xchg':
            d, s = ops
            return ['{ uint32_t t1 = %s, t2 = %s; %s %s }' % (rd(i, d), rd(i, s), wr(i, d, 't2'), wr(i, s, 't1'))], True
        if m.startswith('set'):
            c = CONDS[m[3:]]
            return [wr(i, ops[0], '(%s) ? 1 : 0' % c)], True
        if m == 'bsr':
            d, s = ops
            return ['{ uint32_t v = %s; if (v) { %s set_zf(0); } else set_zf(1); }' % (rd(i, s), wr(i, d, '31 - __builtin_clz(v)'))], True
        # ---- control flow
        if m in ('jmp', 'call') and is_far_indirect(i):
            e = ea(i, ops[0])
            if m == 'jmp':
                return ['{ uint32_t p = %s; rt_farjmp(RD32(p), RD16(p + 4)); return; }' % e], False
            return ['{ uint32_t p = %s; rt_farcall(RD32(p), RD16(p + 4), 0x%xu); }' % (e, nxt)], True
        if m == 'jmp':
            op = ops[0]
            if op.type == X86_OP_IMM:
                return [self.goto(op.imm & 0xffffffff, a)], False
            if a in jump_tables:
                idx = rd(i, op).replace('RD32(', 'RD32(')
                L = ['switch (%s) {' % rd(i, op)]
                for t in sorted(set(jump_tables[a])):
                    L.append('  case 0x%xu: %s' % (t, self.goto(t, a)))
                L.append('  default: rt_dispatch_tail(%s); return; }' % rd(i, op))
                return L, False
            return ['rt_dispatch_tail(%s); return;' % rd(i, op)], False
        if m.startswith('j') and m[1:] in CONDS:
            t = ops[0].imm & 0xffffffff
            return ['if (%s) %s' % (CONDS[m[1:]], self.goto(t, a))], True
        if m in ('jecxz', 'jcxz'):
            t = ops[0].imm & 0xffffffff
            r = 'R[1]' if m == 'jecxz' else '(uint16_t)R[1]'
            return ['if (%s == 0) %s' % (r, self.goto(t, a))], True
        if m in ('loop', 'loope', 'loopne'):
            t = ops[0].imm & 0xffffffff
            c = {'loop': '', 'loope': ' && get_zf()', 'loopne': ' && !get_zf()'}[m]
            return ['if (--R[1] != 0%s) %s' % (c, self.goto(t, a))], True
        if m == 'call':
            op = ops[0]
            if op.type == X86_OP_IMM:
                return self.call_direct(op.imm & 0xffffffff, nxt), True
            return ['{ uint32_t t = %s; CALLD(t, 0x%xu); }' % (rd(i, op), nxt)], True
        if m == 'ret':
            n = ops[0].imm if ops else 0
            return ['RET(%d); return;' % n], False
        if m == 'retf':
            n = ops[0].imm if ops else 0
            return ['rt_retf(%d); return;' % n], False
        if m == 'iretd':
            return ['rt_iret(); return;'], False
        if m == 'leave': return ['ESP = EBP; EBP = POP32();'], True
        if m == 'enter':
            sz, lvl = ops[0].imm, ops[1].imm
            if lvl: raise Unsupported('enter level')
            return ['PUSH32(EBP); EBP = ESP; ESP -= %d;' % sz], True
        if m == 'pushal':
            return ['{ uint32_t t = ESP; PUSH32(R[0]); PUSH32(R[1]); PUSH32(R[2]); PUSH32(R[3]); PUSH32(t); PUSH32(R[5]); PUSH32(R[6]); PUSH32(R[7]); }'], True
        if m == 'popal':
            return ['R[7] = POP32(); R[6] = POP32(); R[5] = POP32(); ESP += 4; R[3] = POP32(); R[2] = POP32(); R[1] = POP32(); R[0] = POP32();'], True
        if m == 'pushfd': return ['PUSH32(get_eflags());'], True
        if m == 'popfd': return ['set_eflags(POP32());'], True
        if m == 'lahf': return [wreg('ah', 'get_eflags() & 0xd5')], True
        if m == 'sahf': return ['set_eflags((get_eflags() & ~0xd5u) | (%s & 0xd5));' % rreg('ah')], True
        if m == 'cmc': return ['set_cf(!get_cf());'], True
        if m == 'stc': return ['set_cf(1);'], True
        if m == 'clc': return ['set_cf(0);'], True
        if m == 'cld': return ['DF = 0;'], True
        if m == 'std': return ['DF = 1;'], True
        if m == 'cli': return ['IF = 0;'], True
        if m == 'sti': return ['IF = 1;'], True
        if m in ('les', 'lds', 'lfs', 'lgs', 'lss'):
            d, s = ops
            seg = SREGS[m[1:]]
            e = ea(i, s)
            return ['{ uint32_t p = %s; uint32_t o = RD32(p); uint16_t sel = RD16(p + 4); %s set_sreg(%d, sel); }' % (e, wr(i, d, 'o'), seg)], True
        # ---- string ops
        base = m.split()[-1]
        rep = m.split()[0] if ' ' in m else ''
        if base in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                    'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd', 'insb', 'insw', 'insd',
                    'outsb', 'outsw', 'outsd'):
            if base == 'movsd' and ops and ops[0].size == 8: raise Unsupported('sse movsd')
            sz = {'b': 8, 'w': 16, 'd': 32}[base[-1]]
            srcseg = 3
            for op in ops:
                if op.type == X86_OP_MEM and op.mem.segment and regname(i, op.mem.base) in ('esi', 'si'):
                    srcseg = SREGS[regname(i, op.mem.segment)]
            kind = base[:-1]
            pfx = bytes(i.bytes)[:4]
            k0 = 0
            while pfx[k0] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2, 0xf3): k0 += 1
            if not rep and 0xf3 in pfx[:k0]: rep = 'repe' if kind in ('scas', 'cmps') else 'rep'
            if not rep and 0xf2 in pfx[:k0]: rep = 'repne' if kind in ('scas', 'cmps') else 'rep'
            if kind in ('movs', 'stos', 'lods', 'ins', 'outs') and rep in ('rep', 'repne', 'repe'):
                rep = 'rep'
            return ['rt_str_%s(%d, %d, "%s"[0]);' % (kind, sz, srcseg, {'rep': 'r', 'repe': 'e', 'repne': 'n', '': '-'}[rep])], True
        if m in ('in', 'out'):
            if m == 'in':
                d, s = ops; sz = opsize(d)
                port = rd(i, s, 16) if s.type == X86_OP_REG else '0x%x' % s.imm
                return [wr(i, d, 'rt_in%d(%s)' % (sz, port))], True
            d, s = ops; sz = opsize(s)
            port = rd(i, d, 16) if d.type == X86_OP_REG else '0x%x' % d.imm
            return ['rt_out%d(%s, %s);' % (sz, port, rd(i, s))], True
        if m == 'int':
            return ['rt_int(0x%x);' % ops[0].imm], True
        if m in ('ljmp', 'lcall'):
            op = ops[0]
            if op.type == X86_OP_MEM:
                e = ea(i, op)
                if m == 'ljmp':
                    return ['{ uint32_t p = %s; rt_farjmp(RD32(p), RD16(p + 4)); return; }' % e], False
                return ['{ uint32_t p = %s; rt_farcall(RD32(p), RD16(p + 4), 0x%xu); }' % (e, nxt)], True
            raise Unsupported('direct far branch')
        if m == 'lsl':
            d, s = ops
            return ['{ uint32_t r; if (rt_lsl(%s, &r)) { %s set_zf(1); } else set_zf(0); }' % (rd(i, s, 16), wr(i, d, 'r'))], True
        if m == 'lar':
            d, s = ops
            return ['{ uint32_t r; if (rt_lar(%s, &r)) { %s set_zf(1); } else set_zf(0); }' % (rd(i, s, 16), wr(i, d, 'r'))], True
        if m == 'verr': return ['set_zf(rt_lsl(%s, 0));' % rd(i, ops[0], 16)], True
        if m == 'smsw': return [wr(i, ops[0], '0x0011')], True
        if m == 'aam':
            b = ops[0].imm if ops else 10
            return ['{ uint8_t al = (uint8_t)R[0]; uint8_t ah = al / %d; al %%= %d; R[0] = (R[0] & 0xffff0000u) | (ah << 8) | al; alu_or(al, 0, 8); }' % (b, b)], True
        if m.startswith('f'):
            return self.fpu(i, m, ops), True
        raise Unsupported('mnemonic')

    def fpu(self, i, m, ops):
        def memop(op):
            return ea(i, op), op.size
        if m in ('fld', 'fild'):
            op = ops[0]
            if op.type == X86_OP_REG: return ['fpu_push(ST(%d));' % (op.reg - X86_REG_ST0)]
            e, s = memop(op)
            k = {('fld', 4): 'f32', ('fld', 8): 'f64', ('fld', 10): 'f80', ('fild', 2): 'i16', ('fild', 4): 'i32', ('fild', 8): 'i64'}[(m, s)]
            return ['fpu_push(fpu_ld_%s(%s));' % (k, e)]
        if m in ('fst', 'fstp', 'fist', 'fistp'):
            op = ops[0]; pop = m.endswith('p')
            if op.type == X86_OP_REG:
                L = ['ST(%d) = ST(0);' % (op.reg - X86_REG_ST0)]
            else:
                e, s = memop(op)
                k = {('fst', 4): 'f32', ('fst', 8): 'f64', ('fst', 10): 'f80', ('fis', 2): 'i16', ('fis', 4): 'i32', ('fis', 8): 'i64'}[(m[:3] if m.startswith('fis') else 'fst', s)]
                L = ['fpu_st_%s(%s, ST(0));' % (k, e)]
            if pop: L.append('fpu_pop();')
            return L
        arith = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/', 'fsubr': 'r-', 'fdivr': 'r/'}
        bm = m.rstrip('p') if m not in ('fsubp', 'fsubrp') else m[:-1]
        if m in ('faddp', 'fsubp', 'fmulp', 'fdivp', 'fsubrp', 'fdivrp'): bm = m[:-1]
        if bm in arith or m in ('fiadd', 'fisub', 'fimul', 'fidiv'):
            pop = m in ('faddp', 'fsubp', 'fmulp', 'fdivp', 'fsubrp', 'fdivrp')
            opn = arith.get(bm) or {'fiadd': '+', 'fisub': '-', 'fimul': '*', 'fidiv': '/'}[m]
            def comb(x, y):
                if opn.startswith('r'): return '(%s) %s (%s)' % (y, opn[1], x)
                return '(%s) %s (%s)' % (x, opn, y)
            if m.startswith('fi'):
                e, s = memop(ops[0])
                return ['ST(0) = %s;' % comb('ST(0)', 'fpu_ld_i%d(%s)' % (s*8, e))]
            if len(ops) == 1 and ops[0].type == X86_OP_MEM:
                e, s = memop(ops[0])
                return ['ST(0) = %s;' % comb('ST(0)', 'fpu_ld_f%d(%s)' % (s*8, e))]
            if len(ops) == 0:
                d, s = 1, 0
            elif len(ops) == 1:
                d, s = 0, ops[0].reg - X86_REG_ST0
            else:
                d, s = ops[0].reg - X86_REG_ST0, ops[1].reg - X86_REG_ST0
            L = ['ST(%d) = %s;' % (d, comb('ST(%d)' % d, 'ST(%d)' % s))]
            if pop: L.append('fpu_pop();')
            return L
        if m in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp'):
            if ops and ops[0].type == X86_OP_MEM:
                e, s = memop(ops[0]); other = 'fpu_ld_f%d(%s)' % (s*8, e)
            elif ops: other = 'ST(%d)' % (ops[0].reg - X86_REG_ST0)
            else: other = 'ST(1)'
            L = ['fpu_compare(ST(0), %s);' % other]
            if m.endswith('pp'): L += ['fpu_pop();', 'fpu_pop();']
            elif m.endswith('p'): L.append('fpu_pop();')
            return L
        if m == 'fnstsw':
            if ops[0].type == X86_OP_REG: return [wreg('ax', 'fpu_sw()')]
            return ['WR16(%s, fpu_sw());' % ea(i, ops[0])]
        if m == 'fnstcw': return ['WR16(%s, FPU_CW);' % ea(i, ops[0])]
        if m == 'fldcw': return ['FPU_CW = RD16(%s);' % ea(i, ops[0])]
        if m == 'fldz': return ['fpu_push(0.0);']
        if m == 'fld1': return ['fpu_push(1.0);']
        if m == 'fldlg2': return ['fpu_push(0.301029995663981195);']
        if m == 'fchs': return ['ST(0) = -ST(0);']
        if m == 'fabs': return ['ST(0) = fabs(ST(0));']
        if m == 'fsqrt': return ['ST(0) = sqrt(ST(0));']
        if m == 'frndint': return ['ST(0) = fpu_round(ST(0));']
        if m == 'fyl2x': return ['ST(1) = ST(1) * log2(ST(0)); fpu_pop();']
        if m == 'fxch':
            r = ops[0].reg - X86_REG_ST0 if ops else 1
            return ['{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (r, r)]
        if m in ('fninit', 'finit'): return ['fpu_init();']
        if m in ('fnsave', 'fsave'): return ['fpu_save(%s);' % ea(i, ops[0])]
        if m == 'frstor': return ['fpu_restore(%s);' % ea(i, ops[0])]
        raise Unsupported('fpu')

# ---------------------------------------------------------------- generate
funcs = sorted(entries)
chunks = collections.defaultdict(list)
CHUNK = 80
unimpl = collections.Counter()
for k, e in enumerate(funcs):
    em = Emitter('f_%08x' % e, [e])
    lines = em.emit()
    for l in lines:
        if 'rt_unimpl' in l: unimpl[l.split('"')[1].split(' ')[0]] += 1
    chunks[k // CHUNK].append('\n'.join(lines))

hdr = '#include "../runtime/rt.h"\n#include "funcs.h"\n'
for f in os.listdir(OUT):
    if f.startswith('code_') and f.endswith('.c'): os.remove(os.path.join(OUT, f))
for k, src in chunks.items():
    open(os.path.join(OUT, 'code_%03d.c' % k), 'w').write(hdr + '\n\n'.join(src) + '\n')
with open(os.path.join(OUT, 'funcs.h'), 'w') as f:
    f.write('#pragma once\n')
    for e in funcs: f.write('void f_%08x(void);\n' % e)
with open(os.path.join(OUT, 'table.c'), 'w') as f:
    f.write(hdr)
    f.write('const struct fentry FUNCS[] = {\n')
    for e in funcs: f.write('  {0x%x, f_%08x},\n' % (e, e))
    f.write('};\nconst int NFUNCS = %d;\n' % len(funcs))
    f.write('const uint32_t GUEST_ENTRY = 0x%x;\n' % entry)
    f.write('const struct import_entry IMPORTS[] = {\n')
    for a, dll, name in pe.imports: f.write('  {0x%x, "%s", "%s"},\n' % (a, dll, name))
    f.write('};\nconst int NIMPORTS = %d;\n' % len(pe.imports))
with open(os.path.join(OUT, 'image.bin'), 'wb') as f:
    f.write(struct.pack('<II', pe.base, len(pe.image)))
    f.write(bytes(pe.image))
with open(os.path.join(OUT, 'image_info.h'), 'w') as f:
    f.write('#define IMAGE_BASE 0x%x\n#define IMAGE_SIZE 0x%x\n' % (pe.base, len(pe.image)))
    f.write('#define TEXT_LO 0x%x\n#define TEXT_HI 0x%x\n' % (CB, CE))

print('functions %d, jump tables %d' % (len(funcs), len(jump_tables)))
print('instructions covered: %d' % len(seen))
print('unimplemented sites:', unimpl.most_common(40))
