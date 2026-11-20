// A small AArch64 evaluator for the generator's jump-table resolution (replaces the Unicorn engine gen.py steps).
//
// gen.py runs the instructions between a jump table's bounds check and its `br` once per index value and reads the
// branch target. This evaluator runs them with the semantics of the Arm Architecture Reference Manual and keeps the
// state the Python engine keeps between tables (one engine per module): memory writes persist, as do x29, x30 and the
// flags (gen.py resets x0..x28 and sp before each index). The windows are integer code (address arithmetic, the table
// load, branches) with some FP work mixed in. That FP work is computed as Unicorn does (scalar arithmetic, the common
// AdvSIMD forms, FRECPE / FRSQRTE's estimates), since what it leaves in memory can decide a later table (EoW): the
// rest is "unknown" (see emu.hpp), which is enough as long as it doesn't decide the target. Instructions it does not
// run (system registers, exclusives, svc) stop the table, as an engine error does in gen.py. LASGEN_EMU_DEBUG=1 logs
// every stop.
#include "emu.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace lasgen {

Emu::Emu(const Nso &nso, uint64_t base_) : base(base_), image(nso.image), scratch(SCRATCH_SIZE, 0) {
    debug = getenv("LASGEN_EMU_DEBUG") != nullptr;
    for (bool &k : xk) k = true;
    for (bool &k : vknown) k = true;
}

void Emu::unsupported(uint32_t w) {
    if (debug) fprintf(stderr, "[emu] unsupported %08x at %llx\n", w, (unsigned long long)pc);
    throw EmuFault();
}

void Emu::need_known(const char *what) {
    if (!unk) return;
    if (debug) fprintf(stderr, "[emu] unknown %s at %llx\n", what, (unsigned long long)pc);
    throw EmuFault();
}

// ---------------------------------------------------------------- memory

uint8_t *Emu::mem(uint64_t a, size_t n) {
    if (a >= base && a - base <= image.size() && n <= image.size() - (a - base)) return &image[a - base];
    if (a >= SCRATCH_MEM && a - SCRATCH_MEM <= SCRATCH_SIZE && n <= SCRATCH_SIZE - (a - SCRATCH_MEM))
        return &scratch[a - SCRATCH_MEM];
    if (debug) fprintf(stderr, "[emu] unmapped %llx at %llx\n", (unsigned long long)a, (unsigned long long)pc);
    throw EmuFault();
}

uint8_t *Emu::wmem(uint64_t a, size_t n) {
    uint8_t *p = mem(a, n);
    if (forking)
        for (size_t i = 0; i < n; i++) undo.emplace(a + i, std::make_pair(p[i], tainted.count(a + i) != 0));
    return p;
}

// ---------------------------------------------------------------- unknown flags: each outcome

Emu::Regs Emu::regs() const {
    Regs r;
    memcpy(r.x, x, sizeof x), memcpy(r.xk, xk, sizeof xk), memcpy(r.v, v, sizeof v), memcpy(r.vknown, vknown, sizeof vknown);
    r.sp = sp, r.nzcv = nzcv, r.spk = spk, r.flags_known = flags_known;
    return r;
}

void Emu::set_regs(const Regs &r) {
    memcpy(x, r.x, sizeof x), memcpy(xk, r.xk, sizeof xk), memcpy(v, r.v, sizeof v), memcpy(vknown, r.vknown, sizeof vknown);
    sp = r.sp, nzcv = r.nzcv, spk = r.spk, flags_known = r.flags_known;
}

void Emu::rollback() {
    for (auto &e : undo) {
        *mem(e.first, 1) = e.second.first;
        taint(e.first, 1, e.second.second);
    }
    undo.clear();
    set_regs(snap);
}

void Emu::begin_paths() {
    snap = regs();
    undo.clear();
    forking = true;
    choices.clear(), next_choice = 0;
}

void Emu::retry(const std::vector<bool> &c) {
    rollback();
    choices = c, next_choice = 0;
}

Emu::PathEnd Emu::path_end() const {
    PathEnd e{regs(), {}};
    for (auto &u : undo) {
        uint64_t a = u.first;
        const uint8_t *p = a >= SCRATCH_MEM && a - SCRATCH_MEM < SCRATCH_SIZE ? &scratch[a - SCRATCH_MEM] : &image[a - base];
        e.mem.push_back({a, {*p, tainted.count(a) != 0}});
    }
    return e;
}

void Emu::end_paths(const std::vector<PathEnd> &ends) {
    forking = false;
    if (ends.empty()) {  // (a plain run: its state stays)
        undo.clear();
        return;
    }
    rollback();
    // registers: a value every run left the same, else unknown
    Regs j = ends[0].r;
    for (auto &e : ends) {
        for (int i = 0; i < 31; i++)
            if (!e.r.xk[i] || e.r.x[i] != j.x[i]) j.xk[i] = false;
        for (int i = 0; i < 32; i++)
            if (!e.r.vknown[i] || e.r.v[i] != j.v[i]) j.vknown[i] = false;
        if (!e.r.spk || e.r.sp != j.sp) j.spk = false;
        if (!e.r.flags_known || e.r.nzcv != j.nzcv) j.flags_known = false;
    }
    set_regs(j);
    // memory: each byte any run wrote, as every run left it (one that didn't write it left it as it was), else unknown
    std::unordered_map<uint64_t, std::pair<uint8_t, bool>> after;
    std::unordered_set<uint64_t> differ;
    for (size_t k = 0; k < ends.size(); k++)
        for (auto &m : ends[k].mem) after.emplace(m.first, m.second);
    for (auto &a : after) {
        uint8_t before = *mem(a.first, 1);
        bool before_t = tainted.count(a.first) != 0;
        for (auto &e : ends) {
            std::pair<uint8_t, bool> got = {before, before_t};
            for (auto &m : e.mem)
                if (m.first == a.first) got = m.second;
            if (got != a.second) differ.insert(a.first);
        }
    }
    for (auto &a : after) {
        *mem(a.first, 1) = a.second.first;
        taint(a.first, 1, a.second.second || differ.count(a.first));
    }
}

bool Emu::unknown_flags_cond(int cond) {
    if (!forking) {
        if (debug) fprintf(stderr, "[emu] condition on unknown flags at %llx\n", (unsigned long long)pc);
        throw EmuFault();
    }
    (void)cond;
    if (next_choice >= choices.size()) throw EmuFork();
    return choices[next_choice++];
}

bool Emu::is_tainted(uint64_t a, int size) const {
    if (tainted.empty()) return false;
    for (int i = 0; i < size; i++)
        if (tainted.count(a + i)) return true;
    return false;
}

void Emu::taint(uint64_t a, int size, bool unknown) {
    for (int i = 0; i < size; i++)
        if (unknown) tainted.insert(a + i);
        else if (!tainted.empty()) tainted.erase(a + i);
}

uint64_t Emu::load(uint64_t a, int size, bool &known) {
    uint8_t *p = mem(a, size);
    uint64_t v = 0;
    memcpy(&v, p, size);
    known = !is_tainted(a, size);
    return v;
}

void Emu::store(uint64_t a, int size, uint64_t v, bool known) {
    memcpy(wmem(a, size), &v, size);
    taint(a, size, !known);
}

void Emu::vload(int r, uint64_t a, int size) {
    uint8_t *p = mem(a, size);
    unsigned __int128 val = 0;
    memcpy(&val, p, size);
    v[r] = val;
    vknown[r] = !is_tainted(a, size);
}

void Emu::vstore(int r, uint64_t a, int size) {
    memcpy(wmem(a, size), &v[r], size);
    taint(a, size, !vknown[r]);
}

// ---------------------------------------------------------------- registers

uint64_t Emu::xr(int r) {  // zr
    if (r == 31) return 0;
    unk |= !xk[r];
    return x[r];
}

uint64_t Emu::xsp(int r) {  // sp
    if (r == 31) {
        unk |= !spk;
        return sp;
    }
    return xr(r);
}

void Emu::wxk(int r, uint64_t v, bool known) {
    if (r != 31) x[r] = v, xk[r] = known;
}

void Emu::wxspk(int r, uint64_t v, bool known) {
    if (r == 31) sp = v, spk = known;
    else x[r] = v, xk[r] = known;
}

static uint64_t mask_of(int sf) { return sf ? ~0ull : 0xFFFFFFFFull; }

