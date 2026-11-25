"""Generate C for a whole NSO module.

The reference for the C++ generator the app runs (runtime/gen/): tools/test_gen_cpp.py checks that both write the same
C. Not shipped; it stays until the C++ port has been released.

Usage: gen.py <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--simd asm|c] [--dyn-base] [--new-chunk-bounds]

--simd: FP/SIMD/CRC32 as AArch64 inline asm (default on arm64 hosts) or portable C (default elsewhere).
--new-chunk-bounds: cut the files by size and save those cuts to tools/chunk_bounds.py (every file compiles once).
"""
import argparse
import bisect
import os
import platform
import re
import struct
import sys
import time
from collections import Counter

import unicorn as uc
from unicorn import arm64_const as A

from analyze import eh_frame_functions
import chunk_bounds
import lifter
from lifter import CANON, Insn, Lifter, Unsupported, bits, emit_asm_batch, gaddr, hx, sext
from nso import Nso, R_AARCH64_RELATIVE

SCRATCH_MEM = 0x7300000000


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def fname(a):
    return f'f_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


class Module:
    def __init__(self, name, path, base, simd='asm'):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        self.words = struct.unpack(f'<{n.text[1] // 4}I', bytes(n.image[n.text[0]:n.text[0] + n.text[1]]))
        self.lifter = Lifter(base, simd)
        self.cache = {}
        self.stats = Counter()
        self.mu = None

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is None:
            try:
                ins = self.lifter.lift(self.word(a), a)
            except Unsupported as e:
                self.stats['unsupported'] += 1
                ins = Insn('trap', f'rt_udf(c, {gaddr(a)}); /* {e} */')
            self.cache[a] = ins
        return ins

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set()
        for a in eh_frame_functions(n):
            starts.add(b + a)
        for name, info, shndx, value, size in n.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(b + value)
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(b + add)
        starts = sorted(a for a in starts if self.in_text(a))
        return starts

    # ------------------------------------------------------------ jump tables

    def emu(self):
        if self.mu is None:
            print(f'{self.name}: starting the jump table emulator', file=sys.stderr, flush=True)
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            n = self.nso
            mu.mem_map(self.base, len(n.image))
            mu.mem_write(self.base, bytes(n.image))
            mu.mem_map(SCRATCH_MEM, 0x100000)
            mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
            self.mu = mu
        return self.mu

    def jump_table(self, br_pc, entry, end):
        """Resolve `br xN` targets from a bounds-checked jump table, or None."""
        r = self._jump_table_fallthrough(br_pc, entry, end)
        if r is None:
            r = self._jump_table_taken(br_pc, entry, end)
        return r

    def _index_regs(self, start, at, idx):
        """The index register plus registers holding the same value in start..at: sources it was
        copied/masked from, and copies of those (the table lookup sometimes recomputes the index
        from another copy after the bounds check)."""
        def copy_src(w):
            if (w & 0x7F800000) == 0x12000000:  # AND (imm)
                return bits(w, 5, 5)
            if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
                return bits(w, 16, 5)
            if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
                return bits(w, 5, 5)
            return None
        regs = {idx}
        changed = True
        while changed:
            changed = False
            for a in range(start, at, 4):
                w = self.word(a)
                rd, src = w & 31, copy_src(w)
                if src is None or src >= 31 or rd >= 31:
                    continue
                for x, y in ((rd, src), (src, rd)):
                    if x in regs and y not in regs:
                        regs.add(y)
                        changed = True
        return sorted(regs)

    def _set_index(self, mu, regs, i):
        for r in regs:
            mu.reg_write(A.UC_ARM64_REG_X0 + r if r < 29 else A.UC_ARM64_REG_X29, i)

    def _emulate_table(self, br_pc, entry, end, start, idx, count, set_at):
        """Run start..br for each index value (set at address set_at), collect targets."""
        mu = self.emu()
        rn = bits(self.word(br_pc), 5, 5)

        def step_to(frm, to):
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        targets = []
        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if set_at != start and not step_to(start, set_at):
                    return None
                self._set_index(mu, [idx], i)
                if not step_to(set_at, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    def _jump_table_taken(self, br_pc, entry, end):
        """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
        # start of the block holding the br
        s = br_pc
        while s - 4 >= entry and br_pc - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
            s -= 4
        for g in range(entry, end, 4):
            w = self.word(g)
            if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
                continue
            t = g + sext(bits(w, 5, 19), 19) * 4
            if t != s:
                continue
            cw = self.word(g - 4) if g - 4 >= entry else 0
            if (cw & 0x7F80001F) != 0x7100001F or bits(cw, 22, 1):
                continue
            limit = bits(cw, 10, 12)
            count = limit + 1 if bits(w, 0, 4) == 9 else limit
            if count <= 0 or count > 4096:
                continue
            return self._emulate_table(br_pc, entry, end, s, bits(cw, 5, 5), count, s)
        return None

    def _jump_table_fallthrough(self, br_pc, entry, end):
        rn = bits(self.word(br_pc), 5, 5)
        # find the guarding b.hi / b.hs and its cmp, scanning backwards
        a = br_pc - 4
        guard = cmp = None
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
                guard = a
                break
            if self.lift(a).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap'):
                return None
            a -= 4
        if guard is None:
            return None
        a = guard - 4
        while a >= entry and guard - a <= 4 * 8:
            w = self.word(a)
            # SUBS (imm) with Rd = zr, i.e. CMP #imm
            if (w & 0x7F80001F) == 0x7100001F:
                cmp = a
                break
            if self.lift(a).kind != 'op':
                return None
            a -= 4
        if cmp is None:
            return None
        w = self.word(cmp)
        if bits(w, 22, 1):
            return None
        limit = bits(w, 10, 12)
        count = limit + 1 if bits(self.word(guard), 0, 4) == 8 else limit
        if count <= 0 or count > 4096:
            return None
        idx = bits(w, 5, 5)
        # window start: back from cmp to the first control-flow instruction
        start = cmp
        while start - 4 >= entry and cmp - (start - 4) <= 4 * 12 and self.lift(start - 4).kind in ('op', 'asm'):
            start -= 4
        # (through the br: EoW's echo summon code, main+0x4367ac on 1.0.2, does `cmp w8, #12; b.hi; and x8, x0, #0xff`
        # and indexes with that x8, so x0 must hold the index too - with only w8 set, every value read the same case,
        # the others looked unreached and the orphan pass split the function there: issues #87, #89)
        idx_regs = self._index_regs(start, br_pc, idx)
        mu = self.emu()
        targets = []

        def step_to(frm, to):
            # single-step: Unicorn's `until` is unreliable mid translation-block
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                # replay address materialization (ADRP/ADR/ADD-imm) from the function entry, so table
                # bases computed before loops or branches are known
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if not step_to(start, cmp):
                    return None
                self._set_index(mu, idx_regs, i)
                if not step_to(cmp, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    # ------------------------------------------------------------ functions

    def computed_target(self, br_pc, entry):
        """True if the br register comes from address arithmetic (a table) rather than a load."""
        rn = bits(self.word(br_pc), 5, 5)
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if self.lift(a).kind not in ('op', 'asm'):
                return False
            if (w & 31) == rn:
                if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                    return True
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    return True
                return False
            a -= 4
        return False

    def scan(self, entry, end):
        """Collect labels and external entries referenced by a function."""
        labels, ext, tables = {entry}, set(), {}
        terminated = False
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if ins.kind in ('b', 'bcond'):
                t = ins.target
                if entry <= t < end:
                    labels.add(t)
                elif self.in_text(t):
                    ext.add(t)
            elif ins.kind == 'bl':
                if self.in_text(ins.target):
                    ext.add(ins.target)
            elif ins.kind == 'br':
                jt = self.jump_table(a, entry, end)
                if jt:
                    tables[a] = jt
                    labels.update(jt)
                    self.stats['jumptables'] += 1
                elif self.computed_target(a, entry):
                    # unresolved table jump: allow jumping to any instruction of the function
                    tables[a] = 'all'
                    self.stats['br_all_labels'] += 1
                else:
                    self.stats['br_dispatch'] += 1
            terminated = ins.kind in ('b', 'br', 'ret', 'trap')
        if not terminated and end < self.hi:
            ext.add(end)
        return labels, ext, tables

    def render_single(self, ins, a, jump, refs):
        """C for one lifted instruction (no batching); used for patch variants."""
        k = ins.kind
        if k == 'asm':
            return [emit_asm_batch([ins.asm])]
        if k in ('op', 'trap'):
            return [ins.code]
        if k == 'b':
            return [jump(ins.target)]
        if k == 'bcond':
            return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
        if k == 'bl':
            t = ins.target
            if self.in_text(t):
                refs.add(t)
                return [f'c->x[30] = {gaddr(a + 4)}; {fname(t)}(c);']
            return [f'c->x[30] = {gaddr(a + 4)}; c->pc = {gaddr(t)}; rt_dispatch(c);']
        if k == 'blr':
            return [f'c->pc = {ins.target}; c->x[30] = {gaddr(a + 4)}; rt_dispatch(c);']
        if k == 'br':
            return [f'c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);']
        if k == 'ret':
            return ['return;']
        raise Unsupported(k)

    def emit_function(self, entry, end, labels, tables, hle):
        out = [f'{"__attribute__((optnone)) " if entry in OPTNONE else ""}void {fname(entry)}(Cpu *c) {{']
        if entry in hle:
            out.append(f'    {sanitize(hle[entry])}(c);')
            out.append('}')
            return out
        out.append(f'    FN_ENTER({gaddr(entry)});')
        batch = []
        refs = set()

        def flush():
            if batch:
                out.append('    ' + emit_asm_batch(batch))
                batch.clear()

        def jump(t):
            if entry <= t < end:
                return f'goto L_{t:x};'
            if not self.in_text(t):
                return f'c->pc = {gaddr(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'

        # Functions with jump tables also get the all-instruction fallback: a table whose emulation
        # missed some cases then still reaches the right instruction instead of aborting.
        all_labels = bool(tables)
        if all_labels:
            labels = set(range(entry, end, 4))
        hooks_here = [a for a in range(entry, end, 4) if a in HOOKS]
        if hooks_here:  # (a hook's jump targets need their labels)
            labels = set(labels)
            for a in hooks_here:
                for t in HOOKS[a][1]:
                    if not entry <= t < end:
                        raise ValueError(f'hook {a:#x}: target {t:#x} is outside its function')
                    labels.add(t)
        last = None
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if a in labels:
                flush()
                out.append(f'L_{a:x}:;')
            if a in HOOKS:  # hook site (tools/patches.py HOOKS): the runtime decides, the instruction stays as it is
                flush()
                word, targets, _ = HOOKS[a]
                w = struct.unpack_from('<I', self.nso.image, a - self.base)[0]
                if w != word:
                    raise ValueError(f'hook {a:#x}: instruction {w:#010x}, expected {word:#010x}')
                body = ' '.join(self.render_single(ins, a, jump, refs))
                jumps = ' '.join(f'else if (h_ == {k + 2}) goto L_{t:x};' for k, t in enumerate(targets))
                out.append(f'    {{ int h_ = rt_hook(c, {gaddr(a)}); if (h_ == 0) {{ {body} }} {jumps} }}'
                           f'  /* {a:#x}: hook (runtime/hle/hooks.c) */')
                last = 'op'
                continue
            if a in PATCHES:  # hand-written replacement for one instruction (tools/patches.py)
                flush()
                out.append('    ' + rebase_patch(PATCHES[a](self, a)))
                last = 'op'
                continue
            k = ins.kind
            if k == 'asm':
                if len(batch) >= 24:
                    flush()
                batch.append(ins.asm)
                try:
                    emit_asm_batch(batch)
                except Unsupported:
                    batch.pop()
                    flush()
                    batch.append(ins.asm)
                last = k
                continue
            flush()
            if k == 'op' or k == 'trap':
                out.append('    ' + ins.code)
            elif k == 'b':
                out.append('    ' + jump(ins.target))
            elif k == 'bcond':
                out.append(f'    if ({ins.cond}) {{ {jump(ins.target)} }}')
            elif k == 'bl':
                t = ins.target
                if self.in_text(t):
                    refs.add(t)
                    out.append(f'    c->x[30] = {gaddr(a + 4)}; {fname(t)}(c);')
                else:
                    out.append(f'    c->x[30] = {gaddr(a + 4)}; c->pc = {gaddr(t)}; rt_dispatch(c);')
            elif k == 'blr':
                out.append(f'    c->pc = {ins.target}; c->x[30] = {gaddr(a + 4)}; rt_dispatch(c);')
            elif k == 'br':
                jt = tables.get(a)
                if jt == 'all':
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                if jt:
                    cases = ' '.join(f'case {case_key(t)}: goto L_{t:x};' for t in jt)
                    out.append(f'    switch ({switch_on(ins.target)}) {{ {cases} default: break; }}')
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
            elif k == 'ret':
                out.append('    return;')
            last = k
        flush()
        if last not in ('b', 'br', 'ret', 'trap'):
            out.append('    ' + jump(end) if end < self.hi else f'    rt_udf(c, {gaddr(end)});')
        if all_labels:
            out.append('L_dispatch:')
            cases = ' '.join(f'case {case_key(t)}: goto L_{t:x};' for t in range(entry, end, 4))
            out.append(f'    switch ({switch_on("c->pc")}) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        if lifter.DYN_BASE and any(GB_USE.search(l) for l in out):
            out.insert(1, '    const uint64_t GB = lasgame_gb;')
        self._refs = refs
        return out


# tools/patches.py, for the main module only (its sites are main addresses): loaded in main() once the game version
# is known from the NSO's build ID (tools/game_addresses.py; an unknown build ID stops the generator)
HOOKS, PATCHES = {}, {}

# --dyn-base (lifter.gaddr): jump table and dispatch switches compare window offsets, so their case labels stay
# constants; the hand-written patches' code addresses move with the window like the lifted ones.
GB_USE = re.compile(r'\bGB\b')
PATCH_ADDR = re.compile(r'\b0x(7[0-3][0-9a-f]{8})ull\b')


def case_key(t):
    return hx(t - CANON) if lifter.DYN_BASE else hx(t)


def switch_on(expr):
    return f'{expr} - GB' if lifter.DYN_BASE else expr


def rebase_patch(code):
    return PATCH_ADDR.sub(lambda m: gaddr(int(m.group(1), 16)), code) if lifter.DYN_BASE else code

# Functions built without optimisation: guest address -> its first instruction word (checked, as patches are).
# Workaround: LLVM's loop passes (LICM, then loop-load elimination) run for over half an hour and 7 GB on these at
# -O1, a cost no flag of theirs caps (-disable-licm-promotion below doesn't), and they don't run in normal play.
OPTNONE = {}


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def table_base_adr(m, i):
    """True if the ADR at word i is a jump table's base (newer compilers: adr xT, case0; ldrb wI, [table, idx];
    add xT, xT, wI, sxtw #2; br xT): its target is a case label, not a function, and must not split the function."""
    regs = {m.words[i] & 31}
    for w in m.words[i + 1:i + 9]:
        if (w & 0xFFFFFC1F) == 0xD61F0000:  # BR
            return bits(w, 5, 5) in regs
        if (w & 0x1F000000) == 0x0B000000 and bits(w, 5, 5) in regs:  # ADD/SUB (shifted or extended register)
            regs.add(w & 31)
        elif (w & 0x7C000000) == 0x14000000 or (w & 0xFE000000) in (0x54000000, 0xD6000000):  # other branches
            return False
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    # --new-chunk-bounds only: how the files are cut by size (with saved cuts, those decide; tools/chunk_bounds.py)
    ap.add_argument('--chunk', type=int, default=1500)  # functions per file at most
    ap.add_argument('--chunk-bytes', type=int, default=1500000)  # and about this much C (even compile times)
    ap.add_argument('--big-fn', type=int, default=100000)  # bytes of C from which a function gets its own file
    ap.add_argument('--new-chunk-bounds', action='store_true',
                    help='cut the files by size and save the cuts for this module build (tools/chunk_bounds.py)')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    ap.add_argument('--game', default='la')  # another game on the same engine (runtime/game_addresses/games.h): no
    # LA patches or hooks (all of them are LA addresses)
    ap.add_argument('--dyn-base', action='store_true',
                    help='Android: guest addresses relative to the window the runtime picks (lifter.gaddr)')
    args = ap.parse_args()
    lifter.DYN_BASE = args.dyn_base
    # (first: a generator that ends early still shows it started)
    print(f'{args.name}: started', file=sys.stderr, flush=True)

    global HOOKS, PATCHES
    t0 = time.time()
    m = Module(args.name, args.nso, args.base, args.simd)
    import game_addresses
    if args.game != 'la':
        if args.game not in game_addresses.GAMES:
            sys.exit(f'{args.name}: unknown game {args.game}')
        build = game_addresses.game_build(m.nso.build_id) if args.name == 'main' else None
        if args.name == 'main' and (not build or build['key'] != args.game):
            sys.exit(f'{args.name}: main build ID {m.nso.build_id[:16].upper()} is not {args.game}')
        print(f'{args.name}: game {game_addresses.GAMES[args.game]}' + (f" {build['release']}" if build else '') +
              ' (no LA patches)', file=sys.stderr)
        if args.name == 'main' and args.game == 'eow':
            # the build's own addresses (runtime/game_addresses/eow_<table>.h); each site asserts its instruction
            T = game_addresses.other_table(build)
            # (OPTNONE) a 25 500-line function behind a data table (1.0.2: main+0x3d85e90), never entered in 4 200 frames
            # of start, file load and play (LARECOMP_FNPROF); at -O1 it took 30+ minutes to compile
            OPTNONE[T['OPTNONE_TABLE_FN']] = 0xa9ba7bfd  # stp x29, x30, [sp, #-0x60]!
            # a 27 300-line function called once while the game starts (LARECOMP_FNPROF: once in boot to the overworld);
            # at -O2 it took 8 minutes on a Mac and 30+ on a Snapdragon 865 (Android builds it on the phone)
            OPTNONE[T['OPTNONE_SLOW_FN']] = 0xa9ba7bfd  # stp x29, x30, [sp, #-0x60]!
            for _a, _w in OPTNONE.items():
                _got = struct.unpack_from('<I', m.nso.image, _a - m.base)[0]
                assert _got == _w, f'OPTNONE {_a:#x}: {_got:#x}, not {_w:#x}'
            # (warp, runtime/hle/warp_other.c) the level-jump request processor (REQUEST_PROC, x0 = its object,
            # the request at +0x10): its `mov x20, x0` tells us where the request is
            def _eow_request(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert w == 0xaa0003f4, hex(w)  # mov x20, x0
                return ('{ extern void rt_other_request_holder(uint64_t); rt_other_request_holder(c->x[0]); c->x[20] = c->x[0]; }'
                        f'  /* {a:#x}: mov x20, x0 (level-jump request processor) */')
            PATCHES[T['REQUEST_SITE']] = _eow_request
            # (blur removal, runtime/hle/qol.c) the renderer's per-frame set-up makes the tilt-shift object
            # (renderer +0x600) once, the first time it's reached, as LA's main+0x1071220 does: name copy at +0x58,
            # uniform buffer at +0x50. Its `add x19, x23, #0x600` hands qol.c the object.
            def _eow_tilt(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert w == 0x911802f3, hex(w)  # add x19, x23, #0x600
                return ('{ extern void rt_tilt_obj(uint64_t); c->x[19] = c->x[23] + 0x600; rt_tilt_obj(c->x[19]); }'
                        f'  /* {a:#x}: add x19, x23, #0x600 (tilt-shift set-up: blur removal) */')
            PATCHES[T['TILT_SITE']] = _eow_tilt
            # (resolution, runtime/nvn/output_scale.c) the game's own dynamic resolution, off - as LA's
            # GAME_DYNRES_SITE. Its frame controller (1.0.2: main+0x346530) reads the scaler's enabled byte (+0x480 of its
            # object); on, it turns the GPU timing counters into a load figure and steps its 3D a level (1080 - 36 x
            # level lines) after 6 samples over 90% / under 70%. It started at 1408x792 and climbed
            # to 1920x1080 on a fast GPU: our scale (Match display maps 792 lines to the window) then drew the 3D at
            # up to 1.9x the window's pixels (6982x1964 for 5120x1440), new targets at each step, and frames over
            # 16.7 ms once our own window step (SMAA, upscale) came on top. Read as 0: it stays at 1408x792.
            # LARECOMP_GAME_DYNRES=1 (test): the game's own again.
            def _eow_dynres(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert w == 0x39520348, hex(w)  # ldrb w8, [x26, #0x480]
                return ('{ extern int rt_game_dynres(void); '
                        'c->x[8] = rt_game_dynres() ? LD8(c->x[26] + 0x480) : 0; }'
                        f'  /* {a:#x}: ldrb w8, [x26, #0x480] (the game\'s dynamic resolution: off) */')
            PATCHES[T['DYNRES_SITE']] = _eow_dynres
            # (frame pacing, runtime/nvn/pace.c rt_other_fps_limit) the frame controller's own 30 / 60 fps switch, off:
            # the frame time it calls slow, 1/60 s (`movk wD, #0x3c88, lsl #16` after `mov wD, #0x8889`) - in its 30 fps
            # branch (a 12-frame window with none slow goes back to 60), its 60 fps branch (slow frames there go to 30)
            # and the flag of a slow frame that sends it to 30 the next frame - is the runtime's: 1 s, no frame slow,
            # always 60 (0 with the FPS target at 30: every frame slow, its 30 fps mode)
            def _eow_fps_limit(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert (w & 0xFFFFFFE0) == 0x72A79100, hex(w)  # movk wD, #0x3c88, lsl #16
                d = w & 31
                return ('{ extern uint32_t rt_other_fps_limit(void); '
                        f'c->x[{d}] = rt_other_fps_limit(); }}'
                        f'  /* {a:#x}: movk w{d}, #0x3c88, lsl #16 (1/60 s: a slow frame) */')
            for a in (T['FPS_LIMIT_30_SITE'], T['FPS_LIMIT_60_SITE'], T['FPS_LIMIT_FLAG_SITE']):
                PATCHES[a] = _eow_fps_limit
            # (font, as LA's FONT_BOX_GLYPH_SITE, LAS #45) setting up a font (1.0.2: main+0x8ac1c), the game looks up
            # the glyph of "□" (U+25A1, its box for a missing character) and marks it (`ldrb w8, [x0, #0x38]` after the
            # lookup) without checking for none; its second lookup in the same function does check. A font the game
            # finds no such glyph in (issue #81: Windows 10's Yu Gothic, although its cmap lists it; or one set with
            # LARECOMP_FONT) crashed reading 0x38. No glyph now skips to the function's end, where the second lookup's
            # check goes.
            def _eow_font_box_glyph(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert w == 0x3940e008, hex(w)  # ldrb w8, [x0, #0x38]
                return (f"if (c->x[0] == 0) goto L_{T['FONT_BOX_GLYPH_SKIP']:x}; c->x[8] = LD8(c->x[0] + 0x38);"
                        f'  /* {a:#x}: ldrb w8, [x0, #0x38] (font box glyph: none skips the marking) */')
            PATCHES[T['FONT_BOX_GLYPH_SITE']] = _eow_font_box_glyph
            # (font, as LA's FONT_FALLBACK_SITES, LAS #452) a character the font has no glyph for is measured and drawn
            # as that same "□" (the font's [+0x34]): its width (1.0.2: main+0x8adac), its widths (main+0x8ae5c) and its
            # glyph (main+0x8af2c) read the box's glyph without checking for none. With a font whose "□" the game
            # can't use, the first missing character crashed there. No box glyph now reads as an empty one (no width,
            # nothing drawn).
            _font_fallback_ops = {0x79409800: ('ldrh w0, [x0, #0x4c]', 'c->x[0] = LD16(c->x[0] + 0x4c);'),
                                  0x39413809: ('ldrb w9, [x0, #0x4e]', 'c->x[9] = LD8(c->x[0] + 0x4e);'),
                                  0x79409816: ('ldrh w22, [x0, #0x4c]', 'c->x[22] = LD16(c->x[0] + 0x4c);'),
                                  0x79409c08: ('ldrh w8, [x0, #0x4e]', 'c->x[8] = LD16(c->x[0] + 0x4e);')}
            def _eow_font_fallback(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert w in _font_fallback_ops, hex(w)
                asm, op = _font_fallback_ops[w]
                return ('{ static uint8_t empty_glyph_[0x60] __attribute__((aligned(16))); '
                        'if (c->x[0] == 0) c->x[0] = (uint64_t)(uintptr_t)empty_glyph_; } ' + op +
                        f'  /* {a:#x}: {asm} (font fallback glyph: none reads as an empty one) */')
            for n in ('FONT_FALLBACK_SITE_0', 'FONT_FALLBACK_SITE_1', 'FONT_FALLBACK_SITE_2', 'FONT_FALLBACK_SITE_3'):
                PATCHES[T[n]] = _eow_font_fallback
            # (widescreen, runtime/hle/widescreen.c rt_aspect_other) the code builds 16:9 (0x3fe38e39) as
            # `movz wD, #0x8e39; movk wD, #0x3fe3, lsl #16`: the movk gets the runtime's aspect instead
            eow_aspect = [v for k, v in sorted(T.items()) if k.startswith('ASPECT_SITE_')]
            def _eow_aspect(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert (w & 0xFFFFFFE0) == 0x72A7FC60, hex(w)  # movk wD, #0x3fe3, lsl #16
                d = w & 31
                return (f'{{ extern uint32_t rt_aspect_other(int); c->x[{d}] = rt_aspect_other({eow_aspect.index(a)}); }}'
                        f'  /* {a:#x}: movk w{d}, #0x3fe3, lsl #16 (16:9) */')
            for a in eow_aspect:
                PATCHES[a] = _eow_aspect
            # (widescreen, runtime/hle/widescreen.c rt_screen_spot) layouts placed at a target's spot on screen (the
            # lock-on reticle L_LockOn_00 and the others below): the code works the spot out with the widened camera
            # and turns it into the 16:9 layout's units (x 960, y 540 from the centre), then stores it as the root
            # pane's translation, `stp sA, sB, [xN, #0x30]` = x, y. The runtime rescales them to the window's shape.
            eow_spot = [v for k, v in sorted(T.items()) if k.startswith('SPOT_SITE_')]
            def _eow_spot(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                assert (w & 0xFFFF8000) == 0x2D060000, hex(w)  # stp sA, sB, [xN, #0x30]
                ra, rn, rb = w & 31, (w >> 5) & 31, (w >> 10) & 31
                return (f'{{ extern void rt_screen_spot(int site, uint64_t pane, float *xy); float xy_[2]; '
                        f'memcpy(&xy_[0], &c->v[{ra}], 4); memcpy(&xy_[1], &c->v[{rb}], 4); '
                        f'rt_screen_spot({eow_spot.index(a)}, c->x[{rn}], xy_); memcpy((void *)(uintptr_t)(c->x[{rn}] + 0x30), xy_, 8); }}'
                        f'  /* {a:#x}: stp s{ra}, s{rb}, [x{rn}, #0x30] (a layout at a spot on screen: widescreen) */')
            for a in eow_spot:
                PATCHES[a] = _eow_spot
        if args.name == 'main' and os.environ.get('GEN_TRACE_STORE_OFF'):  # (debug) every `str xT, [xN, #off]` and
            # `stp` whose either slot is at one of these offsets (comma-separated): the runtime hears of it
            # (rt_trace_store: site, base, value) - finding where an object is registered
            offs = {int(o, 0) for o in os.environ['GEN_TRACE_STORE_OFF'].split(',')}
            def _reg(r):
                return 'c->sp' if r == 31 else f'c->x[{r}]'
            def _trace(gen, a):
                w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
                rn = (w >> 5) & 31
                base = _reg(rn)
                if (w & 0xFFC00000) == 0xF9000000:
                    off = ((w >> 10) & 0xFFF) * 8
                    pairs = [(off, w & 31)]
                else:  # stp (signed offset)
                    imm = (w >> 15) & 0x7F
                    off = (imm - 128 if imm & 64 else imm) * 8
                    pairs = [(off, w & 31), (off + 8, (w >> 10) & 31)]
                body = ''.join(f'*(uint64_t *)(uintptr_t)({base} + {o}) = {"0ull" if r == 31 else _reg(r)}; ' for o, r in pairs)
                body += 'uint64_t b_ = ' + base + '; '
                calls = ''.join(f'rt_trace_store({a:#x}ull, b_ + {o}, {"0ull" if r == 31 else _reg(r)}); ' for o, r in pairs if o in offs)
                return ('{ extern void rt_trace_store(uint64_t, uint64_t, uint64_t); ' + body + calls + '}'
                        f'  /* {a:#x}: traced store */')
            n = 0
            for i, w in enumerate(m.words):
                if (w & 0xFFC00000) == 0xF9000000 and ((w >> 10) & 0xFFF) * 8 in offs and ((w >> 5) & 31) != (w & 31):
                    PATCHES[m.lo + i * 4] = _trace
                    n += 1
                elif (w & 0xFFC00000) == 0xA9000000:
                    imm = (w >> 15) & 0x7F
                    off = (imm - 128 if imm & 64 else imm) * 8
                    if (off in offs or off + 8 in offs) and ((w >> 5) & 31) not in (w & 31, (w >> 10) & 31):
                        PATCHES[m.lo + i * 4] = _trace
                        n += 1
            print(f'{args.name}: {n} stores to {sorted(hex(o) for o in offs)} traced', file=sys.stderr)
    elif args.name == 'main':
        try:
            version = game_addresses.select_build_id(m.nso.build_id)
        except game_addresses.UnknownGameVersion as e:
            sys.exit(f'{args.name}: {e}')
        import patches
        HOOKS, PATCHES = patches.HOOKS, patches.PATCHES
        label, build_id = game_addresses.VERSIONS[version]
        print(f'{args.name}: game version {label} (build {build_id[:16]}), address table {version}', file=sys.stderr)
    starts = m.boundaries()
    hle = {}
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    # Partition text into functions; any branch/call target outside its function becomes a new
    # boundary (splitting the containing function) until a fixpoint is reached.
    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register.
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            if m.in_text(pc + imm) and not (args.game != 'la' and table_base_adr(m, i)):
                start_set.add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    start_set.add(t)
    scanned = {}
    rounds = 0
    orphans_done = args.game == 'la'
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][1] - start_set
        if not new and not orphans_done:
            # (another game) an address right after an unconditional branch / return that nothing in its function
            # reaches (no branch, no jump-table case) is a function of its own: EoW calls some only through pointers
            # built at run time - no relocation, ADR or eh_frame entry names them (crashes at main+0x181c250, then at
            # main+0x181c63c: not 16-byte aligned, a function the compiler placed right after a tail call's branch)
            orphans_done = True
            labels = set()
            for i, a in enumerate(order):
                end = order[i + 1] if i + 1 < len(order) else m.hi
                lab, ext, tables = scanned[(a, end)]
                labels |= lab
                if any(v == 'all' for v in tables.values()):
                    labels |= set(range(a, end, 4))  # (an unresolved table may reach anything in it)
            for i, w in enumerate(m.words):
                if (w & 0xFC000000) == 0x14000000 or (w & 0xFFFFFC1F) in (0xD61F0000, 0xD65F0000):
                    t = m.lo + (i + 1) * 4
                    if (i + 1 < len(m.words) and m.words[i + 1] not in (0, 0xD503201F)
                            and t not in labels and t not in start_set):
                        new.add(t)
            print(f'{args.name}: {len(new)} unreferenced functions added', file=sys.stderr)
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
    print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {dict(m.stats)}, '
          f'{time.time() - t0:.0f}s', file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    order = sorted(entries)
    if args.limit:
        order = order[:args.limit]

    def render(a):
        end, labels, tables = entries[a]
        lines = m.emit_function(a, end, labels, tables, hle)
        r = getattr(m, '_refs', set())
        m._refs = set()
        return a, lines, r

    def by_size():
        """Files of at most --chunk functions and about --chunk-bytes of C; a big function gets a file of its own
        (they compile in parallel). Makes the saved cuts; builds use it only for a module without them, since here
        where a cut falls depends on every function before it."""
        ci, pending = 0, None
        while ci < len(order) or pending:
            fns, size = [], 0
            while (pending or ci < len(order)) and len(fns) < args.chunk and size < args.chunk_bytes:
                if pending:
                    f, pending = pending, None
                else:
                    f = render(order[ci])
                    ci += 1
                n = sum(len(l) + 1 for l in f[1])
                if n > args.big_fn and fns:
                    pending = f
                    break
                fns.append(f)
                size += n
                if n > args.big_fn:
                    break
            yield fns

    def by_bounds(starts):
        """File k holds the functions from starts[k] up to starts[k + 1] (the first file also anything before
        starts[0]): a changed function changes only its own file, whatever happens before it."""
        k, fns = 0, []
        for a in order:
            j = max(bisect.bisect_right(starts, a) - 1, 0)
            while j > k:
                yield fns
                fns, k = [], k + 1
            fns.append(render(a))
        while k < len(starts):
            yield fns
            fns, k = [], k + 1

    key = m.nso.build_id[:16]
    saved = None if args.new_chunk_bounds else chunk_bounds.BOUNDS.get(key)
    if saved:
        chunks = by_bounds([args.base + o for o in saved[1]])
    else:
        if not args.new_chunk_bounds:
            print(f'{args.name}: no saved file cuts for build {key}, cutting by size', file=sys.stderr)
        chunks = by_size()
    nchunks = done = 0
    firsts = []
    for fns in chunks:
        body, refs = [], set()
        for a, lines, r in fns:
            body += lines
            refs |= r
        firsts.append(fns[0][0] if fns else None)
        done += len(fns)
        path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n' + ('#include "simd.h"\n' if m.lifter.sc else '')]
        if args.dyn_base:
            text += ['extern __attribute__((visibility("hidden"))) uint64_t lasgame_gb;\n']
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & {a for a, _, _ in fns})]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        # (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        print(f'{args.name}: progress {done}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    if args.new_chunk_bounds:
        what = args.name + (f' {game_addresses.VERSIONS[version][0]}' if args.name == 'main' else '')
        chunk_bounds.save(key, what, [a - args.base for a in firsts])
        print(f'{args.name}: saved {nchunks} file cuts for build {key} to tools/chunk_bounds.py', file=sys.stderr)
    for fn in os.listdir(args.outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(args.name) + r'_(\d{3})\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    import io
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_fn_count = {len(order)};\n')
        if args.dyn_base:  # (in each module's table, weak: main and sdk share one; the table keeps CANON addresses)
            f.write("// the guest window's start (--dyn-base): the runtime sets it before any game code runs\n")
            f.write(f'__attribute__((weak, visibility("hidden"))) uint64_t lasgame_gb = {hx(CANON)};\n')
            f.write('__attribute__((weak, visibility("default"))) void lasgame_set_guest_base(uint64_t gb) '
                    '{ lasgame_gb = gb; }\n')
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    if args.name == 'main':  # (what this code was generated from: the Makefile's game code library id, GAMELIB_BUILD_ID)
        write_if_changed(os.path.join(args.outdir, 'main_build_id.txt'),
                         (game_addresses.game_build(m.nso.build_id)['build_id'] if args.game != 'la' else
                          game_addresses.VERSIONS[version][1]) + '\n')
    print(f'{args.name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total', file=sys.stderr)


if __name__ == '__main__':
    main()
