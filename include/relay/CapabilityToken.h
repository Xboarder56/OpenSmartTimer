#pragma once

// CapabilityToken — HMAC-SHA256 timer-only share-link minting and
// verification for ost-relay/1, plus the timer's ECDSA P-256 signing key.
// See docs/RELAY_PROTOCOL.md §4 (wire format) and §9 (storage invariants).

#include <stdint.h>
#include <stddef.h>

#include "RelayEnvelope.h"

namespace opensmarttimer { namespace relay {

// One entry in the NVS token table. Fixed layout; do not add fields
// without bumping a stored-schema version.
struct TokenRecord {
    uint8_t  fp[kTokenFingerprintBytes];
    uint16_t scope_mask;             // bitfield indexed by Scope
    uint32_t exp_up_ms;              // monotonic uptime (current-boot)
    uint64_t boot_ref;               // encoded current boot_id at mint
    bool     revoked;
    char     label[24];              // owner-visible short label; may be empty
    // 8-byte alignment padding is intentional and reserved.
};

// Bitfield indices for TokenRecord::scope_mask.
enum class Scope : uint8_t {
    View        = 0,   // MUST be set in every mint (RELAY_PROTOCOL.md §4)
    PauseResume = 1,
    Adjust      = 2,
    Release     = 3,
    Start       = 4,   // included in current partner links
    Lock        = 5,   // included in current partner links
    Identify    = 6,
    _Count      = 7,
};

// Result of Verify(): why the token was accepted or rejected. Verify
// never mutates state; it only reads.
enum class VerifyResult : uint8_t {
    Ok,
    MalformedFrame,          // wire-token split failed, base64url decode failed
    BadTag,                  // HMAC mismatch
    WrongAudience,           // aud != "ost-relay/1"
    WrongTimer,              // tid != this timer
    StaleBoot,               // boot != current boot_id
    Expired,                 // exp_up_ms <= now_up_ms
    Unknown,                 // fp not in NVS table
    Revoked,                 // fp is present but revoked
    ScopeMissing,            // requested op is not in scope_mask
};

// Overflow policy: MintResult::TableFull is returned rather than
// silently evicting a live entry (RELAY_PROTOCOL.md §9 makes this load-bearing).
enum class RevokeResult : uint8_t { Ok, NotFound, PersistenceFailed };

enum class MintResult : uint8_t {
    Ok,
    TableFull,               // no room; owner must revoke or wait for expiry
    RateLimited,             // exceeded 20 mints/hour cap
    InvalidRequest,          // e.g. TTL beyond MaxTtlMs, empty scope, missing View
    NvsWriteFailed,
};

// Fixed table capacity, which is also the live share-link limit: a mint
// reuses revoked or expired slots and never evicts a live link. One link:
// one partner grant at a time (clear control, least RAM and flash wear);
// the owner renews it by revoking and minting a fresh one.
constexpr size_t kTokenTableCapacity = 1;
constexpr uint16_t kPartnerScopeMask = (1u << unsigned(Scope::_Count)) - 1;

// Enforced mint bounds.
constexpr uint32_t kDefaultTtlMs = 4  * 60 * 60 * 1000;   // 4h
constexpr uint32_t kMaxTtlMs     = 24 * 60 * 60 * 1000;   // 24h
constexpr uint32_t kMintRateWindowMs = 60 * 60 * 1000;    // 1h
constexpr uint32_t kMintRateMax     = 20;

class INvsStore;      // key/value blob store (EspNvs on the timer)
class IClock;         // exposes uptime_ms(); no wall clock
class IEntropy;       // crypto RNG source
class ITimerIdentity; // exposes timer_id string

class CapabilityToken {
public:
    // Injected dependencies. Ownership stays with the caller; the
    // CapabilityToken object outlives none of them.
    struct Deps {
        INvsStore*      nvs;
        IClock*         clock;
        IEntropy*       rng;
        ITimerIdentity* identity;
    };

    explicit CapabilityToken(const Deps& deps);

    // Bring-up. Generates capability_key on first boot if absent;
    // loads the token table; sweeps expired-by-boot entries. Returns
    // false if NVS is corrupt AND we must not proceed with relay
    // functionality (fail closed).
    bool begin();

    // Ten-second factory-reset hook.
    bool wipeAllTokens();
    bool wipeCapabilityKey();

    // Owner-triggered emergency revoke. Marks every non-expired,
    // non-revoked entry in the NVS token table as revoked in one
    // durable write. Returns the number of entries that transitioned
    // from live to revoked. RelayClient reads that number and emits
    // one token_revoke upstream frame per entry before closing the
    // WSS with code 4010 emergency_revoke.
    //
    // Does NOT touch capability_key — a subsequent owner-authorized
    // mint still works after re-enabling the relay.
    size_t revokeAll(char (*out_fp_hashes)[24], size_t out_capacity);