// AddWithCarry, returning the result and setting NZCV when flags
uint64_t Emu::add_with_carry(int sf, uint64_t a, uint64_t b, int carry, bool flags) {
    uint64_t m = mask_of(sf);
    a &= m, b &= m;
    uint64_t r;
    int N, Z, C, V;
    if (sf) {
        unsigned __int128 u = (unsigned __int128)a + b + carry;
        __int128 s = (__int128)(int64_t)a + (__int128)(int64_t)b + carry;
        r = (uint64_t)u;
        N = (int)(r >> 63), Z = r == 0, C = (u >> 64) != 0, V = (__int128)(int64_t)r != s;
    } else {
        uint64_t u = a + b + (uint64_t)carry;
        int64_t s = (int64_t)(int32_t)a + (int64_t)(int32_t)b + carry;
        r = u & m;
        N = (int)(r >> 31) & 1, Z = r == 0, C = (u >> 32) != 0, V = (int64_t)(int32_t)r != s;
    }
    if (flags) set_flags((uint64_t)(N << 3 | Z << 2 | C << 1 | V) << 28);
    return r;
}

void Emu::logic_flags(int sf, uint64_t r) {
    int N = (int)(sf ? r >> 63 : (r >> 31) & 1), Z = (r & mask_of(sf)) == 0;
    set_flags((uint64_t)(N << 3 | Z << 2) << 28);
}

