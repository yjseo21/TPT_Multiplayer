#include "Protocol.h"
#include "simulation/SimulationData.h"
#include <zlib.h>
#include <bit>
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace multiplayer
{
namespace
{
struct Writer
{
    std::string bytes;
    void U8(uint8_t v) { bytes.push_back(char(v)); }
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) U8(uint8_t(v >> (i * 8))); }
    void U64(uint64_t v) { U32(uint32_t(v)); U32(uint32_t(v >> 32)); }
    void Int(int v) { U32(std::bit_cast<uint32_t>(int32_t(v))); }
    void Float(float v) { U32(std::bit_cast<uint32_t>(v)); }
    void Text(const std::string &s) { U32(uint32_t(s.size())); bytes += s; }
};
struct Reader
{
    std::string_view bytes;
    size_t pos = 0;
    void Need(size_t n) { if (n > bytes.size() - pos) throw std::runtime_error("truncated frame"); }
    uint8_t U8() { Need(1); return uint8_t(bytes[pos++]); }
    uint32_t U32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(U8()) << (i * 8); return v; }
    uint64_t U64() { auto lo = U32(); return uint64_t(lo) | (uint64_t(U32()) << 32); }
    int Int() { return std::bit_cast<int32_t>(U32()); }
    float Float()
    {
        auto bits = U32();
        // Integer validation remains effective even with upstream's fast-math options.
        if ((bits & 0x7f800000U) == 0x7f800000U) throw std::runtime_error("non-finite frame value");
        return std::bit_cast<float>(bits);
    }
    bool Bool() { auto v = U8(); if (v > 1) throw std::runtime_error("invalid boolean"); return v != 0; }
    std::string Text(size_t limit)
    {
        auto n = U32();
        if (n > limit) throw std::runtime_error("oversized text");
        Need(n); auto s = std::string(bytes.substr(pos, n)); pos += n; return s;
    }
    void End() { if (pos != bytes.size()) throw std::runtime_error("trailing frame data"); }
};
void WriteParticle(Writer &w, const Particle &p)
{
    w.Int(p.type); w.Int(p.life); w.Int(p.ctype);
    w.Float(p.x); w.Float(p.y); w.Float(p.vx); w.Float(p.vy); w.Float(p.temp);
    w.Int(p.tmp3); w.Int(p.tmp4); w.Int(p.flags); w.Int(p.tmp); w.Int(p.tmp2); w.U32(p.dcolour);
}
Particle ReadParticle(Reader &r)
{
    Particle p{};
    p.type = r.Int(); p.life = r.Int(); p.ctype = r.Int();
    p.x = r.Float(); p.y = r.Float(); p.vx = r.Float(); p.vy = r.Float(); p.temp = r.Float();
    p.tmp3 = r.Int(); p.tmp4 = r.Int(); p.flags = r.Int(); p.tmp = r.Int(); p.tmp2 = r.Int(); p.dcolour = r.U32();
    if (p.type <= 0 || p.type >= PT_NUM || p.x < -1 || p.y < -1 || p.x > XRES || p.y > YRES)
        throw std::runtime_error("invalid particle");
    return p;
}
void WritePlayer(Writer &w, const playerst &p)
{
    w.U8(p.comm); w.U8(p.pcomm); w.Int(p.elem);
    for (auto v : p.legs) w.Float(v);
    for (auto v : p.accs) w.Float(v);
    w.U8(p.spwn); w.U32(p.frames); w.U8(p.rocketBoots); w.U8(p.fan); w.Int(p.spawnID);
}
void ReadPlayer(Reader &r, playerst &p)
{
    p.comm = char(r.U8()); p.pcomm = char(r.U8()); p.elem = r.Int();
    if (p.elem < 0 || p.elem >= PT_NUM) throw std::runtime_error("invalid player element");
    for (auto &v : p.legs) v = r.Float();
    for (auto &v : p.accs) v = r.Float();
    p.spwn = char(r.U8()); p.frames = r.U32(); p.rocketBoots = r.Bool(); p.fan = r.Bool(); p.spawnID = r.Int();
}
}