    // Called from the local web UI / BLE when the owner wants a new
    // share link. On MintResult::Ok, out_wire holds the wire token in
    // out_wire[0..out_wire_len). scope_mask MUST include Scope::View.
    MintResult mint(
        uint16_t scope_mask,
        uint32_t ttl_ms,
        const char* label,          // may be nullptr or ""
        char*     out_wire,
        size_t    out_wire_capacity,
        size_t*   out_wire_len
    );

    // Called from the local web UI / BLE. Revokes by fingerprint;
    // returns false if not present. Idempotent.
    bool revoke(const uint8_t fp[kTokenFingerprintBytes]);
    // verify() refuses the link from this call on. With persist=false the
    // flash write waits for saveChanges() (tokens are boot-scoped, so
    // RAM is authoritative until the table is reloaded in the same boot).
    RevokeResult revokeByHash(const char* fp_hash, bool persist = true);
    // Extend: the link keeps its URL; its expiry moves to
    // max(now, current expiry) + add_ms, capped at now + kMaxTtlMs. The
    // timer's record is authoritative (the expiry inside the wire token is
    // only the initial value). Works for an expired link of this boot
    // (reactivation), never for a revoked one. Saves like revokeByHash.
    RevokeResult extendByHash(const char* fp_hash, uint32_t add_ms, bool persist,
                              uint32_t* out_expires_up_ms);
    bool changesUnsaved() const { return changes_unsaved_; }
    bool saveChanges();   // true when nothing remains unsaved

    // Called from RelayClient for every downstream `command`. On Ok,
    // out_record points at the matched entry so the caller can read
    // scope_mask / label without a second lookup. Constant-time on
    // the tag comparison.
    VerifyResult verify(
        const char* wire,
        size_t      wire_len,
        Scope       required_scope,
        TokenRecord**  out_record   // may be nullptr; owned by internal table
    );

    TokenRecord* liveByHash(const char* fp_hash);

    bool lastPersistenceSucceeded() const { return persistence_ok_; }

    // Snapshot of the current table for the local UI ("your active
    // share links"). Returns the number of live entries copied into
    // out[0..cap).
    size_t listLive(TokenRecord* out, size_t cap) const;

    // For the upstream `token_mint` frame after mint() succeeds.
    // fp_hash = SHA-256(fp)[:16] base64url — 22 ASCII chars, NUL-term.
    [[nodiscard]] static bool fingerprintHash(
        const uint8_t fp[kTokenFingerprintBytes],
        char out[24]
    );

    // ECDSA P-256 signing key material. `begin()` generates the private
    // scalar on first boot (using deps_.rng) and persists it to
    // `relay.signkey` NVS. RelayClient reads it via `signKeyPrivate()` and
    // hands it to the crypto layer for signing downstream frames
    // (RELAY_PROTOCOL.md §7). The 33-byte compressed public key is embedded in every
    // capability-token payload (`vk` field, RELAY_PROTOCOL.md §4).
    const uint8_t* signKeyPrivate() const { return sign_private_; }
    const uint8_t* signKeyPublic() const  { return sign_public_; }

private:
    // In-RAM copy of the NVS token table. Persisted whole on write.
    TokenRecord table_[kTokenTableCapacity];
    uint8_t     capability_key_[kCapabilityKeyBytes];
    // P-256 private scalar + derived compressed public. The scalar is
    // persisted in NVS; the public key is cached for the mint payload.
    uint8_t     sign_private_[32];
    uint8_t     sign_public_[33];

    // Rate-limit ring: last kMintRateMax mint timestamps (uptime ms).
    // The `valid` bitmap decouples "slot in use" from "timestamp
    // happens to be zero", so mints at t=0 count correctly.
    uint32_t recent_mints_[kMintRateMax];
    uint32_t recent_mints_valid_;    // bit i = slot i is populated
    size_t   recent_mints_head_;

    Deps deps_;
    bool persistence_ok_;
    bool changes_unsaved_ = false;
    bool initialized_;
    mutable uint32_t last_clock_ms_;
    mutable bool clock_exhausted_;
    bool clockValid() const;

    // Helpers exposed only to unit tests via friend declarations in
    // the real implementation.
    bool loadFromNvs();
    bool persistToNvs();
    void sweepExpiredByBoot();
    bool rateLimitCheckAndRecord(uint32_t now_up_ms);
};

}  // namespace relay
}  // namespace opensmarttimer
