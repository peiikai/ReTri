// The jump-table evaluator (emu.cpp).
#pragma once

#include "gen_internal.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lasgen {

// An access outside the mapped memory, an instruction the evaluator does not run, or an unknown value that would
// decide an address or a branch (gen.py: unicorn.UcError).
struct EmuFault {};
// A condition on unknown flags, with no answer left for it (Emu::retry): run the path again both ways.
struct EmuFork {};

class Emu {
  public:
    static constexpr uint64_t SCRATCH_MEM = 0x7300000000ull;  // gen.py SCRATCH_MEM
    static constexpr size_t SCRATCH_SIZE = 0x100000;

    Emu(const Nso &nso, uint64_t base);
    // Run the instruction at `at`; pc is then the next one. Throws EmuFault.
    void step(uint64_t at);

    void set_x(int r, uint64_t v) { x[r] = v, xk[r] = true; }
    void set_sp(uint64_t v) { sp = v, spk = true; }
    // Register r's value; false if it is unknown.
    bool get_x(int r, uint64_t &v) const {
        v = x[r];
        return xk[r];
    }
    uint64_t pc = 0;

    // Trying each outcome of the conditions that read unknown flags (FCMP of unknown values, as Unicorn computes them
    // all): begin_paths() takes a snapshot and logs the memory written from then on; retry() rolls back to it and
    // answers those conditions in order with `choices` (EmuFork when they run out); path_end() is the state a run ended
    // in; end_paths() rolls back and keeps what every run agrees on (the rest unknown), as the one Unicorn ran is one
    // of them. end_paths() with no runs keeps the current state (a plain run).
    struct Regs {
        uint64_t x[31], sp, nzcv;
        bool xk[31], spk, flags_known;
        unsigned __int128 v[32];
        bool vknown[32];
    };
    struct PathEnd {
        Regs r;
        std::vector<std::pair<uint64_t, std::pair<uint8_t, bool>>> mem;  // bytes written: value, tainted
    };
    void begin_paths();
    void retry(const std::vector<bool> &choices);
    PathEnd path_end() const;
    void end_paths(const std::vector<PathEnd> &ends);

  private:
    // Values the evaluator does not compute (the FP / SIMD forms it doesn't run, values derived from them, memory
    // written with them) are unknown. Unknown values flow through registers and memory like any other; only where one
    // would decide an address, a branch or the table's target does the table stop, so it never gives a wrong target.
    uint64_t x[31] = {};
    bool xk[31];
    uint64_t sp = 0, nzcv = 0;
    bool spk = true, flags_known = true;
    unsigned __int128 v[32] = {};
    bool vknown[32];
    std::unordered_set<uint64_t> tainted;  // bytes of memory holding an unknown value
    bool unk = false;                      // the instruction being run read an unknown value
    bool forking = false;                  // between begin_paths and end_paths
    std::vector<bool> choices;             // retry's answers, and how many are used
    size_t next_choice = 0;
    Regs snap;
    std::unordered_map<uint64_t, std::pair<uint8_t, bool>> undo;  // written bytes' values before (and taint)

    uint64_t base;
    std::vector<uint8_t> image;    // the module's memory (writable, as mapped in gen.py)
    std::vector<uint8_t> scratch;  // SCRATCH_MEM
    bool debug = false;

    uint8_t *mem(uint64_t a, size_t n);
    uint8_t *wmem(uint64_t a, size_t n);  // mem() for a write (logged while forking)
    Regs regs() const;
    void set_regs(const Regs &r);
    void rollback();
    bool unknown_flags_cond(int cond);  // cond_holds when the flags are unknown: the next answer (forking)
    void need_known(const char *what);
    uint64_t load(uint64_t a, int size, bool &known);
    void store(uint64_t a, int size, uint64_t v, bool known);
    bool is_tainted(uint64_t a, int size) const;
    void taint(uint64_t a, int size, bool unknown);
    void vload(int r, uint64_t a, int size);
    void vstore(int r, uint64_t a, int size);
    void simd_ldst(uint32_t w);
    void fp_simd(uint32_t w);
    bool fp_scalar(uint32_t w);
    bool simd_vector(uint32_t w);
    uint64_t xr(int r);
    uint64_t xsp(int r);
    void wx(int r, uint64_t v) { wxk(r, v, !unk); }
    void wxsp(int r, uint64_t v) { wxspk(r, v, !unk); }
    void wxk(int r, uint64_t v, bool known);
    void wxspk(int r, uint64_t v, bool known);
    void set_flags(uint64_t f) { nzcv = f, flags_known = !unk; }
    uint64_t add_with_carry(int sf, uint64_t a, uint64_t b, int carry, bool flags);
    void logic_flags(int sf, uint64_t r);
    bool cond_holds(int cond);
    [[noreturn]] void unsupported(uint32_t w);
};

}  // namespace lasgen