std::string EncodeFrame(const RenderableSimulation &s, uint64_t tick, uint64_t revision, bool paused)
{
    Writer w;
    w.U64(tick); w.U64(revision); w.U8(paused);
    w.Int(s.currentTick); w.Int(s.emp_decor); w.Int(s.aheat_enable); w.Int(s.edgeMode);
    uint32_t count = 0;
    for (int i = 0; i < s.parts.active; ++i) if (s.parts[i].type) ++count;
    w.U32(count);
    for (int i = 0; i < s.parts.active; ++i) if (s.parts[i].type) { w.U32(i); WriteParticle(w, s.parts[i]); }
    for (int y = 0; y < YCELLS; ++y) for (int x = 0; x < XCELLS; ++x)
    {
        w.Float(s.vx[y][x]); w.Float(s.vy[y][x]); w.Float(s.pv[y][x]); w.Float(s.hv[y][x]);
        w.U8(s.bmap[y][x]); w.U8(s.emap[y][x]);
        w.Float(s.gravIn.mass[{x, y}]); w.U32(s.gravIn.mask[{x, y}]);
        w.Float(s.gravOut.forceX[{x, y}]); w.Float(s.gravOut.forceY[{x, y}]);
    }
    // Preserve maps exactly: rebuilding from positions loses the ordering of stacked particles.
    for (int y = 0; y < YRES; ++y) for (int x = 0; x < XRES; ++x)
    { w.Int(s.pmap[y][x]); w.Int(s.photons[y][x]); }
    WritePlayer(w, s.player); WritePlayer(w, s.player2);
    for (auto &p : s.fighters) WritePlayer(w, p);
    w.U32(uint32_t(s.signs.size()));
    for (auto &s : s.signs) { w.Int(s.x); w.Int(s.y); w.Int(s.ju); w.Text(s.text.ToUtf8()); }
    if (w.bytes.size() > MaxFrameBytes) throw std::runtime_error("frame exceeds protocol limit");
    uLongf size = compressBound(uLong(w.bytes.size()));
    std::string compressed(size, '\0');
    if (compress2(reinterpret_cast<Bytef *>(compressed.data()), &size,
        reinterpret_cast<const Bytef *>(w.bytes.data()), uLong(w.bytes.size()), Z_BEST_SPEED) != Z_OK)
        throw std::runtime_error("frame compression failed");
    compressed.resize(size);
    Writer packet;
    packet.bytes = "TPM1"; packet.U32(uint32_t(w.bytes.size())); packet.bytes += compressed;
    if (packet.bytes.size() > MaxFrameBytes) throw std::runtime_error("compressed frame exceeds limit");
    return packet.bytes;
}

