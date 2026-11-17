// Save states (state.h): each part's fields, in one place. Model 2C with the SCSP sound board (The House of the
// Dead) is what online play needs; the other boards' parts (the TGP, the SHARC, the Model 1 sound board) refuse.
//
// What is saved is what the game's future depends on: the CPUs, the memories, the devices' registers, the events
// scheduled. What is not: the ROMs, the settings a player has (widescreen, volumes, VR), and the caches made from
// the memories (decoded tiles, pens' GPU copies, generations), which a load has made again.
#include "runtime/board/game_loop.h"
#include "runtime/board/state.h"
#include "runtime/sound/scsp_board.h"
#include "runtime/tgpx4/mb86235.h"

#include <cstring>
#include <string>

namespace rt {

namespace {
constexpr char kMagic[5] = "M2ST";
constexpr uint32_t kVersion = 2; // (2: GeoPoly::sortz)

uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    static const bool made = [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
            table[i] = c;
        }
        return true;
    }();
    (void)made;
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}

void vertex(State &s, GeoVertex &v) { s(v.x)(v.y)(v.p); }
} // namespace

// ---- the i960
void Cpu::state(State &s) {
    s.tag("I960");
    s(m_r)(m_rcache)(m_rcache_frame_addr)(m_rcache_pos)(m_fp)(m_SAT)(m_PRCB)(m_PC)(m_AC)(m_IP)(m_PIP)(m_ICR);
    s(m_immediate_irq)(m_immediate_vector)(m_immediate_pri)(m_irq_line_state);
}

// ---- the I/O board
void IoBoard::state(State &s) {
    s.tag("IOBD");
    s(inputs.steer)(inputs.accel)(inputs.brake)(inputs.in0)(inputs.in1)(inputs.in2)(inputs.handbrake);
    s(inputs.gun_x)(inputs.gun_y)(inputs.gun2_x)(inputs.gun2_y);
    s(eeprom)(eeprom_dirty)(drive_commands)(ram_)(port_)(port_config_)(mode_)(analog_channel_)(ctrl_mode_)(gun_mux_);
    s(ee_state_)(ee_cs_)(ee_clk_)(ee_di_)(ee_locked_)(ee_bits_)(ee_accum_)(ee_shift_)(ee_command_)(ee_address_);
    s(ee_done_at_)(ee_now_);
}

// ---- the board
void M2Board::state(State &s) {
    if (img_.game->board != BoardType::Model2c || comm_board_) throw State::Bad("save states: Model 2C boards only, no link");
    s.tag("M2BD");
    // (sizes checked, never changed: the page map and the video point into them)
    for (std::vector<uint8_t> *v : {&ram_, &work_, &cpuctl_, &backup_, &tile_, &chr_, &palette_, &xlat_, &tex0_, &tex1_,
                                    &luma_, &fb_a_, &fb_b_, &comm_, &prot_ram_})
        s.block(v->data(), v->size());
    s(prot_src_)(prot_busy_)(intreq_)(intena_)(lines_)(timervals_);
    for (Timer &t : timers_) s(t.run)(t.orig)(t.start); // (their callbacks: GameLoop's, through the Lockstep)
    s(tick_skip_)(vblank_tick_)(videocontrol_)(zclip_)(render_mode_)(render_test_)(render_unk_)(comm_cn_)(comm_fg_);
    s(frame_)(uart_txrdy_)(uart_shift_busy_)(uart_have_hold_)(uart_in_)(uart_hold_)(uart_out_)(sound_total_);
    io_.state(s);
    tgp_.state(s);
    geo_->state(s);
    video_->state(s);
    if (s.loading()) {
        for (Timer &t : timers_) t.expiry = kNoExpiry; // (the Lockstep's callbacks were cleared: add_call)
        // texture RAM changed under the renderers: every page uploaded again
        ++tex_generation_;
        for (uint64_t &g : tex_page_generation_) g = tex_generation_;
    }
}

