/*
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "core/model.h"

#include <string.h>

namespace vdm
{

const char *linkStateName(LinkState s)
{
    switch (s) {
    case LinkState::Disconnected:
        return "disconnected";
    case LinkState::Connected:
        return "connected";
    case LinkState::Incompatible:
        return "incompatible";
    case LinkState::Updating:
        return "updating";
    }
    return "?";
}

void Model::addLog(uint32_t ms, uint8_t level, const char *text, size_t len)
{
    uint8_t idx = static_cast<uint8_t>((log_head + log_count) % kLogSize);
    if (log_count == kLogSize) {
        log_head = static_cast<uint8_t>((log_head + 1u) % kLogSize);
    } else {
        log_count++;
    }
    LogEntry &e = log[idx];
    e.ms = ms;
    e.level = level;
    if (len > VDM_LOG_TEXT_MAX) {
        len = VDM_LOG_TEXT_MAX;
    }
    memcpy(e.text, text, len);
    e.text[len] = '\0';
}

} // namespace vdm
