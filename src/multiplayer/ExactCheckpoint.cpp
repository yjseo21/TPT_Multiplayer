#include "ExactCheckpoint.h"
#include "World.h"
#include "simulation/Simulation.h"
#include "simulation/Air.h"
#include "simulation/Sign.h"
#include <zlib.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace multiplayer
{
namespace
{
constexpr size_t MaxUncompressed = 128U * 1024 * 1024;
constexpr size_t MaxPacket = 64U * 1024 * 1024;
constexpr uint32_t FormatVersion = 1;
// The application must use the same engine build and simulation dimensions.
constexpr char Magic[] = "TPTXCP1!";

class Writer
{
public:
    static constexpr bool Reading = false;
    std::string bytes;
    // Capture runs on the room's thread: one append per value, into a buffer
    // sized for a full world, rather than one checked push per byte.
    explicit Writer(std::string buffer) : bytes(std::move(buffer)) { bytes.clear(); bytes.reserve(26U * 1024 * 1024); }
    void Byte(uint8_t &v) { bytes.push_back(char(v)); }
    void Unsigned(uint32_t &v) { Append(v); }
    void Wide(uint64_t &v) { Append(v); }
    void Raw(const void *data, size_t size) { bytes.append(static_cast<const char *>(data), size); }
    void End() {}
private:
    template<class T> void Append(T v)
    {
        char little[sizeof(T)];
        for (size_t i=0;i<sizeof(T);++i) little[i] = char(v >> (8*i));
        bytes.append(little, sizeof(T));
    }
};
class Reader
{
public:
    static constexpr bool Reading = true;
    explicit Reader(std::string_view packet) : bytes(packet) {}
    std::string_view bytes;
    size_t offset = 0;
    void Byte(uint8_t &v)
    {
        if (offset == bytes.size()) throw std::runtime_error("checkpoint truncated");
        v = uint8_t(bytes[offset++]);
    }
    void Unsigned(uint32_t &v)
    {
        if (bytes.size()-offset < 4) throw std::runtime_error("checkpoint truncated");
        v = 0;
        for (int i=0;i<4;++i) v |= uint32_t(uint8_t(bytes[offset++])) << (8*i);
    }
    void Wide(uint64_t &v)
    {
        if (bytes.size()-offset < 8) throw std::runtime_error("checkpoint truncated");
        v = 0;
        for (int i=0;i<8;++i) v |= uint64_t(uint8_t(bytes[offset++])) << (8*i);
    }
    void Raw(void *data, size_t size)
    {
        if (bytes.size()-offset < size) throw std::runtime_error("checkpoint truncated");
        std::memcpy(data, bytes.data()+offset, size); offset += size;
    }
    void End()
    {
        if (offset != bytes.size()) throw std::runtime_error("checkpoint has trailing state data");
    }
};
// The format is little-endian, field by field. On little-endian machines, arrays
// of 4-byte values, and particles (fourteen 4-byte fields, in format order),
// are already laid out as the format: they are copied in one piece.
template<class T> constexpr bool BulkValue = std::endian::native == std::endian::little &&
    (std::is_same_v<T, int> || std::is_same_v<T, unsigned int> || std::is_same_v<T, float>) && sizeof(T) == 4;
constexpr bool ParticlePacked = std::endian::native == std::endian::little && sizeof(Particle) == 56 &&
    std::is_trivially_copyable_v<Particle> && sizeof(int) == 4 && sizeof(float) == 4 &&
    offsetof(Particle, type) == 0 && offsetof(Particle, life) == 4 && offsetof(Particle, ctype) == 8 &&
    offsetof(Particle, x) == 12 && offsetof(Particle, y) == 16 && offsetof(Particle, vx) == 20 &&
    offsetof(Particle, vy) == 24 && offsetof(Particle, temp) == 28 && offsetof(Particle, tmp3) == 32 &&
    offsetof(Particle, tmp4) == 36 && offsetof(Particle, flags) == 40 && offsetof(Particle, tmp) == 44 &&
    offsetof(Particle, tmp2) == 48 && offsetof(Particle, dcolour) == 52;
template<class T> constexpr bool Bulk = BulkValue<T> || (std::is_same_v<T, Particle> && ParticlePacked);

template<class A> void Field(A &a, bool &v)
{
    uint8_t value = v ? 1 : 0;
    a.Byte(value);
    if constexpr (A::Reading)
    {
        if (value > 1) throw std::runtime_error("checkpoint has invalid boolean");
        v = value != 0;
    }
}
template<class A> void Field(A &a, uint8_t &v) { a.Byte(v); }
template<class A> void Field(A &a, char &v)
{
    uint8_t value = static_cast<uint8_t>(v);
    a.Byte(value);
    if constexpr (A::Reading) v = static_cast<char>(value);
}
template<class A> void Field(A &a, int &v)
{
    static_assert(sizeof(int)==4);
    uint32_t bits = std::bit_cast<uint32_t>(v);
    a.Unsigned(bits);
    if constexpr (A::Reading) v = std::bit_cast<int>(bits);
}
template<class A> void Field(A &a, unsigned int &v)
{
    static_assert(sizeof(unsigned int)==4);
    a.Unsigned(v);
}
template<class A> void Field(A &a, uint64_t &v) { a.Wide(v); }
template<class A> void Field(A &a, float &v)
{
    static_assert(sizeof(float)==4);
    uint32_t bits = std::bit_cast<uint32_t>(v);
    a.Unsigned(bits);
    if constexpr (A::Reading) v = std::bit_cast<float>(bits);
}
template<class A> void Field(A &a, Particle &p);
template<class A, class T, size_t N> void Field(A &a, T (&values)[N])
{
    if constexpr (Bulk<T>) a.Raw(values, sizeof(values));
    else for (auto &value : values) Field(a, value);
}
template<class A, class T, size_t N> void Field(A &a, std::array<T,N> &values)
{
    if constexpr (Bulk<T>) a.Raw(values.data(), sizeof(T)*N);
    else for (auto &value : values) Field(a, value);
}
template<class A> void Field(A &a, Particle &p)
{
    Field(a,p.type); Field(a,p.life); Field(a,p.ctype);
    Field(a,p.x); Field(a,p.y); Field(a,p.vx); Field(a,p.vy); Field(a,p.temp);
    Field(a,p.tmp3); Field(a,p.tmp4); Field(a,p.flags);
    Field(a,p.tmp); Field(a,p.tmp2); Field(a,p.dcolour);
}
template<class A> void Field(A &a, playerst &p)
{
    Field(a,p.comm); Field(a,p.pcomm); Field(a,p.elem);
    Field(a,p.legs); Field(a,p.accs); Field(a,p.spwn);
    Field(a,p.frames); Field(a,p.rocketBoots); Field(a,p.fan); Field(a,p.spawnID); Field(a,p.controller);
}
template<class A> void SignFields(A &a, std::vector<sign> &signs)
{
    uint32_t count = uint32_t(signs.size());
    Field(a,count);
    if (count > MAXSIGNS) throw std::runtime_error("too many signs in checkpoint");
    if constexpr (A::Reading) signs.clear();
    for (uint32_t i=0;i<count;++i)
    {
        int x=0,y=0,ju=0;
        String content;
        if constexpr (!A::Reading)
        {
            x=signs[i].x; y=signs[i].y; ju=int(signs[i].ju); content=signs[i].text;
        }
        Field(a,x); Field(a,y); Field(a,ju);
        if constexpr (!A::Reading)
            if (content.size() > 2048) throw std::runtime_error("oversize sign text in checkpoint");
        uint32_t n=uint32_t(content.size()); Field(a,n);
        if (n > 2048 || ju < 0 || ju >= sign::Max)
            throw std::runtime_error("invalid sign in checkpoint");
        if constexpr (A::Reading) content.resize(n);
        for (uint32_t j=0;j<n;++j)
        {
            uint32_t ch = uint32_t(content[j]); Field(a,ch);
            if constexpr (A::Reading)
            {
                if (ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF))
                    throw std::runtime_error("invalid sign codepoint");
                content[j]=char32_t(ch);
            }
        }
        if constexpr (A::Reading) signs.emplace_back(content,x,y,sign::Justification(ju));
    }
}

template<class A> void State(A &a, World &w)
{
    Simulation &s=*w.sim;
    if (s.grav || s.useLuaCallbacks || s.frameTime)
        throw std::runtime_error("exact checkpoint requires gravity-disabled, callback-free, quiescent simulation");
    uint32_t xres=XRES,yres=YRES,capacity=NPART,channels=CHANNELS, version=FormatVersion;
    Field(a,version); Field(a,xres); Field(a,yres); Field(a,capacity); Field(a,channels);
    uint32_t buildLength=uint32_t(std::string_view(BuildId).size());
    Field(a,buildLength);
    if (buildLength!=std::string_view(BuildId).size())
        throw std::runtime_error("checkpoint engine build mismatch");
    for (unsigned char expected : std::string_view(BuildId))
    {
        uint8_t actual=expected;
        Field(a,actual);
        if (actual!=expected) throw std::runtime_error("checkpoint build identifier mismatch");
    }
    if (version!=FormatVersion || xres!=XRES || yres!=YRES || capacity!=NPART || channels!=CHANNELS)
        throw std::runtime_error("incompatible exact checkpoint dimensions/version");
    Field(a,w.tick); Field(a,w.revision); Field(a,w.resetGeneration); Field(a,w.paused);
    Field(a,s.frameCount); Field(a,s.currentTick); Field(a,s.emp_decor);
    auto rng=s.rng.state(); Field(a,rng); if constexpr (A::Reading) s.rng.state(rng);
    int freeHead=s.parts.FreeHead(); int active=s.parts.active;
    Field(a,freeHead); Field(a,active);
    if (active<0 || active>NPART || freeHead < -1 || freeHead >= active)
        throw std::runtime_error("bad particle allocation metadata");
    // Preserve even slots beyond active: they can contain stale values which
    // must not be silently changed by checkpoint/recovery.
    Field(a,s.parts.data);
    if constexpr (A::Reading) s.parts.RestoreExactFreeList(freeHead,active);
    Field(a,s.pmap); Field(a,s.photons); Field(a,s.pmap_count);
    Field(a,s.pv); Field(a,s.vx); Field(a,s.vy); Field(a,s.hv);
    Field(a,s.air->opv); Field(a,s.air->ovx); Field(a,s.air->ovy); Field(a,s.air->ohv);
    Field(a,s.bmap); Field(a,s.emap);
    Field(a,s.air->bmap_blockair); Field(a,s.air->bmap_blockairh);
    Field(a,s.fvx); Field(a,s.fvy); Field(a,s.air->kernel);
    // Gravity disabled: retain dormant plane values without starting a worker.
    for (int y=0;y<YCELLS;++y) for (int x=0;x<XCELLS;++x)
    {
        auto pos=Vec2{x,y};
        Field(a,s.gravIn.mass[pos]); Field(a,s.gravIn.mask[pos]);
        Field(a,s.gravOut.forceX[pos]); Field(a,s.gravOut.forceY[pos]);
    }
    Field(a,s.gravForceRecalc);
    Field(a,s.portalp); Field(a,s.wireless);
    Field(a,s.player); Field(a,s.player2); Field(a,s.fighters);
    Field(a,s.elementCount); Field(a,s.ISWIRE);
    Field(a,s.replaceModeSelected); Field(a,s.replaceModeFlags);
    Field(a,s.debug_nextToUpdate); Field(a,s.debug_mostRecentlyUpdated);
    Field(a,s.force_stacking_check); Field(a,s.emp_trigger_count);
    Field(a,s.etrd_count_valid); Field(a,s.etrd_life0_count);
    Field(a,s.lightningRecreate); Field(a,s.gravWallChanged);
    Field(a,s.CGOL); Field(a,s.GSPEED); Field(a,s.gol);
    Field(a,s.Element_LOLZ_lolz); Field(a,s.Element_LOVE_love);
    Field(a,s.Element_PSTN_tempParts); Field(a,s.Element_PPIP_ppip_changed);
    Field(a,s.aheat_enable); Field(a,s.edgeMode);
    Field(a,s.gravityMode); Field(a,s.customGravityX); Field(a,s.customGravityY);
    Field(a,s.legacy_enable); Field(a,s.water_equal_test); Field(a,s.pretty_powder);
    Field(a,s.sandcolour_frame); Field(a,s.deco_space);
    Field(a,s.elementRecount); Field(a,s.fighcount);
    Field(a,s.ensureDeterminism); Field(a,s.NUM_PARTS);
    Field(a,s.sandcolour); Field(a,s.sandcolour_interface);
    Field(a,s.air->airMode); Field(a,s.air->ambientAirTemp);
    Field(a,s.air->edgePressure); Field(a,s.air->edgeVelocityX);
    Field(a,s.air->edgeVelocityY); Field(a,s.air->vorticityCoeff);
    Field(a,s.air->convectionMode);
    SignFields(a,s.signs);
}

void ValidateRestoredWorld(const World &w)
{
    const Simulation &s=*w.sim;
    if (s.NUM_PARTS<0 || s.NUM_PARTS>NPART || s.fighcount>MAX_FIGHTERS)
        throw std::runtime_error("invalid checkpoint particle or fighter count");
    for (int i=0;i<s.parts.active;++i)
        if (s.parts[i].type<0 || s.parts[i].type>=PT_NUM)
            throw std::runtime_error("checkpoint contains unsupported particle type");
    for (const auto &row : s.pmap)
        for (int cell : row)
            if (cell && (ID(cell)<0 || ID(cell)>=s.parts.active))
                throw std::runtime_error("checkpoint has invalid particle map index");
    for (const auto &row : s.photons)
        for (int cell : row)
            if (cell && (ID(cell)<0 || ID(cell)>=s.parts.active))
                throw std::runtime_error("checkpoint has invalid energy map index");
    for (const auto &channel : s.portalp)
        for (const auto &direction : channel)
            for (const auto &p : direction)
                if (p.type<0 || p.type>=PT_NUM)
                    throw std::runtime_error("checkpoint contains unsupported portal particle type");
}

void Put32(std::string &out, uint32_t v)
{
    for (int i=0;i<4;++i) out.push_back(char(v >> (8*i)));
}
uint32_t Get32(std::string_view packet, size_t at)
{
    if (packet.size()-at<4) throw std::runtime_error("truncated checkpoint header");
    uint32_t v=0;
    for (int i=0;i<4;++i) v |= uint32_t(uint8_t(packet[at+i])) << (8*i);
    return v;
}
}