void M2Board::add_call(uint64_t at, uint32_t tag) {
    if (tag >= kCallTimer && tag < kCallTimer + 4) {
        const unsigned n = tag - kCallTimer;
        timers_[n].expiry = ls_->add_callback(at, [this, n] {
            timers_[n].expiry = kNoExpiry; // (its slot is free from here)
            timer_expire(n);
        }, tag);
    } else if (tag == kCallUart) {
        ls_->add_callback(at, [this] { uart_shift_done(); }, tag);
    } else {
        throw State::Bad("save states: an unknown board event");
    }
}

// ---- the TGPx4 (its board's part: TgpBoard::state, tgp_board.cpp)
} // namespace rt

namespace rt::tgpx4 {
void mb86235_device::state(rt::State &s) {
    s.tag("TGX4");
    s(m_prog)(m_count)(m_dataa.ram)(m_datab.ram);
    mb86235_internal_state &c = m_state;
    s(c.pc)(c.delay_pc)(c.ppc)(c.aa)(c.ab)(c.ma)(c.mb)(c.ar)(c.sp)(c.eb)(c.eo)(c.rpc)(c.lpc)(c.prp)(c.pwp)(c.pr)(c.mod);
    mb86235_flags &f = c.flags;
    s(f.az)(f.an)(f.av)(f.au)(f.ad)(f.zc)(f.il)(f.nr)(f.zd)(f.mn)(f.mz)(f.mv)(f.mu)(f.md);
    s(c.st)(c.icount)(c.arg0)(c.arg1)(c.arg2)(c.arg3)(c.arg64)(c.pcp)(c.pcs)(c.jmpdest)(c.alutemp)(c.multemp);
    s(c.condtemp)(c.pdr)(c.ddr)(c.fp0)(c.delay_slot)(c.cur_fifo_state.pc)(c.cur_fifo_state.has_stalled);
}
} // namespace rt::tgpx4

namespace rt {

// ---- the geometrizer
void Geo::state(State &s) {
    s.tag("GEO_");
    raster_state &r = raster_;
    s(r.viewport)(r.center)(r.center_sel)(r.reverse)(r.z_adjust)(r.polygon_z)(r.master_z_clip)(r.cur_command);
    s(r.command_buffer)(r.command_index)(r.poly_list_index)(r.min_z)(r.max_z)(r.texture_ram)(r.log_ram)(r.cur_window);
    for (auto &row : r.clip_plane)
        for (plane &p : row) vertex(s, p.normal), s(p.distance);
    geo_state &g = geo_;
    s(g.mode)(g.matrix);
    vertex(s, g.focus), vertex(s, g.light);
    s(g.lod)(g.coef_table);
    for (texture_parameter &t : g.texture_parameters) s(t.diffuse)(t.ambient)(t.specular_control)(t.specular_scale);
    s(g.polygon_ram0)(g.polygon_ram1);
    // this frame's polygons (a 30 Hz frame draws the last frame's again)
    uint64_t n = polys.size();
    s(n);
    if (s.loading()) {
        if (n > 1u << 20) throw State::Bad("save states: too many polygons");
        polys.resize(size_t(n));
    }
    for (GeoPoly &p : polys) {
        s(p.z)(p.sortz)(p.texheader)(p.luma)(p.texlod)(p.viewport)(p.center)(p.window)(p.reverse)(p.num_vertices)(p.focus);
        if (p.num_vertices > 8) throw State::Bad("save states: a polygon of more than 8 vertices");
        for (unsigned i = 0; i < p.num_vertices; i++) vertex(s, p.v[i]);
    }
    if (s.loading()) pushed.clear();
}

// ---- the video board's 2D side
void Video::state(State &s) {
    s.tag("VIDE");
    s(pens_)(palette_dirty_)(crtc_x_)(crtc_y_)(render_x_)(render_y_);
    if (s.loading()) {
        dec_valid_ = false; // every tile decoded again (and their generations moved)
        system24_source_dirty_ = true;
        system24_palette_generation_ = system24_texture_generation_ + 1;
        tile_memory_w();
        character_memory_w();
        ++colour_generation_;
        render_done_ = false;
    }
}

// ---- the game loop: a header, the parts, a checksum
void GameLoop::save_state(std::vector<uint8_t> &out) {
    if (sound_frame_pending_) throw Fatal("save state: a sound frame is pending");
    out.clear();
    State s(out);
    std::string set = game().set;
    uint32_t version = kVersion;
    uint8_t sound = sound_ != nullptr;
    for (char c : std::string(kMagic)) out.push_back(uint8_t(c));
    s(version)(set)(sound);
    state(s);
    const uint32_t crc = crc32(out.data(), out.size());
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(crc >> (8 * i)));
}

