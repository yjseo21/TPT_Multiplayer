#include "StateDigest.h"
#include "World.h"
#include "simulation/Air.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace multiplayer
{
namespace
{
// FNV-style, one multiply per 64-bit word rather than per byte: the full
// fingerprint covers about 8-20 MB, once a second on the server and on every
// player's computer, so it must take a few milliseconds, not a whole frame.
// Each step is a bijection of the running value, so any single changed word
// always changes the result.
static_assert(std::endian::native == std::endian::little, "digests hash memory as little-endian words");
static_assert(sizeof(Particle) == 14 * 4, "particles are hashed as their raw fields");
struct Hash
{
    uint64_t value = UINT64_C(14695981039346656037);
    void Word(uint64_t v) { value = (value ^ v) * UINT64_C(1099511628211); }
    void U8(uint8_t v) { Word(v); }
    void U32(uint32_t v) { Word(v); }
    void U64(uint64_t v) { Word(v); }
    void I32(int v) { U32(std::bit_cast<uint32_t>(int32_t(v))); }
    void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
    void Bool(bool v) { U8(v ? 1 : 0); }
    void Text(std::string_view s)
    {
        U64(s.size()); for (unsigned char c : s) U8(c);
    }
    void Raw(const void *data, size_t size)
    {
        auto *bytes = static_cast<const unsigned char *>(data);
        size_t i = 0;
        for (; i + 8 <= size; i += 8)
        {
            uint64_t w; std::memcpy(&w, bytes + i, 8); Word(w);
        }
        if (i < size)
        {
            uint64_t w = 0; std::memcpy(&w, bytes + i, size - i); Word(w);
        }
        U64(size);
    }
    // Whole fixed-size arrays of plain numbers, as memory.
    template<class C> void Array(const C &c)
    {
        static_assert(std::is_array_v<C> && std::is_trivially_copyable_v<C>);
        Raw(&c, sizeof(c));
    }
    void ParticleState(const Particle &p) { Raw(&p, sizeof(p)); }
    void PlayerState(const playerst &p)
    {
        U8(uint8_t(p.comm)); U8(uint8_t(p.pcomm)); I32(p.elem);
        for (float v : p.legs) F32(v);
        for (float v : p.accs) F32(v);
        U8(uint8_t(p.spwn)); U32(p.frames); Bool(p.rocketBoots); Bool(p.fan); I32(p.spawnID); I32(p.controller);
    }
    std::string Hex() const
    {
        std::ostringstream s; s << std::hex << std::setfill('0') << std::setw(16) << value;
        return s.str();
    }
};
}

Json::Value DiagnosticDigests(const World &world)
{
    const Simulation &s = *world.sim;
    Json::Value result(Json::objectValue);
    Hash h;
    h.U64(world.tick); h.U64(world.revision); h.U64(world.resetGeneration); h.Bool(world.paused);
    h.U64(s.frameCount); auto rng = s.rng.state(); h.U64(rng[0]); h.U64(rng[1]);
    result["clock-rng"] = h.Hex();

    h = {}; h.I32(s.parts.active); h.I32(s.parts.FreeHead());
    // Every allocated slot is included, including holes whose life field can
    // hold a free-list link and hence influence the next allocation.
    h.Raw(s.parts.data.data(), size_t(std::max(s.parts.active, 0)) * sizeof(Particle));
    result["particle-slots"] = h.Hex();

    h = {}; h.Array(s.pmap); h.Array(s.photons); result["particle-maps"] = h.Hex();
    h = {}; h.Array(s.pv); h.Array(s.vx); h.Array(s.vy); h.Array(s.hv);
    result["air-fields"] = h.Hex();
    h = {}; h.Array(s.air->opv); h.Array(s.air->ovx); h.Array(s.air->ovy); h.Array(s.air->ohv);
    result["air-history"] = h.Hex();
    h = {}; h.Array(s.bmap); h.Array(s.emap); h.Array(s.air->bmap_blockair); h.Array(s.air->bmap_blockairh);
    h.Array(s.fvx); h.Array(s.fvy); result["walls-fans"] = h.Hex();
    h = {};
    for (int y = 0; y < YCELLS; ++y) for (int x = 0; x < XCELLS; ++x)
    {
        auto at = Vec2{x, y};
        h.F32(s.gravIn.mass[at]); h.U32(s.gravIn.mask[at]);
        h.F32(s.gravOut.forceX[at]); h.F32(s.gravOut.forceY[at]);
    }
    h.Bool(s.gravForceRecalc); h.Bool(bool(s.grav)); result["gravity"] = h.Hex();
    h = {};
    h.Array(s.portalp); h.Array(s.wireless);
    result["portals-wireless"] = h.Hex();
    h = {}; h.PlayerState(s.player); h.PlayerState(s.player2);
    for (const auto &fighter : s.fighters) h.PlayerState(fighter);
    result["stickmen"] = h.Hex();
    h = {};
    h.I32(s.aheat_enable); h.I32(s.edgeMode); h.I32(s.currentTick);
    h.I32(s.gravityMode); h.F32(s.customGravityX); h.F32(s.customGravityY);
    h.I32(s.legacy_enable); h.I32(s.water_equal_test); h.I32(s.pretty_powder);
    h.I32(s.CGOL); h.I32(s.GSPEED); h.I32(s.ISWIRE);
    h.Bool(s.ensureDeterminism); h.Bool(s.force_stacking_check); h.Bool(s.elementRecount);
    h.I32(s.air->airMode); h.F32(s.air->ambientAirTemp); h.F32(s.air->edgePressure);
    h.F32(s.air->edgeVelocityX); h.F32(s.air->edgeVelocityY);
    h.F32(s.air->vorticityCoeff); h.I32(s.air->convectionMode);
    result["options"] = h.Hex();
    h = {};
    h.I32(s.emp_trigger_count); h.I32(s.etrd_life0_count); h.Bool(s.etrd_count_valid);
    h.I32(s.lightningRecreate); h.Bool(s.gravWallChanged);
    h.I32(s.Element_PPIP_ppip_changed); h.I32(s.debug_nextToUpdate);
    for (int c : s.elementCount) h.I32(c);
    result["bookkeeping"] = h.Hex();
    h = {};
    h.Array(s.gol);
    result["gol"] = h.Hex();
    h = {}; h.Array(s.pmap_count); h.Array(s.air->kernel);
    h.Array(s.Element_LOLZ_lolz); h.Array(s.Element_LOVE_love); h.Array(s.Element_PSTN_tempParts);
    result["transient-caches"] = h.Hex();
    h = {};
    h.I32(s.NUM_PARTS); h.I32(s.fighcount); h.I32(s.sandcolour_frame);
    h.I32(s.sandcolour); h.I32(s.sandcolour_interface);
    h.I32(s.emp_decor); h.I32(s.deco_space);
    h.I32(s.replaceModeFlags); h.I32(s.replaceModeSelected);
    h.I32(s.debug_mostRecentlyUpdated);
    for (const auto &sign : s.signs)
    {
        h.I32(sign.x); h.I32(sign.y); h.I32(int(sign.ju));
        h.U64(sign.text.size());
        for (auto ch : sign.text) h.U32(uint32_t(ch));
    }
    result["miscellaneous"] = h.Hex();
    return result;
}

std::string DigestSummary(const Json::Value &fields)
{
    Hash h;
    // JsonCpp sorts keys in its map. Include both the field label and the
    // human-readable fixed-width hexadecimal digest for deterministic order.
    for (const auto &name : fields.getMemberNames())
    {
        h.Text(name); h.Text(fields[name].asString());
    }
    return h.Hex();
}
}
