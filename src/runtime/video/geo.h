// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// Model 2 geometrizer and the rasterizer's front end (culling, clipping,
// z-sort keys): the display list, as native C++. The original Model 2's
// geometrizer runs code from ROM inside its DSP that nobody has dumped, so
// this is MAME's high-level implementation (src/mame/sega/model2_v.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81, BSD-3-Clause; notice above kept
// as the licence requires), transplanted: geo_parse and its commands,
// model2_3d_push, model2_3d_process_polygon. Memory is read through
// bounds-checked cursors, so a display list that runs off its buffer is a
// hard error instead of MAME's undefined behaviour. See THIRD_PARTY.md.
#pragma once

#include "runtime/board/fast_paths.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rt {

class State; // save states (runtime/board/state.h)

struct GeoFatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct GeoVertex {
    float x = 0, y = 0;
    float p[3] = {0, 0, 0}; // pz, pu, pv
};

// One polygon kept for rendering (after culling and clipping), as MAME's
// model2_3d_process_polygon adds it to its list.
struct GeoPoly {
    uint16_t z = 0;
    // The z it was sorted by (z above is this, packed, after the z adjust): its nearest or farthest vertex's, or an
    // earlier polygon's (a figure's polygons sort as one). A headset's depth buffer takes it (arcade.cpp key).
    float sortz = 0;
    uint16_t texheader[4] = {0, 0, 0, 0};
    uint8_t luma = 0;
    int32_t texlod = 0;
    int16_t viewport[4] = {0, 0, 0, 0};
    int16_t center[2] = {0, 0};
    uint8_t window = 0;
    uint32_t reverse = 0;
    uint8_t num_vertices = 0;
    // The geometrizer's focus when the polygon was made (its x and y are the
    // camera's times these): the camera-space point is (x / fx, y / fy, z).
    float focus[2] = {1.0f, 1.0f};
#ifdef M2_FAST_GEO
    // Not zero-filled for every polygon kept (160 bytes): only
    // v[0..num_vertices) is ever read, and model2_3d_process_polygon writes
    // those.
    GeoPoly() {}
    union { GeoVertex v[8]; };
#else
    GeoVertex v[8];
#endif
};

// Cursors standing in for MAME's raw u32/u16 pointers. Reads run on from a
// masked start, so a start near the end runs off it (time attack does, in
// polygon RAM and in texture memory). MAME reads past its array there; we
// wrap, as the hardware's address counter would. Sizes are powers of 2.
struct GeoPtr {
    uint32_t *base = nullptr;
    uint32_t size = 0, i = 0;
    bool null() const { return base == nullptr; }
    uint32_t &at() const {
        if (!base) throw GeoFatal("geometrizer read from missing memory");
        return base[i & (size - 1)];
    }
    uint32_t &operator*() const { return at(); }
    GeoPtr operator++(int) { GeoPtr t = *this; ++i; return t; }
    GeoPtr &operator+=(uint32_t n) { i += n; return *this; }
#ifdef M2_FAST_GEO
    // n words from here into out, moving past them: the values n times
    // *(*this)++ gives, a page (or up to the wrap) at a time instead of a
    // page lookup each.
    void read(uint32_t *out, uint32_t n) {
        while (n) {
            const uint32_t w = i & (size - 1);
            uint32_t run = n < size - w ? n : size - w;
            const uint32_t *src;
            {
                if (!base) throw GeoFatal("geometrizer read from missing memory");
                src = base + w;
            }
            for (uint32_t k = 0; k < run; ++k) out[k] = src[k];
            out += run;
            n -= run;
            i += run;
        }
    }
#endif
};
struct GeoPtr16 {
    const uint16_t *base = nullptr;
    uint32_t size = 0, i = 0;
    uint16_t operator*() const {
        if (!base) throw GeoFatal("rasterizer read from missing texture memory");
        return base[i & (size - 1)];
    }
    GeoPtr16 operator++(int) { GeoPtr16 t = *this; ++i; return t; }
#ifdef M2_FAST_GEO
    // As GeoPtr::read, 16-bit words.
    void read(uint16_t *out, uint32_t n) {
        while (n) {
            const uint32_t w = i & (size - 1);
            uint32_t run = n < size - w ? n : size - w;
            const uint16_t *src;
            {
                if (!base) throw GeoFatal("rasterizer read from missing texture memory");
                src = base + w;
            }
            for (uint32_t k = 0; k < run; ++k) out[k] = src[k];
            out += run;
            n -= run;
            i += run;
        }
    }
#endif
};

class Geo {
public:
    // polygons: the model ROM (16 or 32 MB); textures: the texture ROM
    // (0x1000000 bytes); buffer: buffer RAM (0x8000 dwords), which the
    // display list is read from. Both ROMs are kept, not copied (pass them
    // moved: Dead or Alive's are 48 MB).
    Geo(std::vector<uint8_t> polygons, std::vector<uint8_t> textures, uint32_t *buffer);

    // MAME screen_vblank -> geo_parse: walk this frame's display list from
    // read_start (the geometrizer's read-start register).
    void parse(uint32_t read_start);
    void state(State &s); // save states: the geometrizer's state and this frame's polygons

