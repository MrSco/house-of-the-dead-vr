// license:BSD-3-Clause
// copyright-holders:Angelo Salese, ElSemi, Ville Linde, Matthew Daniels
//
// The Fujitsu MB86235 "TGPx4", Model 2C's geometry coprocessor (The House of
// the Dead), as MAME's interpreter (src/devices/cpu/mb86235/mb86235.h,
// mb86235.cpp, mb86235ops.cpp at mame0289, BSD-3-Clause; notice above kept
// as the licence requires). The class keeps MAME's name and members so the
// interpreter (mb86235ops.ipp) is MAME's own code; MAME's device framework is
// replaced by a small shim: the FIFOs and the external bus are the board's
// (Bus), an empty input FIFO stalls the instruction as MAME's FIFO callback
// does, and run() goes until the program waits on that empty FIFO. No DRC.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace rt { class State; }

namespace rt::tgpx4 {

struct Fatal : std::runtime_error { using std::runtime_error::runtime_error; };

// The board's side: the copro FIFOs (MAME generic_fifo_u32, 8 words deep on
// Model 2C) and the external data bus (24-bit dword addresses).
struct Bus {
    virtual ~Bus() = default;
    virtual bool fifo_pop(uint32_t &v) = 0;     // false: empty
    virtual void fifo_push(uint32_t v) = 0;
    virtual size_t fifo_in_size() const = 0;
    virtual size_t fifo_out_size() const = 0;
    virtual void fifo_in_clear() = 0;
    virtual void fifo_out_clear() = 0;
    virtual uint32_t ext_read(uint32_t addr) = 0;
    virtual void ext_write(uint32_t addr, uint32_t data) = 0;
};

class mb86235_device {
public:
    explicit mb86235_device(Bus &bus);
    void reset();                         // MAME device_reset
    uint64_t *program() { return m_prog; } // 4096 64-bit words (MAME copro_tgpx4_program)
    // Until the program stalls on an empty input FIFO (true), or `max`
    // instructions ran (false).
    bool run(uint64_t max);
    uint64_t instructions() const { return m_count; }
    uint32_t pc() const { return m_core->pc; }

    void stall() { m_core->cur_fifo_state.has_stalled = true; }
    void state(rt::State &s); // save states: the program, the RAMs, MAME's state

private:
    // ---- MAME's device framework, shimmed
    struct FifoIn {
        mb86235_device *d;
        uint32_t pop() {
            uint32_t v = 0;
            if (!d->m_bus.fifo_pop(v)) d->stall(); // MAME: the FIFO's empty-pop callback stalls the copro
            return v;
        }
        bool is_full() const { return d->m_bus.fifo_in_size() >= 8; }
        bool is_empty() const { return d->m_bus.fifo_in_size() == 0; }
        void clear() { d->m_bus.fifo_in_clear(); }
    };
    struct FifoOut {
        mb86235_device *d;
        void push(uint32_t v) { d->m_bus.fifo_push(v); }
        bool is_full() const { return d->m_bus.fifo_out_size() >= 8; }
        bool is_empty() const { return d->m_bus.fifo_out_size() == 0; }
        void clear() { d->m_bus.fifo_out_clear(); }
        explicit operator bool() const { return true; }
    };
    struct FifoInPtr { FifoIn f; FifoIn *operator->() { return &f; } explicit operator bool() const { return true; } };
    struct FifoOutPtr { FifoOut f; FifoOut *operator->() { return &f; } explicit operator bool() const { return true; } };
    struct External {
        mb86235_device *d;
        uint32_t read_dword(uint32_t a) { return d->m_bus.ext_read(a & 0xffffff); }
        void write_dword(uint32_t a, uint32_t v) { d->m_bus.ext_write(a & 0xffffff, v); }
    };
    struct Internal { // the A and B buses: 0x400 words of RAM each
        uint32_t ram[0x400];
        uint32_t read_dword(uint32_t a) const { return ram[a & 0x3ff]; }
        void write_dword(uint32_t a, uint32_t v) { ram[a & 0x3ff] = v; }
    };

