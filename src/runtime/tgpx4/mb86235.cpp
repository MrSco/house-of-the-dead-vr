// license:BSD-3-Clause
// copyright-holders:Angelo Salese, ElSemi, Ville Linde, Matthew Daniels
//
// MAME's MB86235 interpreter (mb86235.cpp's execute_run and device_reset,
// mb86235ops.cpp whole) on the shim of mb86235.h. See THIRD_PARTY.md.
#include "runtime/tgpx4/mb86235.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace rt::tgpx4 {

namespace {
using u32 = uint32_t;
inline float u2f(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline uint32_t f2u(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
template <typename T> constexpr bool BIT(T x, int n) { return (x >> n) & 1; }
[[noreturn]] void fatal(const char *fmt, ...) {
    char m[200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    throw Fatal(m);
}
} // namespace
#define fatalerror(...) fatal(__VA_ARGS__)

#include "runtime/tgpx4/mb86235ops.ipp"

#undef fatalerror

mb86235_device::mb86235_device(Bus &bus) : m_bus(bus) {
    m_fifoin.f.d = this;
    m_fifoout.f.d = this;
    m_external.d = this;
    std::memset(m_prog, 0, sizeof m_prog);
    std::memset(&m_dataa, 0, sizeof m_dataa);
    std::memset(&m_datab, 0, sizeof m_datab);
    std::memset(&m_state, 0, sizeof m_state); // MAME device_start
    reset();
}

void mb86235_device::reset() {
    m_core->pc = 0;
    m_core->delay_pc = 0;
    m_core->ppc = 0;
    m_core->delay_slot = false;
}

// MAME execute_run (interpreter), until the program stalls on an empty input
// FIFO: the stalled instruction is retried on the next run, as MAME's halt
// and release of the copro do.
bool mb86235_device::run(uint64_t max) {
    for (uint64_t n = 0; n < max; n++) {
        const uint32_t curpc = check_previous_op_stall() ? m_core->cur_fifo_state.pc : m_core->pc;
        const uint64_t opcode = m_prog[curpc & 0xfff];
        m_core->ppc = curpc;
        if (m_core->delay_slot == true) {
            m_core->pc = m_core->delay_pc;
            m_core->delay_slot = false;
        } else {
            handle_single_step_execution();
        }
        m_core->cur_fifo_state.has_stalled = false;
        execute_op(opcode);
        m_count++;
        if (m_core->cur_fifo_state.has_stalled && m_bus.fifo_in_size() == 0) return true;
    }
    return false;
}

void mb86235_device::unimplemented_op() { fatal("TGPx4: unimplemented opcode at pc=%03x%c", m_core->ppc, ' '); }
void mb86235_device::unimplemented_alu() { fatal("TGPx4: unimplemented ALU op at pc=%03x%c", m_core->ppc, ' '); }
void mb86235_device::unimplemented_control() { fatal("TGPx4: unimplemented control op at pc=%03x%c", m_core->ppc, ' '); }
void mb86235_device::unimplemented_double_xfer1() { fatal("TGPx4: unimplemented double transfer 1 at pc=%03x%c", m_core->ppc, ' '); }
void mb86235_device::unimplemented_double_xfer2() { fatal("TGPx4: unimplemented double transfer 2 at pc=%03x%c", m_core->ppc, ' '); }
void mb86235_device::pcs_overflow() { fatal("TGPx4: PC stack overflow at pc=%03x%c", m_core->ppc, ' '); }
void mb86235_device::pcs_underflow() { fatal("TGPx4: PC stack underflow at pc=%03x%c", m_core->ppc, ' '); }

} // namespace rt::tgpx4