    // This frame's output.
    std::vector<GeoPoly> polys;       // kept polygons, in the order added
    std::vector<uint32_t> pushed;     // every word handed to the rasterizer
    // Every model drawn this frame (geo_object_data): its address and the matrix it was drawn with (model -> camera,
    // MAME's 3 x 4: x' = m0 x + m3 y + m6 z + m9 ...), in its window. The scenery's stays put in the world, so from
    // one frame to the next its matrices change as the camera moves (a headset's comfort: arcade.cpp).
    // (tpa, tha, count, mode: the object command's texture point and header addresses and its polygon count, and the
    // geometrizer's mode then: what model() needs to read the model again.)
    struct Object {
        uint32_t address;
        uint8_t window;
        float m[12];
        uint32_t tpa = 0, tha = 0, count = 0, mode = 0;
    };
    std::vector<Object> objects;
    // One polygon of a model as its ROM has it, in the model's own space (no matrix, no focus, no culling, no light):
    // 3 or 4 points in the rasterizer's order with their texture coordinates (GeoVertex's pu, pv), its normal, its
    // texture header, and its texture parameters as they are now (diffuse and ambient: luma = |n . light| x diffuse +
    // ambient, 0-255). For drawing a figure's parts where the game doesn't (the VR app's partner: core/avatar.cpp).
    struct ModelPoly {
        uint8_t n = 0;
        bool two_sided = false;
        float v[4][3], uv[4][2], normal[3];
        uint16_t texheader[4];
        float diffuse = 0, ambient = 0;
    };
    // An object's polygons (those the rasterizer would draw from some side), after parse. False: its model is not
    // where it was (polygon RAM written since) or runs off its memory.
    bool model(const Object &o, std::vector<ModelPoly> &out);
    bool record_pushes = false;

    struct plane {
        GeoVertex normal;
        float distance = 0;
    };
    struct texture_parameter {
        float diffuse = 0, ambient = 0;
        uint32_t specular_control = 0;
        float specular_scale = 0;
    };
    struct raster_state {
        const uint16_t *texture_rom = nullptr;
        uint32_t texture_rom_mask = 0;
        int16_t viewport[4] = {0, 0, 0, 0};
        int16_t center[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        uint16_t center_sel = 0;
        uint32_t reverse = 0;
        int32_t z_adjust = 0;
        float polygon_z = 0;
        uint8_t master_z_clip = 0;
        uint32_t cur_command = 0;
        uint32_t command_buffer[32] = {};
        uint32_t command_index = 0;
        uint32_t poly_list_index = 0;
        uint16_t min_z = 0, max_z = 0;
        uint16_t texture_ram[0x10000] = {};
        uint8_t log_ram[0x8000] = {};
        uint8_t cur_window = 0;
        plane clip_plane[4][4];
    };
    struct geo_state {
        raster_state *raster = nullptr;
        uint32_t mode = 0;
        uint32_t *polygon_rom = nullptr;
        uint32_t polygon_rom_mask = 0;
        float matrix[12] = {};
        GeoVertex focus, light;
        float lod = 0;
        float coef_table[32] = {};
        texture_parameter texture_parameters[32];
        uint32_t polygon_ram0[0x8000] = {};
        uint32_t polygon_ram1[0x8000] = {};
    };

    int windows() const { return raster_.cur_window; } // after parse: the last window used
    void zclip_w(uint32_t data) { raster_.master_z_clip = uint8_t(data); } // model2_3d_zclip_w
    // Widescreen (enhancement, 0 = off): a full-width viewport's left and right
    // clip planes move out by this many pixels, so polygons beyond the 4:3
    // edges survive for the wider screen. Applies from the next window command.
    void set_wide_margin(int pixels) { wide_margin_ = pixels; }
    // VR (enhancement): polygons are clipped only to just in front of the
    // camera, not to the screen's edges, so a renderer with a wider view (a
    // headset's, turned with the player's head) gets what lies outside the
    // game's picture as far as the game sent it. M2_VR=1 sets it (tests).
    void set_vr(bool on) { vr_ = on; }
    bool vr() const { return vr_; }

private:
    int wide_margin_ = 0;
    bool vr_ = false;
    std::vector<uint8_t> polygon_rom_; // little-endian dwords, read as geo_.polygon_rom
    std::vector<uint8_t> texture_rom_; // little-endian words, read as raster_.texture_rom
    uint32_t *buffer_;
    raster_state raster_;
    geo_state geo_;

    GeoPtr buf(uint32_t i) { return GeoPtr{buffer_, 0x8000, i}; }
    uint16_t float_to_zval(float floatval, int32_t z_adjust);
    bool check_culling(raster_state *raster, uint32_t attr, float min_z, float max_z);
    template <unsigned NumVerts> void model2_3d_process_polygon(raster_state *raster, uint32_t attr);
    void model2_3d_push(raster_state *raster, uint32_t input);
    void model2_3d_push_point(raster_state *raster, const GeoVertex &point);
    void render_frame_start();
    void geo_parse_np_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_np_s(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_s(geo_state *geo, GeoPtr input, uint32_t count);
    GeoPtr geo_nop(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_object_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_direct_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_window_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_polygon_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_parameters(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_zsort_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_focal_distance(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_light_source(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_matrix_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_translate_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_data_mem_push(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_test(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_end(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_dummy(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_log_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_lod(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_upload(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_jump(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_process_command(geo_state *geo, uint32_t opcode, GeoPtr input, bool *end_code);
};

} // namespace rt