    Bus &m_bus;
    uint64_t m_prog[0x1000];
    uint64_t m_count = 0;
    FifoInPtr m_fifoin;
    FifoOutPtr m_fifoout;
    External m_external;
    Internal m_dataa, m_datab;

    // ---- MAME's state (mb86235.h)
    struct mb86235_flags {
        uint32_t az, an, av, au, ad, zc, il, nr, zd, mn, mz, mv, mu, md;
    };
    struct fifo_state {
        uint32_t pc;
        bool has_stalled;
    };
    struct mb86235_internal_state {
        uint32_t pc, delay_pc, ppc;
        uint32_t aa[8], ab[8], ma[8], mb[8], ar[8];
        uint32_t sp, eb, eo, rpc, lpc;
        uint32_t prp, pwp;
        uint32_t pr[24];
        uint32_t mod;
        mb86235_flags flags;
        uint32_t st;
        int icount;
        uint32_t arg0, arg1, arg2, arg3;
        uint64_t arg64;
        uint32_t pcp;
        uint32_t pcs[4];
        uint32_t jmpdest, alutemp, multemp, condtemp;
        uint32_t pdr, ddr;
        float fp0;
        bool delay_slot;
        fifo_state cur_fifo_state;
    };
    mb86235_internal_state m_state;
    mb86235_internal_state *m_core = &m_state;

    // ---- MAME's interpreter (mb86235ops.ipp)
    void unimplemented_op();
    void unimplemented_alu();
    void unimplemented_control();
    void unimplemented_double_xfer1();
    void unimplemented_double_xfer2();
    void pcs_overflow();
    void pcs_underflow();
    bool check_previous_op_stall();
    void execute_op(uint64_t op);
    void do_alu1(uint64_t op);
    void do_alu2(uint64_t op);
    void do_alu2_trans2_1(uint64_t op);
    void do_alu2_trans1_1(uint64_t op);
    void do_alu1_trans2_2(uint64_t op);
    void do_alu1_trans1_2(uint64_t op);
    void do_trans1_3(uint64_t op);
    void do_alu_control(uint64_t op);
    inline uint32_t get_prx(uint8_t which);
    inline uint32_t get_constfloat(uint8_t which);
    inline uint32_t get_constint(uint8_t which);
    inline uint32_t get_alureg(uint8_t which, bool isfloatop);
    inline uint32_t get_mulreg(uint8_t which, bool isfloatop);
    inline void set_alureg(uint8_t which, uint32_t value);
    inline void decode_aluop(uint8_t opcode, uint32_t src1, uint32_t src2, uint8_t imm, uint8_t dst_which);
    inline void decode_mulop(bool isfmul, uint32_t src1, uint32_t src2, uint8_t dst_which);
    inline bool decode_branch_jump(uint8_t which);
    inline uint32_t do_control_dst(uint64_t op);
    inline void push_pc(uint32_t pcval);
    inline uint32_t pop_pc();
    inline void set_mod(uint16_t mod1, uint16_t mod2);
    inline uint32_t get_transfer_reg(uint8_t which);
    inline void set_transfer_reg(uint8_t which, uint32_t value);
    inline uint32_t decode_ea(uint8_t mode, uint8_t rx, uint8_t ry, uint16_t disp, bool isbbus);
    inline uint32_t read_abus(uint32_t addr);
    inline uint32_t read_bbus(uint32_t addr);
    inline void write_abus(uint32_t addr, uint32_t data);
    inline void write_bbus(uint32_t addr, uint32_t data);
    inline void increment_pwp();
    inline void increment_prp();
    inline void decrement_prp();
    inline void zero_prp();
    inline void set_alu_flagsd(uint32_t val);
    inline void set_alu_flagsf(float val);
    inline void set_alu_flagsi(int val);
    inline bool get_alu_second_src(uint8_t which);
    void handle_single_step_execution();
};

} // namespace rt::tgpx4