Frame DecodeFrame(std::string_view packet)
{
    if (packet.size() < 9 || packet.size() > MaxFrameBytes || packet.substr(0, 4) != "TPM1")
        throw std::runtime_error("invalid frame header");
    Reader header{packet.substr(4, 4)};
    auto expected = header.U32();
    if (!expected || expected > MaxFrameBytes) throw std::runtime_error("invalid inflated size");
    std::string raw(expected, '\0');
    uLongf size = expected;
    uLong sourceSize = uLong(packet.size() - 8);
    if (uncompress2(reinterpret_cast<Bytef *>(raw.data()), &size,
        reinterpret_cast<const Bytef *>(packet.data() + 8), &sourceSize) != Z_OK ||
        size != expected || sourceSize != packet.size() - 8)
        throw std::runtime_error("invalid compressed frame");
    Reader r{raw};
    Frame f;
    f.tick = r.U64(); f.revision = r.U64(); f.paused = r.Bool();
    f.world = std::make_unique<RenderableSimulation>();
    auto &s = *f.world;
    s.currentTick = r.Int(); s.emp_decor = r.Int(); s.aheat_enable = r.Int(); s.edgeMode = r.Int();
    if (s.edgeMode < 0 || s.edgeMode >= NUM_EDGEMODES) throw std::runtime_error("invalid edge mode");
    auto count = r.U32();
    if (count > NPART) throw std::runtime_error("too many particles");
    int previous = -1;
    for (uint32_t n = 0; n < count; ++n)
    {
        auto id = r.U32();
        if (id >= NPART || int(id) <= previous) throw std::runtime_error("invalid particle slot");
        previous = int(id); s.parts.data[id] = ReadParticle(r); s.parts.active = int(id) + 1;
    }
    for (int y = 0; y < YCELLS; ++y) for (int x = 0; x < XCELLS; ++x)
    {
        s.vx[y][x] = r.Float(); s.vy[y][x] = r.Float(); s.pv[y][x] = r.Float(); s.hv[y][x] = r.Float();
        s.bmap[y][x] = r.U8(); s.emap[y][x] = r.U8();
        if (s.bmap[y][x] >= UI_WALLCOUNT) throw std::runtime_error("invalid wall");
        s.gravIn.mass[{x,y}] = r.Float(); s.gravIn.mask[{x,y}] = r.U32();
        s.gravOut.forceX[{x,y}] = r.Float(); s.gravOut.forceY[{x,y}] = r.Float();
    }
    auto mapEntry = [&]() {
        int v = r.Int();
        if (v && (ID(v) < 0 || ID(v) >= s.parts.active || TYP(v) <= 0 || TYP(v) >= PT_NUM || !s.parts[ID(v)].type))
            throw std::runtime_error("invalid particle map reference");
        return v;
    };
    for (int y = 0; y < YRES; ++y) for (int x = 0; x < XRES; ++x)
    { s.pmap[y][x] = mapEntry(); s.photons[y][x] = mapEntry(); }
    ReadPlayer(r, s.player); ReadPlayer(r, s.player2);
    for (auto &p : s.fighters) ReadPlayer(r, p);
    count = r.U32();
    if (count > MAXSIGNS) throw std::runtime_error("too many signs");
    for (uint32_t i = 0; i < count; ++i)
    {
        int x = r.Int(), y = r.Int(), j = r.Int();
        if (x < 0 || x >= XRES || y < 0 || y >= YRES || j < 0 || j >= sign::Max)
            throw std::runtime_error("invalid sign");
        auto text = r.Text(4096);
        s.signs.emplace_back(ByteString(text).FromUtf8(), x, y, sign::Justification(j));
    }
    r.End();
    return f;
}

