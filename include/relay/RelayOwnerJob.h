#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace opensmarttimer { namespace relay {

// One bounded asynchronous owner operation. The caller supplies next_id()
// plus the current boot ID. Old IDs never execute again, even after their
// result has been replaced; the most recent exact retry replays its result.
// Runtime protects this object with its spinlock. No pointer escapes to a
// request buffer, and a rejected submission cannot change running arguments.
class RelayOwnerJob {
public:
    enum class Kind : uint8_t { Mint, Revoke, Extend };
    enum class State : uint8_t { Idle, Pending, Running, Done };
    enum class Check : uint8_t { New, Replay, Busy, Conflict };
    struct Request {
        uint32_t id = 0;
        Kind kind = Kind::Mint;
        uint32_t ttl_ms = 0;
        char argument[24] = {};  // label for mint, fingerprint for revoke
    };

    Check check(const Request& candidate) const {
        if (candidate.id && candidate.id == request.id) {
            return candidate.kind == request.kind && candidate.ttl_ms == request.ttl_ms &&
                !strcmp(candidate.argument, request.argument) ? Check::Replay : Check::Conflict;
        }
        if (candidate.id != next_id() || !candidate.id) return Check::Conflict;
        return busy() ? Check::Busy : Check::New;
    }
    void accept(const Request& candidate) { request = candidate; state = State::Pending; }
    bool busy() const { return state == State::Pending || state == State::Running; }
    uint32_t next_id() const { return request.id == UINT32_MAX ? 0 : request.id + 1; }
    Request request{};
    State state = State::Idle;
};

// Share-URL cache slot for a newly created link: an empty slot, else one
// whose link is no longer live (revoked or expired). Never a live link's
// URL. Returns N when every slot holds a live link (the caller then skips
// caching rather than evicting). Slot needs a NUL-terminated `fp` member.
template <typename Slot, size_t N>
size_t chooseShareSlot(const Slot (&slots)[N], const char (*live)[24], size_t live_count) {
    for (size_t i = 0; i < N; ++i) if (!slots[i].fp[0]) return i;
    for (size_t i = 0; i < N; ++i) {
        bool is_live = false;
        for (size_t j = 0; j < live_count; ++j) is_live |= strcmp(slots[i].fp, live[j]) == 0;
        if (!is_live) return i;
    }
    return N;
}

}}  // namespace opensmarttimer::relay
