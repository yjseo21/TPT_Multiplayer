#pragma once
#include "Protocol.h"

namespace multiplayer
{
// Canonical *validated command* representation. Never record incoming raw JSON:
// it may contain untrusted extras, credentials, or different default values.
Json::Value RecordCommand(const Command &command);
Command ReadRecordedCommand(const Json::Value &json);
}