Json::Value ParseJson(std::string_view text)
{
    if (text.size() > MaxCommandBytes) throw std::runtime_error("message too large");
    Json::CharReaderBuilder b;
    b["failIfExtra"] = true; b["rejectDupKeys"] = true; b["allowComments"] = false;
    b["stackLimit"] = 16;
    auto reader = std::unique_ptr<Json::CharReader>(b.newCharReader());
    Json::Value v; std::string error;
    if (!reader->parse(text.data(), text.data() + text.size(), &v, &error) || !v.isObject())
        throw std::runtime_error("invalid JSON object");
    return v;
}
std::string JsonText(const Json::Value &value)
{
    Json::StreamWriterBuilder b; b["indentation"] = ""; return Json::writeString(b, value);
}
int Integer(const Json::Value &v, const char *field, int low, int high)
{
    if (!v[field].isInt() || v[field].asInt() < low || v[field].asInt() > high)
        throw std::runtime_error(std::string("invalid ") + field);
    return v[field].asInt();
}
uint64_t Sequence(const Json::Value &v)
{
    if (!v["seq"].isUInt64() || !v["seq"].asUInt64() || v["seq"].asUInt64() > 9007199254740991ULL)
        throw std::runtime_error("invalid sequence");
    return v["seq"].asUInt64();
}
unsigned Command::Cost() const
{
    if (kind == ToolAction)
    {
        if (tool == "DEFAULT_WL_FAN" && action == "line" && !dragging) return XRES*YRES;
        if (action == "fill") return XRES*YRES;
        if (action == "box") return unsigned((std::abs(x2-x1)+1)*(std::abs(y2-y1)+1));
        return unsigned((std::max(std::abs(x2-x1), std::abs(y2-y1))+1)*(2*radius+1)*(2*radiusY+1));
    }
    return kind == Brush ? unsigned((std::max(std::abs(x2-x1), std::abs(y2-y1)) + 1) * (2*radius+1) * (2*radius+1)) : 1;
}
Command ParseCommand(const Json::Value &v, bool admin)
{
    Command c{}; c.sequence = Sequence(v);
    if (!v["op"].isString()) throw std::runtime_error("missing operation");
    auto op = v["op"].asString();
    if (op == "brush")
    {
        c.kind = Command::Brush;
        c.x1 = Integer(v, "x1", CELL, XRES-CELL-1); c.y1 = Integer(v, "y1", CELL, YRES-CELL-1);
        c.x2 = Integer(v, "x2", CELL, XRES-CELL-1); c.y2 = Integer(v, "y2", CELL, YRES-CELL-1);
        c.radius = Integer(v, "radius", 0, 16); c.element = Integer(v, "element", 0, PT_NUM-1);
        if (c.element && !SimulationData::CRef().elements[c.element].Enabled) throw std::runtime_error("disabled element");
        if (c.Cost() > 65536) throw std::runtime_error("stroke too large; split it into segments");
    }
    else if (op == "tool")
    {
        c.kind = Command::ToolAction;
        if (!v["tool"].isString() || v["tool"].asString().size() > 80) throw std::runtime_error("invalid tool identifier");
        c.tool = v["tool"].asString();
        c.action = v.get("action", "line").asString(); c.brush = v.get("brush", "ellipse").asString();
        if (c.action != "point" && c.action != "line" && c.action != "end" && c.action != "box" && c.action != "fill" && c.action != "click" && c.action != "drag")
            throw std::runtime_error("invalid tool action");
        if (c.brush != "ellipse" && c.brush != "ellipse-perfect" && c.brush != "rectangle" && c.brush != "triangle") throw std::runtime_error("invalid brush shape");
        c.x1 = Integer(v, "x1", CELL, XRES-CELL-1); c.y1 = Integer(v, "y1", CELL, YRES-CELL-1);
        c.x2 = v.isMember("x2") ? Integer(v, "x2", CELL, XRES-CELL-1) : c.x1;
        c.y2 = v.isMember("y2") ? Integer(v, "y2", CELL, YRES-CELL-1) : c.y1;
        // The game's brushes: up to 200 cells each way, horizontal and vertical radius.
        c.radius = Integer(v, "radius", 0, 200);
        c.radiusY = v.isMember("radiusY") ? Integer(v, "radiusY", 0, 200) : c.radius;
        c.flags = v.isMember("flags") ? Integer(v, "flags", 0, 3) : 0;
        c.replace = v.isMember("replace") ? Integer(v, "replace", 0, PT_NUM-1) : 0;
        c.strength = v.isMember("strength") ? Integer(v, "strength", 1, 1000) : 100;
        auto flag = [&](const char *key, bool fallback) {
            if (!v.isMember(key)) return fallback;
            if (!v[key].isBool()) throw std::runtime_error("invalid modifier flag");
            return v[key].asBool();
        };
        c.dragging = flag("dragging", true); c.shift = flag("shift", false); c.ctrl = flag("ctrl", false); c.alt = flag("alt", false);
        if (c.Cost() > ToolCommandCostLimit) throw std::runtime_error("tool operation too large; split it into segments");
    }
    else
    {
        if (!admin) throw std::runtime_error("administrator permission required");
        if (op == "pause")
        {
            c.kind = Command::Pause;
            if (!v["paused"].isBool()) throw std::runtime_error("invalid paused value");
            c.paused = v["paused"].asBool();
        }
        else if (op == "step") c.kind = Command::Step;
        else if (op == "clear") c.kind = Command::Clear;
        else throw std::runtime_error("unknown operation");
    }
    return c;
}
}