void GameLoop::load_state(const uint8_t *data, size_t n) {
    if (sound_frame_pending_) throw Fatal("load state: a sound frame is pending");
    if (n < 8 || std::memcmp(data, kMagic, 4) != 0) throw State::Bad("not a save state");
    uint32_t crc = 0;
    for (int i = 0; i < 4; i++) crc |= uint32_t(data[n - 4 + size_t(i)]) << (8 * i);
    if (crc32(data, n - 4) != crc) throw State::Bad("save state: damaged (its checksum)");
    State s(data + 4, n - 8);
    uint32_t version = 0;
    std::string set;
    uint8_t sound = 0;
    s(version)(set)(sound);
    if (version != kVersion) throw State::Bad("save state: another version (" + std::to_string(version) + ")");
    if (set != game().set) throw State::Bad("save state: another game's (" + set + ")");
    if (bool(sound) != (sound_ != nullptr)) throw State::Bad("save state: with and without the sound board");
    state(s);
    if (!s.at_end()) throw State::Bad("save state: longer than its fields");
}

void GameLoop::state(State &s) {
    s.tag("LOOP");
    s(frames_)(frame_start_)(vblank_start_)(in_vblank_);
    s(inputs_.steer)(inputs_.accel)(inputs_.brake)(inputs_.in0)(inputs_.in1)(inputs_.in2)(inputs_.handbrake);
    s(inputs_.gun_x)(inputs_.gun_y)(inputs_.gun2_x)(inputs_.gun2_y);
    if (in_vblank_ && !s.loading()) throw Fatal("save state: mid frame");
    cpu_->state(s);
    std::vector<Lockstep::Saved> calls;
    ls_->state(s, calls); // (loading: its callbacks cleared, added back below)
    board_->state(s);
    if (sound_) sound_->state(s);
    std::vector<uint8_t> replies = replies_->take(); // (the sound board's replies the next frame takes)
    s(replies);
    replies_->put(std::vector<uint8_t>(replies));
    s.tag("END_");
    if (s.loading()) {
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) { // (last first: see Lockstep::state)
            if (it->tag == M2Board::kCallProbe) ls_->add_callback(it->count, [this] { probe(); }, M2Board::kCallProbe);
            else board_->add_call(it->count, it->tag);
        }
        ls_->settle();
        pending_sound_bytes_.clear();
        frame_done_ = true;
    }
}

} // namespace rt