bool Emu::cond_holds(int cond) {
    if (!flags_known) return unknown_flags_cond(cond);
    int N = (int)(nzcv >> 31) & 1, Z = (int)(nzcv >> 30) & 1, C = (int)(nzcv >> 29) & 1, V = (int)(nzcv >> 28) & 1;
    bool r;
    switch (cond >> 1) {
        case 0: r = Z; break;
        case 1: r = C; break;
        case 2: r = N; break;
        case 3: r = V; break;
        case 4: r = C && !Z; break;
        case 5: r = N == V; break;
        case 6: r = N == V && !Z; break;
        default: r = true; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

static uint64_t ror_n(uint64_t v, int r, int size) {
    r %= size;
    if (!r) return v;
    return ((v >> r) | (v << (size - r))) & mask_of(size == 64);
}

static uint64_t shift_reg(int sf, uint64_t v, int typ, int amt) {
    int size = sf ? 64 : 32;
    v &= mask_of(sf);
    switch (typ) {
        case 0: return (v << amt) & mask_of(sf);
        case 1: return v >> amt;
        case 2: return sf ? (uint64_t)((int64_t)v >> amt) : (uint64_t)(uint32_t)((int32_t)(uint32_t)v >> amt);
        default: return ror_n(v, amt, size);
    }
}

static uint64_t extend_reg(uint64_t v, int option, int shift) {
    switch (option) {
        case 0: v = (uint8_t)v; break;
        case 1: v = (uint16_t)v; break;
        case 2: v = (uint32_t)v; break;
        case 4: v = (uint64_t)(int64_t)(int8_t)v; break;
        case 5: v = (uint64_t)(int64_t)(int16_t)v; break;
        case 6: v = (uint64_t)(int64_t)(int32_t)v; break;
        default: break;
    }
    return v << shift;
}

static int bit_len(uint64_t x) {
    int n = 0;
    while (x) n++, x >>= 1;
    return n;
}

// DecodeBitMasks; false for reserved encodings (UNDEFINED)
static bool bit_masks(int N, int imms, int immr, int size, bool immediate, uint64_t &wmask, uint64_t &tmask) {
    int len = bit_len((uint64_t)((N << 6) | (~imms & 0x3F))) - 1;
    if (len < 1) return false;
    int levels = (1 << len) - 1;
    if (immediate && (imms & levels) == levels) return false;
    int S = imms & levels, R = immr & levels, d = (S - R) & levels;
    int esize = 1 << len;
    uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    uint64_t welem = S + 1 >= 64 ? ~0ull : (1ull << (S + 1)) - 1;
    uint64_t telem = d + 1 >= 64 ? ~0ull : (1ull << (d + 1)) - 1;
    uint64_t wr = R ? (((welem >> R) | (welem << (esize - R))) & emask) : welem;
    wmask = tmask = 0;
    for (int i = 0; i < size; i += esize) wmask |= wr << i, tmask |= telem << i;
    return true;
}

// ---------------------------------------------------------------- one instruction

void Emu::step(uint64_t at) {
    pc = at;
    unk = false;
    bool known;
    uint32_t w = (uint32_t)load(pc, 4, known);
    if (!known) unsupported(w);  // (code overwritten with an unknown value)
    uint64_t next = pc + 4;
    int op0 = (int)bits(w, 25, 4);
    int sf = (int)bits(w, 31, 1);
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
    uint64_t m = mask_of(sf);

    if ((op0 & 0b1110) == 0b1000) {  // data processing (imm)
        int op = (int)bits(w, 23, 3);
        if (op == 0 || op == 1) {  // ADR / ADRP
            int64_t imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21);
            wx(rd, bits(w, 31, 1) ? (pc & ~0xFFFull) + ((uint64_t)imm << 12) : pc + (uint64_t)imm);
        } else if (op == 2) {  // add/sub imm
            int opc = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
            uint64_t imm = bits(w, 10, 12) << (bits(w, 22, 1) ? 12 : 0);
            uint64_t a = xsp(rn);
            uint64_t r = opc ? add_with_carry(sf, a, ~imm, 1, S) : add_with_carry(sf, a, imm, 0, S);
            if (S) wx(rd, r);
            else wxsp(rd, r);
        } else if (op == 4) {  // logical imm
            int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
            uint64_t wm, tm;
            if ((!sf && N) || !bit_masks(N, (int)bits(w, 10, 6), (int)bits(w, 16, 6), sf ? 64 : 32, true, wm, tm))
                unsupported(w);
            uint64_t a = xr(rn) & m, r;
            if (opc == 0) r = a & wm;
            else if (opc == 1) r = a | wm;
            else if (opc == 2) r = a ^ wm;
            else r = a & wm, logic_flags(sf, r);
            r &= m;
            if (opc == 3) wx(rd, r);
            else wxsp(rd, r);
        } else if (op == 5) {  // move wide
            int opc = (int)bits(w, 29, 2), hw = (int)bits(w, 21, 2);
            if ((!sf && hw > 1) || opc == 1) unsupported(w);
            uint64_t imm = bits(w, 5, 16) << (hw * 16);
            if (opc == 0) wx(rd, ~imm & m);
            else if (opc == 2) wx(rd, imm);
            else {
                uint64_t old = xr(rd);
                wx(rd, ((old & ~(0xFFFFull << (hw * 16))) | imm) & m);
            }
        } else if (op == 6) {  // bitfield
            int opc = (int)bits(w, 29, 2), N = (int)bits(w, 22, 1);
            int immr = (int)bits(w, 16, 6), imms = (int)bits(w, 10, 6), size = sf ? 64 : 32;
            uint64_t wm, tm;
            if (N != sf || opc == 3 || (!sf && (immr >= 32 || imms >= 32)) || !bit_masks(N, imms, immr, size, false, wm, tm))
                unsupported(w);
            uint64_t src = xr(rn) & m, bot = ror_n(src, immr, size), r;
            if (opc == 2) r = bot & wm & tm;  // UBFM
            else if (opc == 0) {               // SBFM
                uint64_t top = ((src >> imms) & 1) ? m : 0;
                r = (top & ~tm) | (bot & wm & tm);
            } else {  // BFM
                uint64_t dst = xr(rd) & m;
                uint64_t b2 = (dst & ~wm) | (bot & wm);
                r = (dst & ~tm) | (b2 & tm);
            }
            wx(rd, r & m);
        } else if (op == 7) {  // EXTR
            int lsb = (int)bits(w, 10, 6), size = sf ? 64 : 32;
            if ((!sf && lsb >= 32) || bits(w, 22, 1) != (uint64_t)sf || bits(w, 29, 2) || bits(w, 21, 1)) unsupported(w);
            uint64_t hi = xr(rn) & m, lo = xr(rm) & m;
            wx(rd, lsb ? ((lo >> lsb) | (hi << (size - lsb))) & m : lo);
        } else {
            unsupported(w);
        }
    } else if ((op0 & 0b1110) == 0b1010) {  // branches, exception generation, system
        if (bits(w, 26, 5) == 0b00101) {  // B / BL
            uint64_t t = pc + (uint64_t)(sext(bits(w, 0, 26), 26) * 4);
            if (bits(w, 31, 1)) wxk(30, pc + 4, true);
            next = t;
        } else if (bits(w, 25, 7) == 0b0101010 && !bits(w, 4, 1) && !bits(w, 24, 1)) {  // B.cond
            if (cond_holds((int)bits(w, 0, 4))) next = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        } else if (bits(w, 25, 6) == 0b011010) {  // CBZ / CBNZ
            uint64_t v0 = xr((int)bits(w, 0, 5)) & m;
            need_known("branch condition");
            if ((v0 != 0) == (bool)bits(w, 24, 1)) next = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
        } else if (bits(w, 25, 6) == 0b011011) {  // TBZ / TBNZ
            int b = (int)((bits(w, 31, 1) << 5) | bits(w, 19, 5));
            uint64_t v0 = (xr((int)bits(w, 0, 5)) >> b) & 1;
            need_known("branch condition");
            if (v0 == bits(w, 24, 1)) next = pc + (uint64_t)(sext(bits(w, 5, 14), 14) * 4);
        } else if (bits(w, 25, 7) == 0b1101011) {  // branch register
            int opc = (int)bits(w, 21, 4);
            if (bits(w, 16, 5) != 31 || bits(w, 10, 6) || bits(w, 0, 5) || opc > 2) unsupported(w);
            uint64_t t = xr(rn);
            need_known("branch target");
            if (opc == 1) wxk(30, pc + 4, true);
            next = t;
        } else if (bits(w, 22, 10) == 0b1101010100 && bits(w, 19, 3) == 0 && bits(w, 0, 5) == 31 &&
                   bits(w, 16, 3) == 3 && (bits(w, 12, 4) == 2 || bits(w, 12, 4) == 3)) {
            // hints and barriers: nothing to do here
        } else {
            unsupported(w);
        }
    } else if ((op0 & 0b0101) == 0b0100) {  // loads and stores
        if (bits(w, 26, 1)) {
            simd_ldst(w);
        } else if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
            int opc = (int)bits(w, 30, 2), rt = (int)bits(w, 0, 5);
            uint64_t addr = pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4);
            if (opc == 0) {
                uint64_t v0 = load(addr, 4, known);
                wxk(rt, v0, known);
            } else if (opc == 1) {
                uint64_t v0 = load(addr, 8, known);
                wxk(rt, v0, known);
            } else if (opc == 2) {
                uint64_t v0 = load(addr, 4, known);
                wxk(rt, (uint64_t)(int64_t)(int32_t)v0, known);
            }
            // (prfm: nothing)
        } else if (bits(w, 27, 3) == 0b101) {  // pair
            int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1);
            int rt = (int)bits(w, 0, 5), rt2 = (int)bits(w, 10, 5);
            if (opc == 3 || (opc == 1 && (!L || mode == 0))) unsupported(w);  // (mode 0: LDNP / STNP)
            int scale = opc == 2 ? 3 : 2, esz = 1 << scale;
            int64_t off = sext(bits(w, 15, 7), 7) * esz;
            uint64_t b = xsp(rn);
            need_known("address");
            uint64_t ea = mode == 1 ? b : b + (uint64_t)off;
            if (L) {
                bool k1, k2;
                uint64_t a = load(ea, esz, k1), c2 = load(ea + esz, esz, k2);
                if (opc == 1) a = (uint64_t)(int64_t)(int32_t)a, c2 = (uint64_t)(int64_t)(int32_t)c2;
                if (mode & 1) wxspk(rn, mode == 1 ? b + (uint64_t)off : ea, true);
                wxk(rt, a, k1);
                wxk(rt2, c2, k2);
            } else {
                unk = false;
                uint64_t a = xr(rt);
                bool k1 = !unk;
                unk = false;
                uint64_t c2 = xr(rt2);
                bool k2 = !unk;
                store(ea, esz, a, k1);
                store(ea + esz, esz, c2, k2);
                if (mode & 1) wxspk(rn, mode == 1 ? b + (uint64_t)off : ea, true);
            }
        } else if (bits(w, 27, 3) == 0b111) {  // register: unsigned offset, unscaled, pre / post, register offset
            int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2), rt = (int)bits(w, 0, 5);
            uint64_t b = xsp(rn), ea = 0;
            int wbmode = 0;  // 1 post, 2 pre
            int64_t imm9 = 0;
            if (bits(w, 24, 2) == 1) {
                ea = b + (bits(w, 10, 12) << size);
            } else if (bits(w, 24, 2) == 0) {
                int kind = (int)bits(w, 10, 2);
                if (bits(w, 21, 1)) {
                    if (kind != 2) unsupported(w);
                    int option = (int)bits(w, 13, 3);
                    if (!(option & 2)) unsupported(w);
                    ea = b + extend_reg(xr(rm), option, bits(w, 12, 1) ? size : 0);
                } else {
                    imm9 = sext(bits(w, 12, 9), 9);
                    if (kind == 2) unsupported(w);  // unprivileged
                    if (kind == 1) ea = b, wbmode = 1;
                    else ea = b + (uint64_t)imm9, wbmode = kind == 3 ? 2 : 0;
                }
            } else {
                unsupported(w);
            }
            need_known("address");
            int nbytes = 1 << size;
            uint64_t nb_addr = wbmode == 1 ? b + (uint64_t)imm9 : ea;
            if (opc == 0) {
                uint64_t v0 = xr(rt);
                store(ea, nbytes, v0, !unk);
                if (wbmode) wxspk(rn, nb_addr, true);
            } else if (opc == 1) {
                uint64_t v0 = load(ea, nbytes, known);
                if (wbmode) wxspk(rn, nb_addr, true);
                wxk(rt, v0, known);
            } else if (size == 3) {
                if (opc != 2) unsupported(w);  // (prfm: nothing)
            } else if (size == 2 && opc == 3) {
                unsupported(w);
            } else {
                uint64_t v0 = load(ea, nbytes, known);
                int nb = 8 << size;
                v0 = (uint64_t)((int64_t)(v0 << (64 - nb)) >> (64 - nb));
                if (wbmode) wxspk(rn, nb_addr, true);
                wxk(rt, opc == 2 ? v0 : v0 & 0xFFFFFFFFull, known);
            }
        } else if ((w & 0x3FBFFC00) == 0x089FFC00) {  // LDAR / STLR (EoW main+0xb79974: ldar before a table)
            int size = (int)bits(w, 30, 2), rt = (int)bits(w, 0, 5), nbytes = 1 << size;
            uint64_t ea = xsp(rn);
            need_known("address");
            if (bits(w, 22, 1)) {
                uint64_t v0 = load(ea, nbytes, known);
                wxk(rt, v0, known);
            } else {
                uint64_t v0 = xr(rt);
                store(ea, nbytes, v0, !unk);
            }
        } else {
            unsupported(w);  // exclusives, atomics
        }
    } else if ((op0 & 0b0111) == 0b0101) {  // data processing (register)
        int size = sf ? 64 : 32;
        if (!bits(w, 28, 1)) {
            if (bits(w, 24, 4) == 0b1010) {  // logical shifted
                int opc = (int)bits(w, 29, 2), sh = (int)bits(w, 22, 2), N = (int)bits(w, 21, 1), amt = (int)bits(w, 10, 6);
                if (!sf && amt >= 32) unsupported(w);
                uint64_t b = shift_reg(sf, xr(rm), sh, amt);
                if (N) b = ~b & m;
                uint64_t a = xr(rn) & m, r;
                if (opc == 1) r = a | b;
                else if (opc == 2) r = a ^ b;
                else r = a & b;
                if (opc == 3) logic_flags(sf, r);
                wx(rd, r & m);
            } else if (bits(w, 24, 4) == 0b1011) {  // add/sub shifted or extended
                int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
                uint64_t a, b;
                bool to_sp = false;
                if (!bits(w, 21, 1)) {
                    int sh = (int)bits(w, 22, 2), amt = (int)bits(w, 10, 6);
                    if (sh == 3 || (!sf && amt >= 32)) unsupported(w);
                    a = xr(rn);
                    b = shift_reg(sf, xr(rm), sh, amt);
                } else {
                    int amt = (int)bits(w, 10, 3);
                    if (bits(w, 22, 2) || amt > 4) unsupported(w);
                    a = xsp(rn);
                    b = extend_reg(xr(rm), (int)bits(w, 13, 3), amt);
                    to_sp = !S;
                }
                uint64_t r = op ? add_with_carry(sf, a, ~b, 1, S) : add_with_carry(sf, a, b, 0, S);
                if (to_sp) wxsp(rd, r);
                else wx(rd, r);
            } else {
                unsupported(w);
            }
        } else {
            int op2 = (int)bits(w, 21, 4);
            if (bits(w, 24, 4) == 0b1011) {  // 3-source
                int op31 = (int)bits(w, 21, 3), o0 = (int)bits(w, 15, 1), ra = (int)bits(w, 10, 5);
                if (bits(w, 29, 2)) unsupported(w);
                uint64_t r = 0, n = xr(rn), mm = xr(rm);
                if (op31 == 0) {
                    uint64_t p = (n * mm) & m, acc = xr(ra);
                    r = (o0 ? acc - p : acc + p) & m;
                } else if ((op31 == 1 || op31 == 5) && sf) {
                    uint64_t p = op31 == 1 ? (uint64_t)((int64_t)(int32_t)n * (int64_t)(int32_t)mm)
                                           : (uint64_t)(uint32_t)n * (uint32_t)mm;
                    uint64_t acc = xr(ra);
                    r = o0 ? acc - p : acc + p;
                } else if (op31 == 2 && sf && !o0) {
                    r = (uint64_t)(((__int128)(int64_t)n * (__int128)(int64_t)mm) >> 64);
                } else if (op31 == 6 && sf && !o0) {
                    r = (uint64_t)(((unsigned __int128)n * mm) >> 64);
                } else {
                    unsupported(w);
                }
                wx(rd, r);
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 0) {  // add/sub with carry
                if (bits(w, 10, 6)) unsupported(w);
                int op = (int)bits(w, 30, 1), S = (int)bits(w, 29, 1);
                unk |= !flags_known;
                int carry = (int)(nzcv >> 29) & 1;
                uint64_t b = op ? ~xr(rm) : xr(rm);
                wx(rd, add_with_carry(sf, xr(rn), b, carry, S));
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 2) {  // conditional compare
                if (!bits(w, 29, 1) || bits(w, 10, 1) || bits(w, 4, 1)) unsupported(w);
                int op = (int)bits(w, 30, 1), cond = (int)bits(w, 12, 4);
                if (!flags_known && !forking) {  // (FCMP before: the flags stay unknown)
                    xr(rn), xr(rm);
                } else if (cond_holds(cond)) {
                    uint64_t b = bits(w, 11, 1) ? (uint64_t)rm : xr(rm);
                    uint64_t a = xr(rn);
                    if (op) add_with_carry(sf, a, ~b, 1, true);
                    else add_with_carry(sf, a, b, 0, true);
                } else {
                    set_flags(bits(w, 0, 4) << 28);
                }
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 4) {  // conditional select
                int op = (int)bits(w, 30, 1), o2 = (int)bits(w, 10, 2), cond = (int)bits(w, 12, 4);
                if (bits(w, 29, 1) || o2 > 1) unsupported(w);
                uint64_t r;
                if (!flags_known && !forking) {  // a select on FCMP's flags: an unknown value
                    r = xr(rn), xr(rm);
                    unk = true;
                } else if (cond_holds(cond)) r = xr(rn);
                else {
                    r = xr(rm);
                    if (op) r = ~r;
                    if (o2) r += 1;
                }
                wx(rd, r & m);
            } else if (bits(w, 24, 4) == 0b1010 && op2 == 6) {
                int opcode = (int)bits(w, 10, 6);
                if (bits(w, 29, 1)) unsupported(w);
                uint64_t a = xr(rn) & m, r = 0;
                if (bits(w, 30, 1)) {  // 1-source
                    if (bits(w, 16, 5)) unsupported(w);
                    if (opcode == 0) {
                        for (int i = 0; i < size; i++)
                            if ((a >> i) & 1) r |= 1ull << (size - 1 - i);
                    } else if (opcode == 1 || opcode == 2 || (opcode == 3 && sf)) {
                        int cont = opcode == 1 ? 16 : opcode == 2 ? 32 : 64;  // REV16, REV32 / REV (32-bit), REV
                        for (int base0 = 0; base0 < size; base0 += cont)
                            for (int i = 0; i < cont; i += 8)
                                r |= ((a >> (base0 + i)) & 0xFF) << (base0 + cont - 8 - i);
                    } else if (opcode == 4) {
                        r = (uint64_t)(size - bit_len(a));
                    } else if (opcode == 5) {
                        uint64_t t = ((a >> 1) ^ a) & (m >> 1);
                        r = (uint64_t)(size - 1 - bit_len(t));
                    } else {
                        unsupported(w);
                    }
                } else {  // 2-source
                    uint64_t b = xr(rm) & m;
                    if (opcode == 2) r = b ? a / b : 0;
                    else if (opcode == 3) {
                        if (!b) r = 0;
                        else if (sf) {
                            int64_t sa = (int64_t)a, sb = (int64_t)b;
                            r = (sa == INT64_MIN && sb == -1) ? a : (uint64_t)(sa / sb);
                        } else {
                            int64_t sa = (int32_t)(uint32_t)a, sb = (int32_t)(uint32_t)b;
                            r = (uint64_t)(sa / sb);
                        }
                    } else if (opcode >= 8 && opcode <= 11) {
                        r = shift_reg(sf, a, opcode - 8, (int)(b & (uint64_t)(size - 1)));
                    } else {
                        unsupported(w);  // (CRC32, PAC)
                    }
                }
                wx(rd, r & m);
            } else {
                unsupported(w);
            }
        }
    } else if ((op0 & 0b0111) == 0b0111) {
        fp_simd(w);
    } else {
        unsupported(w);  // unallocated
    }
    pc = next;
}

