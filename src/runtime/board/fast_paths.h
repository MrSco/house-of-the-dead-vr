// Fast paths of the shared runtime: the same results, less work. Each flag is
// one subject, so a port turns on the ones that pay on its host (the PS Vita
// and Dreamcast ports this grew from did). The desktop and the Quest define
// none: the reference.
//
// M2_FAST_GEN  runtime support of the i960 code rewritten by scripts/fast_gen.py
//              and of m2recomp --fast_inaccuracy:
//              Cpu::work_ram, gen::wram_*, Lockstep's pending/check/set_end/epoch.
// M2_FAST_BUS  M2Board: the TGP's FIFO, function port, status and buffer RAM
//              decoded before the page table; the last plain RAM/texture page
//              read and the last plain RAM page written, remembered.
// M2_FAST_TGP  the TGP's FIFOs as a ring (WordFifo), read and written by the
//              TGP directly instead of through TgpBus's virtual calls.
// M2_FAST_SHARC  the recompiled SHARC's board accesses (Model 2B: its input and
//              output FIFOs, buffer RAM, the copro data ROM) read and written
//              directly (TgpBoard::SharcCopro sets the pointers) instead of
//              through data_space and the Bus's virtual calls (needs M2_FAST_TGP).
// M2_FAST_CPU  register frames copied as 16 words; spills to and refills from
//              work RAM straight through Cpu::work_ram (needs M2_FAST_GEN).
// M2_FAST_GEO  geometrizer: block reads of points and texture words, no
//              zero-fill of polygons and clip buffers, culled polygons skip
//              their texture reads and lighting, polygon words straight into
//              the rasterizer's slots.
#pragma once


#if defined(M2_FAST_CPU) && !defined(M2_FAST_GEN)
#error "M2_FAST_CPU spills register frames through Cpu::work_ram: define M2_FAST_GEN too"
#endif

#if defined(M2_FAST_SHARC) && !defined(M2_FAST_TGP)
#error "M2_FAST_SHARC needs M2_FAST_TGP (the board's FIFOs as WordFifo)"
#endif