// ---- the sound board
namespace snd {

void Board::state(rt::State &) { throw rt::State::Bad("save states: not with this sound board"); }

void ScspBoard::state(rt::State &s) {
    s.tag("SCSB");
    Cpu68k &c = cpu_;
    s(c.d)(c.a)(c.other_sp)(c.pc)(c.x)(c.n)(c.z)(c.v)(c.c)(c.sys)(c.irq_line);
    s.block(c.ram.data(), c.ram.size()); // (the SCSP's sound RAM too: it points into it)
    uint64_t next = sched_.next_count;
    s(sched_.count)(next)(sched_.auto_irq);
    std::vector<std::pair<uint64_t, uint64_t>> events;
    if (!s.loading() && !sched_.saved(events)) throw rt::State::Bad("save states: a sound event without a tag");
    uint64_t n = events.size();
    s(n);
    if (s.loading()) {
        if (n > 1u << 16) throw rt::State::Bad("save states: too many sound events");
        events.resize(size_t(n));
    }
    for (auto &[when, tag] : events) s(when)(tag);
    s(carry_)(irq_lines_)(timer_gen_)(line_free_);
    uint64_t received = received_;
    s(received)(done_)(replies_);
    scsp_.state(s);
    if (s.loading()) {
        received_ = size_t(received);
        sched_.clear();
        for (const auto &[when, tag] : events) {
            if ((tag & ~((uint64_t(1) << 40) - 1)) == kEvMidi) add_midi(when, uint8_t(tag));
            else if ((tag & ~((uint64_t(1) << 40) - 1)) == kEvTimer) add_timer(when, int((tag >> 32) & 0xff), uint32_t(tag));
            else throw rt::State::Bad("save states: an unknown sound event");
        }
        sched_.next_count = next;
        out_.clear(); // (this player's samples not yet played: of the game before)
    }
}

void Scsp::state(rt::State &s) {
    s.tag("SCSP");
    s(owner_age_)(rand_)(m_udata.data);
    // an LFO's tables as their numbers (-1: none)
    auto lfo = [&](SCSP_LFO_t &l, bool amplitude) {
        int *const tables[4] = {amplitude ? m_ALFO_SAW : m_PLFO_SAW, amplitude ? m_ALFO_SQR : m_PLFO_SQR,
                                amplitude ? m_ALFO_TRI : m_PLFO_TRI, amplitude ? m_ALFO_NOI : m_PLFO_NOI};
        int8_t table = -1, scale = -1;
        for (int i = 0; i < 4; i++) if (l.table == tables[i]) table = int8_t(i);
        for (int i = 0; i < 8; i++) if (l.scale == (amplitude ? m_ASCALES[i] : m_PSCALES[i])) scale = int8_t(i);
        if (!s.loading() && ((l.table && table < 0) || (l.scale && scale < 0)))
            throw rt::State::Bad("save states: an LFO table not the SCSP's");
        s(l.phase)(l.phase_step)(table)(scale);
        if (s.loading()) {
            if (table > 3 || scale > 7) throw rt::State::Bad("save states: an LFO table out of range");
            l.table = table < 0 ? nullptr : tables[table];
            l.scale = scale < 0 ? nullptr : amplitude ? m_ASCALES[scale] : m_PSCALES[scale];
        }
    };
    for (SCSP_SLOT &sl : m_Slots) {
        s(sl.udata.data)(sl.Backwards)(sl.active)(sl.cur_addr)(sl.nxt_addr)(sl.step);
        SCSP_EG_t &e = sl.EG;
        s(e.volume)(e.state)(e.step)(e.AR)(e.D1R)(e.D2R)(e.RR)(e.DL)(e.EGHOLD)(e.LPLINK);
        lfo(sl.PLFO, false);
        lfo(sl.ALFO, true);
        s(sl.slot)(sl.Prev)(sl.effect)(sl.eg_samples)(sl.eg_steps)(sl.eg_next)(sl.eg_gain)(sl.eg_column)(sl.eg_state);
        if (s.loading()) sl.decoded = false; // (its registers decoded again, with this player's volumes)
    }
    s(m_RINGBUF)(m_BUFPTR)(m_IrqTimA)(m_IrqTimBC)(m_IrqMidi)(m_IrqCPU)(m_IrqDMA)(m_latched_MSLC)(m_latched_MSLC_data);
    s(m_MidiOutStack)(m_MidiOutW)(m_MidiOutR)(m_MidiStack)(m_MidiW)(m_MidiR)(m_TimPris)(m_TimCnt);
    s(m_dma.dmea)(m_dma.drga)(m_dma.dtlg)(m_dma.dgate)(m_dma.ddir)(m_mcieb)(m_mcipd);
    ScspDsp &d = m_DSP;
    s(d.RBP)(d.RBL)(d.COEF)(d.MADRS)(d.MPRO)(d.TEMP)(d.MEMS)(d.DEC)(d.MIXS)(d.EXTS)(d.EFREG)(d.Stopped)(d.LastStep);
    if (s.loading()) {
        d.native_checked = false; // (the recompiled DSP program checked against MPRO again)
        m_RBUFDST = m_RINGBUF + m_BUFPTR;
    }
}

} // namespace snd