// ---------------------------------------------------------------- FP / SIMD

// FP/SIMD loads and stores: the memory effects (and faults) exactly; the values are kept, so a spill and reload stays
// known.
void Emu::simd_ldst(uint32_t w) {
    int rt = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
    if (bits(w, 27, 3) == 0b011 && bits(w, 24, 2) == 0) {  // literal
        int opc = (int)bits(w, 30, 2);
        if (opc == 3) unsupported(w);
        vload(rt, pc + (uint64_t)(sext(bits(w, 5, 19), 19) * 4), 4 << opc);
        return;
    }
    if (bits(w, 27, 3) == 0b101) {  // pair
        int opc = (int)bits(w, 30, 2), mode = (int)bits(w, 23, 2), L = (int)bits(w, 22, 1), rt2 = (int)bits(w, 10, 5);
        if (opc == 3) unsupported(w);
        int esz = 4 << opc;
        int64_t off = sext(bits(w, 15, 7), 7) * esz;
        uint64_t b = xsp(rn);
        need_known("address");
        uint64_t ea = mode == 1 ? b : b + (uint64_t)off;
        if (L) {
            vload(rt, ea, esz);
            vload(rt2, ea + esz, esz);
        } else {
            vstore(rt, ea, esz);
            vstore(rt2, ea + esz, esz);
        }
        if (mode & 1) wxspk(rn, mode == 1 ? b + (uint64_t)off : ea, true);
        return;
    }
    if (bits(w, 27, 3) == 0b111) {  // register
        int size = (int)bits(w, 30, 2), opc = (int)bits(w, 22, 2);
        if ((opc & 2) && size) unsupported(w);
        int esz = (opc & 2) ? 16 : 1 << size;
        bool L = opc & 1;
        uint64_t b = xsp(rn), ea = 0;
        int wbmode = 0;
        int64_t imm9 = 0;
        if (bits(w, 24, 2) == 1) {
            ea = b + bits(w, 10, 12) * (uint64_t)esz;
        } else if (bits(w, 24, 2) == 0) {
            int kind = (int)bits(w, 10, 2);
            if (bits(w, 21, 1)) {
                int option = (int)bits(w, 13, 3);
                if (kind != 2 || !(option & 2)) unsupported(w);
                int sh = esz == 16 ? 4 : size;
                ea = b + extend_reg(xr(rm), option, bits(w, 12, 1) ? sh : 0);
            } else {
                if (kind == 2) unsupported(w);
                imm9 = sext(bits(w, 12, 9), 9);
                if (kind == 1) ea = b, wbmode = 1;
                else ea = b + (uint64_t)imm9, wbmode = kind == 3 ? 2 : 0;
            }
        } else {
            unsupported(w);
        }
        need_known("address");
        if (L) vload(rt, ea, esz);
        else vstore(rt, ea, esz);
        if (wbmode) wxspk(rn, wbmode == 1 ? b + (uint64_t)imm9 : ea, true);
        return;
    }
    if (bits(w, 31, 1) == 0 && bits(w, 24, 6) == 0b001100 && bits(w, 21, 1) == 0) {  // LD1 / ST1 (multiple)
        int n = 0;
        switch (bits(w, 12, 4)) {
            case 7: n = 1; break;
            case 10: n = 2; break;
            case 6: n = 3; break;
            case 2: n = 4; break;
            default: unsupported(w);  // (LD2..LD4 interleave)
        }
        int esz = bits(w, 30, 1) ? 16 : 8;
        uint64_t b = xsp(rn);
        need_known("address");
        for (int i = 0; i < n; i++) {
            int r = (rt + i) % 32;
            if (bits(w, 22, 1)) vload(r, b + (uint64_t)(i * esz), esz);
            else vstore(r, b + (uint64_t)(i * esz), esz);
        }
        if (bits(w, 23, 1)) {
            uint64_t inc = rm == 31 ? (uint64_t)(n * esz) : xr(rm);
            wxsp(rn, b + inc);
        }
        return;
    }
    unsupported(w);
}

