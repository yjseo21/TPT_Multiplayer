#pragma once
#include "Protocol.h"
#include <set>

namespace multiplayer
{
inline Json::Value PresencePlayers(const Json::Value &message)
{
    auto players = message["players"];
    if (!players.isArray() || players.size() > 4) throw std::runtime_error("invalid presence roster");
    std::set<unsigned> ids;
    for (auto &p : players)
    {
        if (!p["player"].isUInt() || !p["player"].asUInt() || !ids.insert(p["player"].asUInt()).second ||
            !p["name"].isString() || p["name"].asString().empty() || p["name"].asString().size() > 32 ||
            !p["admin"].isBool() || !p["visible"].isBool()) throw std::runtime_error("invalid player presence");
        auto name = p["name"].asString();
        for (auto &c : name) if (static_cast<unsigned char>(c) < 32 || c == 127) c = '?';
        p["name"] = name;
        if (p["visible"].asBool())
        {
            Integer(p, "x", 0, XRES-1); Integer(p, "y", 0, YRES-1); Integer(p, "radius", 0, 16);
            if (!p["drawing"].isBool()) throw std::runtime_error("invalid cursor state");
        }
    }
    return players;
}
}