std::string CaptureExactCheckpointRaw(World &w, std::string buffer)
{
    Writer writer(std::move(buffer));
    State(writer,w);
    if (writer.bytes.empty() || writer.bytes.size()>MaxUncompressed)
        throw std::runtime_error("uncompressed checkpoint exceeds maximum size");
    return std::move(writer.bytes);
}

std::string CompressExactCheckpointRaw(std::string_view raw)
{
    if (raw.empty() || raw.size()>MaxUncompressed)
        throw std::runtime_error("invalid uncompressed checkpoint size");
    uLongf bound=compressBound(uLong(raw.size()));
    std::string compressed(size_t(bound),'\0');
    int rc=compress2(reinterpret_cast<Bytef*>(compressed.data()), &bound,
        reinterpret_cast<const Bytef*>(raw.data()), uLong(raw.size()),Z_BEST_SPEED);
    if (rc!=Z_OK) throw std::runtime_error("checkpoint compression failure");
    compressed.resize(size_t(bound));
    if (compressed.size()+20>MaxPacket) throw std::runtime_error("checkpoint exceeds packet limit");
    std::string packet(Magic,sizeof(Magic)-1);
    Put32(packet,FormatVersion);
    Put32(packet,uint32_t(raw.size()));
    auto crc=crc32(0,reinterpret_cast<const Bytef*>(raw.data()),uInt(raw.size()));
    Put32(packet,uint32_t(crc));
    packet += compressed;
    return packet;
}

