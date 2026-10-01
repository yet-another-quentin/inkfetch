#include "inkfetch/policy.h"

#include <algorithm>

namespace inkfetch {

CyclePlan planCycle(Trigger trigger, const RefreshState& state, int64_t now, const Limits& limits) {
    if (!state.everRefreshed) return {false, true, 0};
    const int64_t since = now - state.lastRefreshAt;
    // Unknown time since the last refresh (see normalize): be cautious.
    if (since < 0) return {false, true, limits.minRefreshS};
    const bool maintenance = since >= int64_t(limits.maxStaleS);
    const bool user = trigger != Trigger::Timer;
    const uint32_t wait = since < int64_t(limits.minRefreshS) ? uint32_t(limits.minRefreshS - since) : 0;
    return {!(maintenance || user), maintenance, wait};
}

void normalize(RefreshState& state, int64_t now) {
    if (state.everRefreshed && state.lastRefreshAt > now) state.lastRefreshAt = now;
}

uint32_t nextSleep(const Limits& limits, const RefreshState& state, int64_t now, uint32_t pollS,
                   std::optional<uint32_t> retryAfter) {
    uint64_t sleep = retryAfter ? std::clamp<uint32_t>(*retryAfter, limits.minRefreshS, limits.maxStaleS)
                                : pollS;
    if (state.everRefreshed) {
        const int64_t untilStale = state.lastRefreshAt + int64_t(limits.maxStaleS) - now;
        if (untilStale > 0 && untilStale < int64_t(sleep)) sleep = uint64_t(untilStale);
    }
    return uint32_t(std::max<uint64_t>(sleep, kMinSleepS));
}

bool otaDue(Trigger trigger, std::optional<int64_t> lastCheckAt, int64_t now, uint32_t everyHours) {
    if (trigger == Trigger::RefreshButton || trigger == Trigger::ModeButton) return false;
    if (!lastCheckAt) return true;
    const int64_t since = now - *lastCheckAt;
    return since < 0 || since >= int64_t(everyHours) * 3600;
}

}  // namespace inkfetch