// ---- scalar FP arithmetic, computed as the Arm ARM's pseudocode does with FPCR = 0 (Unicorn's: round to nearest, no
// flush to zero, NaNs propagated rather than the default NaN), whatever the host's own NaN rules are. Values spilled to
// memory and reloaded (EoW main+0x1abe3c: an fadd stored, main+0x5806e4 loads and compares it) then stay known.
namespace {

template <class F> struct FpBits;
template <> struct FpBits<float> {
    using U = uint32_t;
    static constexpr U SIGN = 0x80000000u, QUIET = 0x00400000u, DEFAULT_NAN = 0x7FC00000u, EXP = 0x7F800000u,
                       FRAC = 0x007FFFFFu;
};
template <> struct FpBits<double> {
    using U = uint64_t;
    static constexpr U SIGN = 0x8000000000000000ull, QUIET = 0x0008000000000000ull, DEFAULT_NAN = 0x7FF8000000000000ull,
                       EXP = 0x7FF0000000000000ull, FRAC = 0x000FFFFFFFFFFFFFull;
};

template <class F> struct Fp {
    using F_ = F;
    using B = FpBits<F>;
    using U = typename B::U;
    static F f(U u) {
        F x;
        memcpy(&x, &u, sizeof x);
        return x;
    }
    static U u(F x) {
        U v;
        memcpy(&v, &x, sizeof v);
        return v;
    }
    static bool nan(U a) { return (a & B::EXP) == B::EXP && (a & B::FRAC); }
    static bool snan(U a) { return nan(a) && !(a & B::QUIET); }
    static bool inf(U a) { return (a & ~B::SIGN) == B::EXP; }
    static bool zero(U a) { return !(a & ~B::SIGN); }
    // FPProcessNaNs: the first signalling NaN (quietened), else the first quiet NaN
    static bool nans(std::initializer_list<U> ops, U &r) {
        for (U a : ops)
            if (snan(a)) return r = a | B::QUIET, true;
        for (U a : ops)
            if (nan(a)) return r = a, true;
        return false;
    }
    // a host result: a NaN from operands that were not NaNs is the invalid-operation default NaN
    static U done(F x) { return x != x ? B::DEFAULT_NAN : u(x); }
    static bool two(int op, U a, U b, U &r) {
        F x = f(a), y = f(b);
        if (op == 6 || op == 7) {  // FMAXNM / FMINNM: a lone quiet NaN gives the other operand
            bool qa = nan(a) && !snan(a), qb = nan(b) && !snan(b);
            if (qa && !nan(b)) return r = b, true;
            if (qb && !nan(a)) return r = a, true;
        }
        if (nans({a, b}, r)) return true;
        switch (op) {
            case 0: r = done(x * y); return true;                  // FMUL
            case 1: r = done(x / y); return true;                  // FDIV
            case 2: r = done(x + y); return true;                  // FADD
            case 3: r = done(x - y); return true;                  // FSUB
            case 4: case 6:                                        // FMAX, FMAXNM
                r = zero(a) && zero(b) ? (a & b) : x > y ? a : b;  // (+0 over -0)
                return true;
            case 5: case 7:                                        // FMIN, FMINNM
                r = zero(a) && zero(b) ? (a | b) : x < y ? a : b;  // (-0 over +0)
                return true;
            case 8: r = done(x * y) ^ B::SIGN; return true;        // FNMUL
        }
        return false;
    }
    // FMULX: FMUL, but 0 x infinity is 2 (signed)
    static U mulx(U a, U b) {
        U r;
        if (nans({a, b}, r)) return r;
        if ((inf(a) && zero(b)) || (zero(a) && inf(b))) return ((a ^ b) & B::SIGN) | u(F(2));
        return done(f(a) * f(b));
    }
    // FRECPS / FRSQRTS (FPRecipStepFused / FPRSqrtStepFused): 2 - a*b, (3 - a*b) / 2, one rounding; a is negated
    // first (a NaN's sign too)
    static U step(U a, U b, bool rsqrt) {
        a ^= B::SIGN;
        U r;
        if (nans({a, b}, r)) return r;
        if ((inf(a) && zero(b)) || (zero(a) && inf(b))) return u(rsqrt ? F(1.5) : F(2));
        if (inf(a) || inf(b)) return ((a ^ b) & B::SIGN) | B::EXP;
        F x = std::fma(f(a), f(b), rsqrt ? F(3) : F(2));
        return done(rsqrt ? x * F(0.5) : x);
    }
    static constexpr int FB = sizeof(F) == 4 ? 23 : 52;  // fraction bits
    // FRECPE (FPRecipEstimate) and FRSQRTE (FPRSqrtEstimate): the Arm ARM's 8-bit estimates, as Unicorn has them
    static U recpe(U op) {
        U sign = op & B::SIGN, r;
        if (nans({op}, r)) return r;
        if (inf(op)) return sign;
        if (zero(op)) return sign | B::EXP;
        if (std::fabs(f(op)) < std::ldexp(F(1), sizeof(F) == 4 ? -128 : -1024)) return sign | B::EXP;
        U frac = op & B::FRAC;
        int64_t exp = (int64_t)((op & B::EXP) >> FB);
        if (exp == 0) {
            if (!((frac >> (FB - 1)) & 1)) exp = -1, frac = (frac << 2) & B::FRAC;
            else frac = (frac << 1) & B::FRAC;
        }
        uint32_t a = 0x100 | (uint32_t)(frac >> (FB - 8));
        a = a * 2 + 1;
        uint32_t est = ((1u << 19) / a + 1) / 2;
        int64_t rexp = (sizeof(F) == 4 ? 253 : 2045) - exp;
        U rf = (U)(est & 0xFF) << (FB - 8);
        if (rexp == 0) rf = ((U)1 << (FB - 1)) | (rf >> 1);
        else if (rexp == -1) rf = ((U)1 << (FB - 2)) | (rf >> 2), rexp = 0;
        return sign | ((U)rexp << FB) | rf;
    }
    static U rsqrte(U op) {
        U r;
        if (nans({op}, r)) return r;
        if (zero(op)) return (op & B::SIGN) | B::EXP;
        if (op & B::SIGN) return B::DEFAULT_NAN;
        if (inf(op)) return 0;
        U frac = op & B::FRAC;
        int64_t exp = (int64_t)((op & B::EXP) >> FB);
        if (exp == 0) {
            while (!((frac >> (FB - 1)) & 1)) frac = (frac << 1) & B::FRAC, exp--;
            frac = (frac << 1) & B::FRAC;
        }
        uint64_t a = !(exp & 1) ? 0x100 | (uint64_t)(frac >> (FB - 8)) : 0x80 | (uint64_t)(frac >> (FB - 7));
        if (a < 256) a = a * 2 + 1;
        else a = ((a >> 1) << 1), a = (a + 1) * 2;
        uint64_t b = 512;
        while (a * (b + 1) * (b + 1) < (1ull << 28)) b++;
        uint64_t est = (b + 1) / 2;
        int64_t rexp = ((sizeof(F) == 4 ? 380 : 3068) - exp) / 2;
        return ((U)rexp << FB) | ((U)(est & 0xFF) << (FB - 8));
    }
    // FPMulAdd: addend + n * m, the operands already negated as the instruction says
    static U muladd(U ad, U n, U m) {
        U r;
        bool infzero = (inf(n) && zero(m)) || (zero(n) && inf(m));
        if (nan(ad) && !snan(ad) && infzero && !snan(n) && !snan(m)) return B::DEFAULT_NAN;
        if (nans({ad, n, m}, r)) return r;
        return done(std::fma(f(n), f(m), f(ad)));
    }
};

}  // namespace

