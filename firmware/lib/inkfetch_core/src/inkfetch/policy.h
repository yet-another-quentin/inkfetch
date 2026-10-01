// When to fetch, when to refresh and how long to sleep, so the panel is never
// damaged: at least `minRefreshS` between two refreshes, at most `maxStaleS` without
// one (the same image is redrawn if needed). All times are in seconds on a clock
// that keeps running through deep sleep.
#pragma once

#include <cstdint>
#include <optional>

#include "inkfetch/panel.h"

namespace inkfetch {

enum class Trigger {
    PowerOn,        // reset or first boot
    Timer,          // scheduled wake-up
    RefreshButton,  // user asked for a refresh
    ModeButton,     // user switched to the next mode
};

// Kept across deep sleep (RTC memory); lost on power loss, which is safe: the next
// cycle then refreshes.
struct RefreshState {
    bool everRefreshed = false;
    int64_t lastRefreshAt = 0;
};

struct CyclePlan {
    // Skip the refresh when the image is unchanged (If-None-Match, CRC). False for
    // user requests and maintenance.
    bool conditional;
    // The panel has not been refreshed for maxStaleS: redraw even the same image.
    bool maintenance;
    // Non-zero when the last refresh is too recent: do nothing now, retry after this.
    uint32_t waitS;
};

CyclePlan planCycle(Trigger trigger, const RefreshState& state, int64_t now, const Limits& limits);

// A clock that went backwards (e.g. after a brownout) makes the last refresh look
// like it is in the future. Treat it as having just happened: waiting minRefreshS
// is the cautious choice when the real time since the last refresh is unknown.
void normalize(RefreshState& state, int64_t now);

// Shortest sleep, to avoid tight wake loops.
constexpr uint32_t kMinSleepS = 30;

// Seconds until the next wake-up: Retry-After (clamped to the limits) or the poll
// interval, never past the point where the panel needs its maintenance refresh.
// Once that point has passed (the maintenance attempt failed: no Wi-Fi, server
// down...), the normal interval applies again rather than a tight retry loop.
uint32_t nextSleep(const Limits& limits, const RefreshState& state, int64_t now, uint32_t pollS,
                   std::optional<uint32_t> retryAfter);

// OTA checks run at most every `everyHours`, never on a user-triggered wake-up (so
// the picture is not delayed).
bool otaDue(Trigger trigger, std::optional<int64_t> lastCheckAt, int64_t now, uint32_t everyHours);

}  // namespace inkfetch
