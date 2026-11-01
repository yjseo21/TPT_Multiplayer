#pragma once
#include <json/json.h>
#include <cstdint>
#include <string>

namespace multiplayer
{
class World;
// A diagnostic field-by-field, endian-independent digest. This is intentionally
// NOT an exact checkpoint schema or a cryptographic authentication primitive.
// It exposes the first differing state group between independent replay runs.
Json::Value DiagnosticDigests(const World &world);
std::string DigestSummary(const Json::Value &fields);
}