// Scalar FP data processing it computes: true if it set its result (known or not)
bool Emu::fp_scalar(uint32_t w) {
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5), type = (int)bits(w, 22, 2);
    if (bits(w, 29, 3) != 0 || type > 1) return false;  // (half precision, M or S set: not computed)
    bool dbl = type == 1;
    auto get = [&](int r) { return dbl ? (uint64_t)v[r] : (uint64_t)(uint32_t)v[r]; };
    auto put = [&](uint64_t r, bool known) { v[rd] = r, vknown[rd] = known; };
    if ((w & 0xFF000000) == 0x1F000000) {  // FMADD / FMSUB / FNMADD / FNMSUB
        int ra = (int)bits(w, 10, 5);
        bool o1 = bits(w, 21, 1), o0 = bits(w, 15, 1), known = vknown[rn] && vknown[rm] && vknown[ra];
        uint64_t n = get(rn), m = get(rm), a = get(ra);
        if (dbl) {
            if (o0 != o1) n ^= FpBits<double>::SIGN;
            if (o1) a ^= FpBits<double>::SIGN;
            put(Fp<double>::muladd(a, n, m), known);
        } else {
            if (o0 != o1) n ^= FpBits<float>::SIGN;
            if (o1) a ^= FpBits<float>::SIGN;
            put(Fp<float>::muladd((uint32_t)a, (uint32_t)n, (uint32_t)m), known);
        }
        return true;
    }
    if ((w & 0xFF200C00) == 0x1E200800) {  // 2-source
        int op = (int)bits(w, 12, 4);
        if (op > 8) return false;
        uint64_t r = 0;
        if (dbl) Fp<double>::two(op, get(rn), get(rm), r);
        else {
            uint32_t r32 = 0;
            Fp<float>::two(op, (uint32_t)get(rn), (uint32_t)get(rm), r32);
            r = r32;
        }
        put(r, vknown[rn] && vknown[rm]);
        return true;
    }
    if ((w & 0xFF200C00) == 0x1E200C00) {  // FCSEL
        if (!flags_known && !forking) return put(0, false), true;
        bool known = vknown[rn] && vknown[rm];
        put(cond_holds((int)bits(w, 12, 4)) ? get(rn) : get(rm), known);
        return true;
    }
    if ((w & 0xFF201FE0) == 0x1E201000) {  // FMOV (immediate): VFPExpandImm
        uint32_t imm8 = (uint32_t)bits(w, 13, 8), sgn = imm8 >> 7, b6 = (imm8 >> 6) & 1, low = imm8 & 0x3F;
        if (dbl)
            put((uint64_t)sgn << 63 | (uint64_t)(b6 ^ 1) << 62 | (b6 ? 0xFFull : 0) << 54 | (uint64_t)low << 48, true);
        else
            put((uint64_t)(sgn << 31 | (b6 ^ 1) << 30 | (b6 ? 0x1Fu : 0) << 25 | low << 19), true);
        return true;
    }
    if ((w & 0xFF207C00) == 0x1E204000) {  // 1-source
        int op = (int)bits(w, 15, 6);
        uint64_t a = get(rn);
        bool known = vknown[rn];
        if (dbl) {
            using P = Fp<double>;
            uint64_t r;
            switch (op) {
                case 0: put(a, known); return true;                                     // FMOV
                case 1: put(a & ~FpBits<double>::SIGN, known); return true;             // FABS
                case 2: put(a ^ FpBits<double>::SIGN, known); return true;              // FNEG
                case 3: put(P::nans({a}, r) ? r : P::done(std::sqrt(P::f(a))), known); return true;  // FSQRT
                case 4:  // FCVT Sd, Dn (the host's conversion keeps a NaN's sign and top payload bits, as Arm's)
                    put(Fp<float>::u((float)P::f(P::snan(a) ? a | FpBits<double>::QUIET : a)), known);
                    return true;
            }
        } else {
            using P = Fp<float>;
            uint32_t a32 = (uint32_t)a, r;
            switch (op) {
                case 0: put(a32, known); return true;
                case 1: put(a32 & ~FpBits<float>::SIGN, known); return true;
                case 2: put(a32 ^ FpBits<float>::SIGN, known); return true;
                case 3: put(P::nans({a32}, r) ? r : P::done(std::sqrt(P::f(a32))), known); return true;
                case 5:  // FCVT Dd, Sn
                    put(Fp<double>::u((double)P::f(P::snan(a32) ? a32 | FpBits<float>::QUIET : a32)), known);
                    return true;
            }
        }
        return false;
    }
    return false;
}