std::string EncodeExactCheckpoint(World &w)
{
    return CompressExactCheckpointRaw(CaptureExactCheckpointRaw(w));
}

void RestoreExactCheckpoint(World &w, std::string_view packet)
{
    if (packet.size()<20 || packet.size()>MaxPacket ||
        packet.substr(0,8)!=std::string_view(Magic,8) || Get32(packet,8)!=FormatVersion)
        throw std::runtime_error("unsupported exact checkpoint packet");
    uint32_t rawLength=Get32(packet,12), expectedCRC=Get32(packet,16);
    if (!rawLength || rawLength>MaxUncompressed)
        throw std::runtime_error("invalid exact checkpoint uncompressed length");
    std::string raw(rawLength,'\0');
    z_stream zs{};
    zs.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(packet.data()+20));
    zs.avail_in=uInt(packet.size()-20);
    zs.next_out=reinterpret_cast<Bytef*>(raw.data());
    zs.avail_out=rawLength;
    if (inflateInit(&zs)!=Z_OK) throw std::runtime_error("checkpoint inflate initialization failed");
    int rc=inflate(&zs,Z_FINISH);
    bool complete=rc==Z_STREAM_END && zs.total_out==rawLength && zs.avail_in==0;
    inflateEnd(&zs);
    if (!complete ||
        crc32(0,reinterpret_cast<const Bytef*>(raw.data()),uInt(raw.size()))!=expectedCRC)
        throw std::runtime_error("corrupt exact checkpoint compressed state or checksum");
    World candidate;
    Reader reader(raw);
    State(reader,candidate);
    reader.End();
    ValidateRestoredWorld(candidate);
    // Only replace after successfully validating the complete checkpoint.
    w.sim.swap(candidate.sim);
    std::swap(w.tick,candidate.tick);
    std::swap(w.revision,candidate.revision);
    std::swap(w.resetGeneration,candidate.resetGeneration);
    std::swap(w.paused,candidate.paused);
}
}