// AdvSIMD it computes (vector FP arithmetic and estimates, logic, immediates, element moves, TBL): Unicorn computes
// them, and earlier tables' FP work can be what a later one loads (EoW main+0x1a13e80: frsqrte / frsqrts / fmul whose
// result is spilled where main+0x5806e4 reloads and compares it). True if it set its result (known or not).
bool Emu::simd_vector(uint32_t w) {
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5), rm = (int)bits(w, 16, 5);
    bool q = bits(w, 30, 1), u = bits(w, 29, 1);
    using V = unsigned __int128;
    auto lane = [](V x, int esize, int i) -> uint64_t {
        return (uint64_t)(x >> (esize * i)) & (esize == 64 ? ~0ull : (1ull << esize) - 1);
    };
    auto setlane = [](V &x, int esize, int i, uint64_t val) {
        V m = (esize == 64 ? (V)~0ull : (V)((1ull << esize) - 1)) << (esize * i);
        x = (x & ~m) | (((V)val << (esize * i)) & m);
    };
    auto put = [&](V r, bool known, bool full) {
        if (!full) r &= (V)~0ull;
        v[rd] = r, vknown[rd] = known;
    };
    // a lane-wise FP operation on 32- or 64-bit lanes (scalar: lane 0 only, the rest zero)
    auto fp_lanes = [&](bool dbl, int lanes, bool known, auto &&fn) {
        V r = 0;
        for (int i = 0; i < lanes; i++) setlane(r, dbl ? 64 : 32, i, fn(i));
        put(r, known, true);
        if (lanes * (dbl ? 64 : 32) <= 64) v[rd] &= (V)~0ull;
    };
    auto f3 = [&](bool dbl, int op, uint64_t a, uint64_t b, uint64_t d) -> uint64_t {
        // op: the three-same FP opcode with U and a (bit 23) above it: (U << 6) | (a << 5) | opcode
        auto go = [&](auto P, auto A, auto Bv, auto Dv) -> uint64_t {
            using Fp_ = decltype(P);
            using U_ = typename Fp_::U;
            U_ r = 0;
            switch (op) {
                case 0x18: Fp_::two(6, A, Bv, r); return r;                         // FMAXNM
                case 0x19: return Fp_::muladd(Dv, A, Bv);                           // FMLA
                case 0x1A: Fp_::two(2, A, Bv, r); return r;                         // FADD
                case 0x1B: return Fp_::mulx(A, Bv);                                 // FMULX
                case 0x1C: return Fp_::nan(A) || Fp_::nan(Bv) || Fp_::f(A) != Fp_::f(Bv) ? 0 : (U_)~(U_)0;  // FCMEQ
                case 0x1E: Fp_::two(4, A, Bv, r); return r;                         // FMAX
                case 0x1F: return Fp_::step(A, Bv, false);                          // FRECPS
                case 0x38: Fp_::two(7, A, Bv, r); return r;                         // FMINNM
                case 0x39: return Fp_::muladd(Dv, A ^ FpBits<typename Fp_::F_>::SIGN, Bv);  // FMLS
                case 0x3A: Fp_::two(3, A, Bv, r); return r;                         // FSUB
                case 0x3E: Fp_::two(5, A, Bv, r); return r;                         // FMIN
                case 0x3F: return Fp_::step(A, Bv, true);                           // FRSQRTS
                case 0x5B: Fp_::two(0, A, Bv, r); return r;                         // FMUL
                case 0x5C: return !Fp_::nan(A) && !Fp_::nan(Bv) && Fp_::f(A) >= Fp_::f(Bv) ? (U_)~(U_)0 : 0;  // FCMGE
                case 0x5F: Fp_::two(1, A, Bv, r); return r;                         // FDIV
                case 0x7A:                                                          // FABD
                    Fp_::two(3, A, Bv, r);
                    return r & ~FpBits<typename Fp_::F_>::SIGN;
                case 0x7C: return !Fp_::nan(A) && !Fp_::nan(Bv) && Fp_::f(A) > Fp_::f(Bv) ? (U_)~(U_)0 : 0;  // FCMGT
            }
            throw EmuFault();  // (not reached: the caller filters)
        };
        if (dbl) return go(Fp<double>(), a, b, d);
        return go(Fp<float>(), (uint32_t)a, (uint32_t)b, (uint32_t)d);
    };
    static const int F3_OPS[] = {0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1E, 0x1F, 0x38, 0x39, 0x3A, 0x3E,
                                 0x3F, 0x5B, 0x5C, 0x5F, 0x7A, 0x7C};
    auto f3_ok = [&](int op) {
        for (int x : F3_OPS)
            if (x == op) return true;
        return false;
    };

    // three same, FP (vector and scalar)
    bool scalar = bits(w, 28, 1);
    if (((w & 0x9F200400) == 0x0E200400 || (w & 0xDF200400) == 0x5E200400) && bits(w, 14, 2) == 3) {
        int op = (int)((u << 6) | (bits(w, 23, 1) << 5) | bits(w, 11, 5));
        bool dbl = bits(w, 22, 1);
        if (!f3_ok(op) || (dbl && !q && !scalar)) return false;
        if (scalar && op != 0x7A && op != 0x1B && op != 0x1F && op != 0x3F && op != 0x1C && op != 0x5C && op != 0x7C)
            return false;
        bool acc = op == 0x19 || op == 0x39;
        int lanes = scalar ? 1 : (q ? 4 : 2) >> (dbl ? 1 : 0);
        int es = dbl ? 64 : 32;
        V a = v[rn], b = v[rm], d = v[rd];
        fp_lanes(dbl, lanes, vknown[rn] && vknown[rm] && (!acc || vknown[rd]),
                 [&](int i) { return f3(dbl, op, lane(a, es, i), lane(b, es, i), lane(d, es, i)); });
        return true;
    }
    // two-register misc, FP (vector and scalar): FABS, FNEG, FSQRT, FRECPE, FRSQRTE, SCVTF, UCVTF, FCVTZS, FCVTZU,
    // the compares with zero
    if ((w & 0x9F3E0C00) == 0x0E200800 || (w & 0xDF3E0C00) == 0x5E200800) {
        int op = (int)((u << 6) | (bits(w, 23, 1) << 5) | bits(w, 12, 5));
        bool dbl = bits(w, 22, 1);
        if (dbl && !q && !scalar) return false;
        int lanes = scalar ? 1 : (q ? 4 : 2) >> (dbl ? 1 : 0), es = dbl ? 64 : 32;
        auto one = [&](uint64_t x) -> uint64_t {
            auto go = [&](auto P, auto X) -> uint64_t {
                using Fp_ = decltype(P);
                using U_ = typename Fp_::U;
                using F_ = typename Fp_::F_;
                U_ r;
                const U_ ones = (U_)~(U_)0;
                F_ fx0 = Fp_::f(X);
                bool nan0 = Fp_::nan(X);
                switch (op) {
                    case 0x2C: return !nan0 && fx0 > 0 ? ones : 0;                         // FCMGT #0
                    case 0x6C: return !nan0 && fx0 >= 0 ? ones : 0;                        // FCMGE #0
                    case 0x2D: return !nan0 && fx0 == 0 ? ones : 0;                        // FCMEQ #0
                    case 0x6D: return !nan0 && fx0 <= 0 ? ones : 0;                        // FCMLE #0
                    case 0x2E: return !nan0 && fx0 < 0 ? ones : 0;                         // FCMLT #0
                    case 0x2F: return X & ~FpBits<F_>::SIGN;                               // FABS
                    case 0x6F: return X ^ FpBits<F_>::SIGN;                                // FNEG
                    case 0x7F: return Fp_::nans({X}, r) ? r : Fp_::done(std::sqrt(Fp_::f(X)));  // FSQRT
                    case 0x3D: return Fp_::recpe(X);                                       // FRECPE
                    case 0x7D: return Fp_::rsqrte(X);                                      // FRSQRTE
                    case 0x1D: return Fp_::u((F_)(std::make_signed_t<U_>)X);              // SCVTF
                    case 0x5D: return Fp_::u((F_)X);                                       // UCVTF
                    case 0x3B: case 0x7B: {                                                // FCVTZS / FCVTZU
                        F_ fx = Fp_::f(X);
                        int nb = sizeof(F_) * 8;
                        if (fx != fx) return 0;
                        if (op == 0x3B) {
                            F_ hi = std::ldexp(F_(1), nb - 1);
                            if (fx >= hi) return (U_)((U_)1 << (nb - 1)) - 1;
                            if (fx <= -hi) return (U_)((U_)1 << (nb - 1));
                            return (U_)(std::make_signed_t<U_>)std::trunc(fx);
                        }
                        F_ hi = std::ldexp(F_(1), nb);
                        if (fx >= hi) return (U_)~(U_)0;
                        if (fx <= 0) return 0;
                        return (U_)std::trunc(fx);
                    }
                }
                throw EmuFault();
            };
            if (dbl) return go(Fp<double>(), x);
            return go(Fp<float>(), (uint32_t)x);
        };
        switch (op) {
            case 0x2C: case 0x6C: case 0x2D: case 0x6D: case 0x2E:
            case 0x2F: case 0x6F: case 0x7F: case 0x3D: case 0x7D: case 0x1D: case 0x5D: case 0x3B: case 0x7B: break;
            default: return false;
        }
        if (scalar && (op == 0x2F || op == 0x6F || op == 0x7F)) return false;  // (those are FP data processing)
        V a = v[rn];
        fp_lanes(dbl, lanes, vknown[rn], [&](int i) { return one(lane(a, es, i)); });
        return true;
    }
    // by element, FP vector: FMLA, FMLS, FMUL, FMULX
    if ((w & 0x9F800400) == 0x0F800000 && !scalar) {
        int opc = (int)bits(w, 12, 4);
        bool dbl = bits(w, 22, 1);
        int op = opc == 1 && !u ? 0x19 : opc == 5 && !u ? 0x39 : opc == 9 && !u ? 0x5B : opc == 9 && u ? 0x1B : -1;
        if (op < 0 || (dbl && (!q || bits(w, 21, 1)))) return false;
        int idx = dbl ? (int)bits(w, 11, 1) : (int)(bits(w, 11, 1) << 1 | bits(w, 21, 1));
        int m = (int)(bits(w, 16, 4) | bits(w, 20, 1) << 4);
        int lanes = (q ? 4 : 2) >> (dbl ? 1 : 0), es = dbl ? 64 : 32;
        bool acc = op == 0x19 || op == 0x39;
        V a = v[rn], d = v[rd];
        uint64_t b = lane(v[m], es, idx);
        fp_lanes(dbl, lanes, vknown[rn] && vknown[m] && (!acc || vknown[rd]),
                 [&](int i) { return f3(dbl, op, lane(a, es, i), b, lane(d, es, i)); });
        return true;
    }
    // three same, logical: AND, BIC, ORR (MOV), ORN, EOR, BSL, BIT, BIF
    if ((w & 0x9F20FC00) == 0x0E201C00) {
        int sz = (int)bits(w, 22, 2);
        V a = v[rn], b = v[rm], d = v[rd], r;
        bool known = vknown[rn] && vknown[rm];
        if (!u) r = sz == 0 ? a & b : sz == 1 ? a & ~b : sz == 2 ? a | b : a | ~b;
        else if (sz == 0) r = a ^ b;
        else {
            known = known && vknown[rd];
            r = sz == 1 ? (d & a) | (~d & b) : sz == 2 ? (d & ~b) | (a & b) : (d & b) | (a & ~b);
        }
        put(r, known, q);
        return true;
    }
    // modified immediate: MOVI, MVNI, ORR, BIC (immediate), FMOV (vector, immediate)
    if ((w & 0x9FF80400) == 0x0F000400) {
        int cmode = (int)bits(w, 12, 4), op = (int)u;
        uint64_t imm8 = bits(w, 16, 3) << 5 | bits(w, 5, 5), imm = 0;
        auto rep = [](uint64_t x, int n) {
            uint64_t r = 0;
            for (int i = 0; i < 64; i += n) r |= x << i;
            return r;
        };
        switch (cmode >> 1) {
            case 0: imm = rep(imm8, 32); break;
            case 1: imm = rep(imm8 << 8, 32); break;
            case 2: imm = rep(imm8 << 16, 32); break;
            case 3: imm = rep(imm8 << 24, 32); break;
            case 4: imm = rep(imm8, 16); break;
            case 5: imm = rep(imm8 << 8, 16); break;
            case 6: imm = rep(cmode & 1 ? imm8 << 16 | 0xFFFF : imm8 << 8 | 0xFF, 32); break;
            case 7:
                if (!(cmode & 1) && !op) imm = rep(imm8, 8);
                else if (!(cmode & 1)) {
                    for (int i = 0; i < 8; i++)
                        if ((imm8 >> i) & 1) imm |= 0xFFull << (8 * i);
                } else if (!op) {
                    uint64_t b6 = (imm8 >> 6) & 1;
                    imm = rep((imm8 >> 7) << 31 | (b6 ^ 1) << 30 | (b6 ? 0x1Full : 0) << 25 | (imm8 & 0x3F) << 19, 32);
                } else {
                    if (!q) return false;
                    uint64_t b6 = (imm8 >> 6) & 1;
                    imm = (imm8 >> 7) << 63 | (b6 ^ 1) << 62 | (b6 ? 0xFFull : 0) << 54 | (imm8 & 0x3F) << 48;
                }
                break;
        }
        V iv = (V)imm << 64 | imm, d = v[rd];
        bool orr_bic = (cmode < 12 && (cmode & 1)) ;  // (cmode 0xx1, 10x1)
        if (orr_bic) put(op ? d & ~iv : d | iv, vknown[rd], q);
        else if (op && cmode < 14) put(~iv, true, q);  // MVNI
        else put(iv, true, q);                          // MOVI, FMOV
        return true;
    }
    // copy: DUP (element, general), INS (element, general)
    if ((w & 0x9FE08400) == 0x0E000400) {
        int imm5 = (int)bits(w, 16, 5), imm4 = (int)bits(w, 11, 4), k = 0;
        if (!(imm5 & 15)) return false;
        while (!((imm5 >> k) & 1)) k++;
        int es = 8 << k, idx = imm5 >> (k + 1);
        V d = v[rd];
        if (!u && imm4 == 0) {  // DUP (element)
            uint64_t x = lane(v[rn], es, idx);
            V r = 0;
            for (int i = 0; i < 128 / es; i++) setlane(r, es, i, x);
            put(r, vknown[rn], q);
            return true;
        }
        if (!u && imm4 == 1) {  // DUP (general)
            uint64_t x = xr(rn);
            V r = 0;
            for (int i = 0; i < 128 / es; i++) setlane(r, es, i, x);
            put(r, !unk, q);
            return true;
        }
        if (!u && imm4 == 3 && q) {  // INS (general)
            uint64_t x = xr(rn);
            setlane(d, es, idx, x);
            put(d, vknown[rd] && !unk, true);
            return true;
        }
        if (u && q) {  // INS (element)
            setlane(d, es, idx, lane(v[rn], es, imm4 >> k));
            put(d, vknown[rd] && vknown[rn], true);
            return true;
        }
        if (!u && (imm4 == 5 || imm4 == 7)) {  // SMOV / UMOV
            uint64_t x = lane(v[rn], es, idx);
            if (imm4 == 5) x = (uint64_t)((int64_t)(x << (64 - es)) >> (64 - es));
            if (!q) x &= 0xFFFFFFFFull;
            wxk(rd, x, vknown[rn]);
            return true;
        }
        return false;
    }
    // TBL / TBX
    if ((w & 0xBFE08C00) == 0x0E000000) {
        int len = (int)bits(w, 13, 2) + 1;
        bool tbx = bits(w, 12, 1), known = vknown[rm] && (!tbx || vknown[rd]);
        V r = tbx ? v[rd] : 0, ix = v[rm];
        for (int i = 0; i < (q ? 16 : 8); i++) {
            unsigned b = (unsigned)lane(ix, 8, i);
            if (b < 16u * len) {
                int t = (rn + (int)(b / 16)) % 32;
                known = known && vknown[t];
                setlane(r, 8, i, lane(v[t], 8, (int)(b % 16)));
            } else if (!tbx) {
                setlane(r, 8, i, 0);
            }
        }
        put(r, known, q);
        return true;
    }
    // EXT
    if ((w & 0xBFE08400) == 0x2E000000) {
        int pos = (int)bits(w, 11, 4);
        if (!q && pos > 7) return false;
        int n = q ? 16 : 8;
        V r = 0;
        for (int i = 0; i < n; i++) {
            int j = pos + i;
            setlane(r, 8, i, j < n ? lane(v[rn], 8, j) : lane(v[rm], 8, j - n));
        }
        put(r, vknown[rn] && vknown[rm], q);
        return true;
    }
    return false;
}

// FP/SIMD data processing. Moves between integer and FP registers keep the value; anything else makes its result
// unknown (and FCMP the flags). Encodings SimdC refuses are treated as not instructions.
void Emu::fp_simd(uint32_t w) {
    int rd = (int)bits(w, 0, 5), rn = (int)bits(w, 5, 5);
    try {
        SimdC().dp(w, pc);
    } catch (const Unsupported &) {
        unsupported(w);
    }
    if ((w & 0x5F20FC00) == 0x1E200000) {  // FP <-> integer
        int sf = (int)bits(w, 31, 1), type = (int)bits(w, 22, 2), rmode = (int)bits(w, 19, 2), opcode = (int)bits(w, 16, 3);
        bool plain = rmode == 0 && ((sf && type == 1) || (!sf && type == 0));
        if (opcode == 7 && plain) {  // FMOV general -> FP
            uint64_t g = xr(rn);
            v[rd] = sf ? g : (uint32_t)g;
            vknown[rd] = !unk;
        } else if (opcode == 6 && plain) {  // FMOV FP -> general
            uint64_t b = (uint64_t)v[rn];
            wxk(rd, sf ? b : (uint32_t)b, vknown[rn]);
        } else if (sf && type == 2 && rmode == 1 && opcode == 7) {  // FMOV Vd.D[1], Xn
            uint64_t g = xr(rn);
            v[rd] = (v[rd] & (((unsigned __int128)1 << 64) - 1)) | (unsigned __int128)g << 64;
            vknown[rd] = vknown[rd] && !unk;
        } else if (sf && type == 2 && rmode == 1 && opcode == 6) {  // FMOV Xd, Vn.D[1]
            wxk(rd, (uint64_t)(v[rn] >> 64), vknown[rn]);
        } else if ((opcode == 0 || opcode == 1) && rmode == 3 && type < 2) {  // FCVTZS / FCVTZU: saturating
            int nb = sf ? 64 : 32;
            double x = type ? Fp<double>::f((uint64_t)v[rn]) : Fp<float>::f((uint32_t)v[rn]);
            uint64_t r;
            if (x != x) r = 0;
            else if (opcode == 0) {
                double hi = std::ldexp(1.0, nb - 1);
                r = x >= hi ? (uint64_t)(sf ? INT64_MAX : INT32_MAX)
                    : x <= -hi ? (uint64_t)(sf ? INT64_MIN : INT32_MIN)
                               : (uint64_t)(int64_t)std::trunc(x);
                if (!sf) r &= 0xFFFFFFFFull;
            } else {
                double hi = std::ldexp(1.0, nb);
                r = x >= hi ? (sf ? UINT64_MAX : 0xFFFFFFFFull) : x <= 0 ? 0 : (uint64_t)std::trunc(x);
            }
            wxk(rd, r, vknown[rn]);
        } else if (opcode == 0 || opcode == 1 || opcode == 4 || opcode == 5) {  // other FCVT* to integer: not computed
            wxk(rd, 0, false);
        } else if ((opcode == 2 || opcode == 3) && rmode == 0 && type < 2) {  // SCVTF / UCVTF (integer)
            uint64_t g = xr(rn);
            if (!sf) g = opcode == 2 ? (uint64_t)(int64_t)(int32_t)g : (uint32_t)g;
            if (type) {
                double x = opcode == 2 ? (double)(int64_t)g : (double)g;
                v[rd] = Fp<double>::u(x);
            } else {
                float x = opcode == 2 ? (float)(int64_t)g : (float)g;
                v[rd] = Fp<float>::u(x);
            }
            vknown[rd] = !unk;
        } else {  // SCVTF / UCVTF (half precision)
            vknown[rd] = false;
        }
        return;
    }
    if (simd_vector(w)) return;
    if ((w & 0x9FE08400) == 0x0E000400 && bits(w, 29, 1) == 0) {  // AdvSIMD copy
        int imm4 = (int)bits(w, 11, 4);
        if (imm4 == 5 || imm4 == 7) {  // other SMOV / UMOV: not computed
            wxk(rd, 0, false);
            return;
        }
    }
    if ((w & 0xFF203C07) == 0x1E202000 && bits(w, 22, 2) < 2) {  // FCMP / FCMPE, single or double
        // computed (EoW main+0x5806f8: an fcmp and csel pick the value a table's index is clamped against)
        int rm = (int)bits(w, 16, 5);
        bool zero = bits(w, 3, 1), dbl = bits(w, 22, 1);
        if (!vknown[rn] || (!zero && !vknown[rm])) {
            flags_known = false;
            return;
        }
        double a, b = 0;
        if (dbl) {
            uint64_t ua = (uint64_t)v[rn], ub = (uint64_t)v[rm];
            memcpy(&a, &ua, 8);
            if (!zero) memcpy(&b, &ub, 8);
        } else {
            uint32_t ua = (uint32_t)v[rn], ub = (uint32_t)v[rm];
            float fa, fb = 0;
            memcpy(&fa, &ua, 4);
            if (!zero) memcpy(&fb, &ub, 4);
            a = fa, b = fb;
        }
        nzcv = a != a || b != b ? 0x3ull << 28 : a == b ? 0x6ull << 28 : a < b ? 0x8ull << 28 : 0x2ull << 28;
        flags_known = true;
        return;
    }
    if ((w & 0x5F203C00) == 0x1E202000 || (w & 0x5F200C00) == 0x1E200400) {  // half-precision FCMP, FCCMP
        flags_known = false;
        return;
    }
    if (fp_scalar(w)) return;
    vknown[rd] = false;
}

}  // namespace lasgen
