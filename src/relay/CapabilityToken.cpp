// CapabilityToken.cpp — timer-side HMAC-SHA256 capability minting
// and verification for ost-relay/1. See docs/RELAY_PROTOCOL.md §§ 4, 9
// for the wire format and NVS invariants.
//
// Depends on the small interfaces in include/relay/; the timer's bindings are
// the Esp* adapters (docs/RELAY_FIRMWARE.md).

#include "CapabilityToken.h"
#include "INvsStore.h"
#include "IClock.h"
#include "IEntropy.h"
#include "ITimerIdentity.h"
#include "Crypto.h"
#include "RelayJson.h"

#include <string.h>
#include <stdio.h>

namespace opensmarttimer { namespace relay {

namespace {

// ESP-IDF NVS namespace names are limited to 15 characters (nvs.h); the
// static_assert below keeps every `relay.<name>` namespace within it.
constexpr const char* kNsTokens    = "relay.tokens";
constexpr const char* kKeyTokens   = "table";
constexpr const char* kNsCapKey    = "relay.capkey";
constexpr const char* kKeyCapKey   = "key";
constexpr const char* kNsSignKey   = "relay.signkey";
constexpr const char* kKeySignSeed = "p256";  // Ed25519 "seed" never shipped

static_assert(sizeof("relay.tokens")  - 1 <= 15, "NVS namespace too long");
static_assert(sizeof("relay.capkey")  - 1 <= 15, "NVS namespace too long");
static_assert(sizeof("relay.signkey") - 1 <= 15, "NVS namespace too long");

// Bit index of every Scope in the token's scope_mask.
inline uint16_t scopeMaskBit(Scope s) { return (uint16_t)(1u << (uint8_t)s); }

// Convert `scopes` from the wire token's JSON array to a scope_mask.
// Returns false if the array contains an unknown scope name.
// Kept for a future mint-side "compose from string list" helper the
// UI layer will call; unused in this file today.
[[maybe_unused]]
bool parseScopeArray(const char* raw, size_t raw_len, uint16_t* out_mask) {
    // `raw` is the raw JSON slice including the surrounding `[…]`.
    if (raw_len < 2 || raw[0] != '[') return false;
    uint16_t mask = 0;
    size_t i = 1;
    while (i < raw_len) {
        while (i < raw_len && (raw[i] == ' ' || raw[i] == ',')) ++i;
        if (i >= raw_len || raw[i] == ']') break;
        if (raw[i] != '"') return false;
        ++i;
        size_t start = i;
        while (i < raw_len && raw[i] != '"') ++i;
        size_t n = i - start;
        if (i < raw_len) ++i;

        // Compare against known scope names. We accept only exact
        // matches; anything else fails verify — old tokens do not
        // silently gain access when new scope names are added.
        if      (n == 4  && memcmp(raw + start, "view", 4) == 0)         mask |= scopeMaskBit(Scope::View);
        else if (n == 12 && memcmp(raw + start, "pause_resume", 12) == 0) mask |= scopeMaskBit(Scope::PauseResume);
        else if (n == 6  && memcmp(raw + start, "adjust", 6) == 0)       mask |= scopeMaskBit(Scope::Adjust);
        else if (n == 7  && memcmp(raw + start, "release", 7) == 0)      mask |= scopeMaskBit(Scope::Release);
        else if (n == 5  && memcmp(raw + start, "start", 5) == 0)        mask |= scopeMaskBit(Scope::Start);
        else if (n == 4  && memcmp(raw + start, "lock", 4) == 0)         mask |= scopeMaskBit(Scope::Lock);
        else if (n == 8  && memcmp(raw + start, "identify", 8) == 0)     mask |= scopeMaskBit(Scope::Identify);
        else return false;
    }
    *out_mask = mask;
    return true;
}

// Encode scope_mask back to the JSON array form the wire token carries
// at mint time. Returns bytes written (excluding NUL); -1 on overflow.
int emitScopeArray(uint16_t mask, char* out, size_t cap) {
    if (cap < 2) return -1;
    size_t pos = 0;
    out[pos++] = '[';
    bool first = true;
    static const struct { Scope s; const char* name; } kOrdered[] = {
        {Scope::View,        "view"},
        {Scope::PauseResume, "pause_resume"},
        {Scope::Adjust,      "adjust"},
        {Scope::Release,     "release"},
        {Scope::Start,       "start"},
        {Scope::Lock,        "lock"},
        {Scope::Identify,    "identify"},
    };
    for (const auto& e : kOrdered) {
        if (!(mask & scopeMaskBit(e.s))) continue;
        if (!first) { if (pos + 1 >= cap) return -1; out[pos++] = ','; }
        first = false;
        size_t n = strlen(e.name);
        if (pos + n + 2 >= cap) return -1;
        out[pos++] = '"';
        memcpy(out + pos, e.name, n);
        pos += n;
        out[pos++] = '"';
    }
    if (pos + 1 >= cap) return -1;
    out[pos++] = ']';
    out[pos] = '\0';
    return (int)pos;
}

// Pack boot_id hex into a uint64_t for compact storage.
uint64_t encodeBootRef(const char* boot_id_hex) {
    uint64_t v = 0;
    for (size_t i = 0; boot_id_hex[i] && i < 16; ++i) {
        char c = boot_id_hex[i];
        uint64_t nyb;
        if (c >= '0' && c <= '9') nyb = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') nyb = (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') nyb = (uint64_t)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | nyb;
    }
    return v;
}

}  // namespace


CapabilityToken::CapabilityToken(const Deps& deps)
    : recent_mints_valid_(0), recent_mints_head_(0), deps_(deps), persistence_ok_(true), initialized_(false), last_clock_ms_(0), clock_exhausted_(false) {
    memset(table_, 0, sizeof(table_));
    memset(capability_key_, 0, sizeof(capability_key_));
    memset(sign_private_, 0, sizeof(sign_private_));
    memset(sign_public_, 0, sizeof(sign_public_));
    memset(recent_mints_, 0, sizeof(recent_mints_));
}

bool CapabilityToken::begin() {
    initialized_ = false;
    if (!deps_.nvs || !deps_.clock || !deps_.rng || !deps_.identity) return false;
    const char* boot = deps_.identity->current_boot_id();
    if (!boot || !*boot || strlen(boot) > 16) return false;
    for (const char* p = boot; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return false;
    // A restart of the relay task is not a new device boot. Never revive
    // current-boot tokens after the 32-bit wire clock has rolled over.
    if (!clockValid()) return false;
    // 1. Load capability_key. If absent, generate — but only if the
    //    entropy source is ready. Anything less is fail-closed.
    size_t key_len = kCapabilityKeyBytes;
    if (!deps_.nvs->readBlob(kNsCapKey, kKeyCapKey, capability_key_, &key_len)
        || key_len != kCapabilityKeyBytes) {
        if (!deps_.rng->ok()) return false;
        deps_.rng->fill(capability_key_, kCapabilityKeyBytes);
        if (!deps_.nvs->writeBlob(kNsCapKey, kKeyCapKey,
                                  capability_key_, kCapabilityKeyBytes)) {
            return false;
        }
    }

    // 2. Load or generate the P-256 signing scalar (RELAY_PROTOCOL.md §7). Same
    //    fail-closed rule as capability_key: if we can't produce or
    //    persist a valid signing key, refuse to bring up the relay.
    size_t priv_len = kSigningPrivateBytes;
    const bool loaded = deps_.nvs->readBlob(kNsSignKey, kKeySignSeed, sign_private_, &priv_len)
        && priv_len == kSigningPrivateBytes;
    if (loaded) {
        // A stored scalar that no longer derives a key is corruption, not a
        // reason to silently rotate the identity partners already trust.
        if (!ecdsaP256PublicFromPrivate(sign_private_, sign_public_)) return false;
    } else {
        if (!deps_.rng->ok()) return false;
        // A uniformly random 32-byte value is outside [1, n-1] with
        // probability below 2^-32; a few draws are ample.
        bool derived = false;
        for (int attempt = 0; attempt < 8 && !derived; ++attempt) {
            deps_.rng->fill(sign_private_, kSigningPrivateBytes);
            derived = ecdsaP256PublicFromPrivate(sign_private_, sign_public_);
        }
        if (!derived || !deps_.nvs->writeBlob(kNsSignKey, kKeySignSeed,
                                              sign_private_, kSigningPrivateBytes)) {
            memset(sign_private_, 0, sizeof(sign_private_));
            return false;
        }
    }

    // 3. Load the token table. Missing is fine (fresh device).
    if (!loadFromNvs()) return false;

    // 4. Sweep entries whose boot_ref differs from the current boot.
    //    Those are implicitly expired anyway; zeroing them keeps the
    //    table free for new mints without any authorization risk.
    sweepExpiredByBoot();
    initialized_ = persistence_ok_;
    return initialized_;
}

bool CapabilityToken::wipeAllTokens() {
    memset(table_, 0, sizeof(table_));
    persistence_ok_ = deps_.nvs->eraseKey(kNsTokens, kKeyTokens);
    return persistence_ok_;
}

bool CapabilityToken::wipeCapabilityKey() {
    initialized_ = false;
    memset(capability_key_, 0, sizeof(capability_key_));
    const bool cap_ok = deps_.nvs->eraseNamespace(kNsCapKey);
    // Signing key rotates on the same factory-reset gesture; a new
    // P-256 keypair means every partner sees vk change → clients
    // MUST re-accept the new share link.
    memset(sign_private_, 0, sizeof(sign_private_));
    memset(sign_public_, 0, sizeof(sign_public_));
    persistence_ok_ = deps_.nvs->eraseNamespace(kNsSignKey) && cap_ok;
    return persistence_ok_;
}

size_t CapabilityToken::revokeAll(char (*out_fp_hashes)[24], size_t out_capacity) {
    size_t n = 0;
    const uint32_t now = deps_.clock->uptime_ms();
    for (size_t i = 0; i < kTokenTableCapacity; ++i) {
        TokenRecord& r = table_[i];
        // Skip empty slots (all zero) and already-revoked/expired.
        bool live = false;
        for (size_t j = 0; j < kTokenFingerprintBytes; ++j) {
            if (r.fp[j]) { live = true; break; }
        }
        if (!live || r.revoked || r.exp_up_ms <= now) continue;

        // Emit fp_hash for the caller to broadcast upstream.
        if (n < out_capacity) {
            // Revoke locally even when a crypto provider cannot calculate
            // the upstream fingerprint; never fabricate a zero digest.
            // An empty hash is skipped by announceRevokedToken().
            if (!fingerprintHash(r.fp, out_fp_hashes[n])) out_fp_hashes[n][0] = '\0';
        }
        r.revoked = true;
        ++n;
    }
    persistence_ok_ = persistToNvs();
    return n;
}

MintResult CapabilityToken::mint(
    uint16_t scope_mask, uint32_t ttl_ms, const char* label,
    char* out_wire, size_t out_wire_capacity, size_t* out_wire_len
) {
    // Validate request before changing the table or computing deadlines.
    if (out_wire_len) *out_wire_len = 0;
    if (!initialized_ || !out_wire || !deps_.rng->ok() || !clockValid()) return MintResult::InvalidRequest;
    if (scope_mask & ~((1u << unsigned(Scope::_Count)) - 1)) return MintResult::InvalidRequest;
    if (!(scope_mask & scopeMaskBit(Scope::View))) return MintResult::InvalidRequest;
    if (ttl_ms == 0 || ttl_ms > kMaxTtlMs) return MintResult::InvalidRequest;

    const uint32_t now = deps_.clock->uptime_ms();
    // Uptime is uint32 on the wire; refuse a link spanning its wrap boundary.
    if (ttl_ms > UINT32_MAX - now) return MintResult::InvalidRequest;
    if (!rateLimitCheckAndRecord(now)) return MintResult::RateLimited;

    // Find a free slot. Never evict a live entry — refuse mint instead.
    ssize_t slot = -1;
    for (size_t i = 0; i < kTokenTableCapacity; ++i) {
        const TokenRecord& r = table_[i];
        bool live = false;
        for (size_t j = 0; j < kTokenFingerprintBytes; ++j) {
            if (r.fp[j]) { live = true; break; }
        }
        if (!live || r.revoked || r.exp_up_ms <= now) { slot = (ssize_t)i; break; }
    }
    if (slot < 0) return MintResult::TableFull;

    TokenRecord rec{};
    deps_.rng->fill(rec.fp, kTokenFingerprintBytes);
    rec.scope_mask = scope_mask;
    rec.exp_up_ms = now + ttl_ms;
    rec.boot_ref = encodeBootRef(deps_.identity->current_boot_id());
    rec.revoked = false;
    if (label) {
        strncpy(rec.label, label, sizeof(rec.label) - 1);
        rec.label[sizeof(rec.label) - 1] = '\0';
    }

    // Build the payload JSON. Order matters for deterministic HMAC
    // (RELAY_PROTOCOL.md §4 lists the field order); readers accept any order,
    // but we sign the exact bytes we serialize so the encoder MUST be
    // stable.
    char payload_buf[512];
    JsonWriter jw(payload_buf, sizeof(payload_buf));
    jw.beginObject();
    jw.key("aud");        jw.valueString("ost-relay/1");
    jw.key("tid");        jw.valueString(deps_.identity->timer_id());
    // fp is 16 raw bytes → 22 base64url chars.
    char fp_b64[24];
    (void)b64urlEncode(rec.fp, kTokenFingerprintBytes, fp_b64, sizeof(fp_b64));
    jw.key("fp");         jw.valueString(fp_b64);
    char scope_json[96];
    if (emitScopeArray(scope_mask, scope_json, sizeof(scope_json)) < 0)
        return MintResult::InvalidRequest;
    jw.key("sc");         jw.valueRaw(scope_json, strlen(scope_json));
    jw.key("boot");       jw.valueString(deps_.identity->current_boot_id());
    jw.key("exp_up_ms");  jw.valueUint(rec.exp_up_ms);
    jw.key("mint_up_ms"); jw.valueUint(now);
    // vk (RELAY_PROTOCOL.md §4): compressed P-256 verify key so the SPA can check
    // downstream frame signatures without any relay involvement.
    char vk_b64[48];
    if (b64urlEncode(sign_public_, kSigningPublicBytes, vk_b64, sizeof(vk_b64)) != 44)
        return MintResult::InvalidRequest;
    jw.key("vk");         jw.valueString(vk_b64);
    jw.endObject();
    size_t payload_len = jw.finalize();
    if (payload_len == 0) return MintResult::InvalidRequest;
    payload_len -= 1;  // strip the trailing NUL from the length

    // Sign.
    uint8_t tag[32];
    if (!hmacSha256(capability_key_, kCapabilityKeyBytes,
                    (const uint8_t*)payload_buf, payload_len, tag))
        return MintResult::InvalidRequest;

    // Assemble the wire form: "ost-cap.v1.<b64u(payload)>.<b64u(tag)>"
    char payload_b64[768];
    int payload_b64_len = b64urlEncode(
        (const uint8_t*)payload_buf, payload_len,
        payload_b64, sizeof(payload_b64));
    if (payload_b64_len < 0) return MintResult::InvalidRequest;

    char tag_b64[32];
    int tag_b64_len = b64urlEncode(tag, kTokenTagBytes, tag_b64, sizeof(tag_b64));
    if (tag_b64_len < 0) return MintResult::InvalidRequest;

    static const char* kPrefix = "ost-cap.v1.";
    size_t need = strlen(kPrefix) + (size_t)payload_b64_len + 1
                + (size_t)tag_b64_len + 1;  // +1 for trailing NUL
    if (need > out_wire_capacity) return MintResult::InvalidRequest;
    size_t pos = 0;
    memcpy(out_wire + pos, kPrefix, strlen(kPrefix)); pos += strlen(kPrefix);
    memcpy(out_wire + pos, payload_b64, payload_b64_len); pos += payload_b64_len;
    out_wire[pos++] = '.';
    memcpy(out_wire + pos, tag_b64, tag_b64_len); pos += tag_b64_len;
    out_wire[pos] = '\0';
    if (out_wire_len) *out_wire_len = pos;

    TokenRecord previous = table_[slot];
    table_[slot] = rec;
    persistence_ok_ = persistToNvs();
    if (!persistence_ok_) {
        table_[slot] = previous;
        if (out_wire_len) *out_wire_len = 0;
        out_wire[0] = 0;
        return MintResult::NvsWriteFailed;
    }
    return MintResult::Ok;
}

bool CapabilityToken::revoke(const uint8_t fp[kTokenFingerprintBytes]) {
    for (size_t i = 0; i < kTokenTableCapacity; ++i) {
        TokenRecord& r = table_[i];
        if (memcmp(r.fp, fp, kTokenFingerprintBytes) == 0) {
            r.revoked = true;
            changes_unsaved_ = true;
            persistence_ok_ = persistToNvs();  // clears changes_unsaved_ on success
            return persistence_ok_;
        }
    }
    return false;
}

RevokeResult CapabilityToken::revokeByHash(const char* fp_hash, bool persist) {
    if (!fp_hash || strlen(fp_hash) != 22) return RevokeResult::NotFound;
    for (auto& r : table_) {
        bool populated = false;
        for (uint8_t byte : r.fp) populated |= byte != 0;
        if (!populated) continue;
        char hash[24];
        if (!fingerprintHash(r.fp, hash)) return RevokeResult::PersistenceFailed;
        if (strcmp(fp_hash, hash) != 0) continue;
        if (persist) return revoke(r.fp) ? RevokeResult::Ok : RevokeResult::PersistenceFailed;
        r.revoked = true;
        changes_unsaved_ = true;
        return RevokeResult::Ok;
    }
    return RevokeResult::NotFound;
}

RevokeResult CapabilityToken::extendByHash(const char* fp_hash, uint32_t add_ms, bool persist,
                                          uint32_t* out_expires_up_ms) {
    if (!fp_hash || strlen(fp_hash) != 22 || !add_ms || add_ms > kMaxTtlMs || !clockValid())
        return RevokeResult::NotFound;
    const uint32_t now = deps_.clock->uptime_ms();
    const uint64_t current_boot = encodeBootRef(deps_.identity->current_boot_id());
    for (auto& r : table_) {
        bool populated = false;
        for (uint8_t byte : r.fp) populated |= byte != 0;
        if (!populated || r.revoked || r.boot_ref != current_boot) continue;
        char hash[24];
        if (!fingerprintHash(r.fp, hash)) return RevokeResult::PersistenceFailed;
        if (strcmp(fp_hash, hash) != 0) continue;
        const uint64_t base = r.exp_up_ms > now ? r.exp_up_ms : now;
        const uint64_t cap = uint64_t(now) + kMaxTtlMs;
        const uint64_t extended = base + add_ms < cap ? base + add_ms : cap;
        if (extended > UINT32_MAX) return RevokeResult::NotFound;
        r.exp_up_ms = uint32_t(extended);
        if (out_expires_up_ms) *out_expires_up_ms = r.exp_up_ms;
        changes_unsaved_ = true;
        if (!persist) return RevokeResult::Ok;
        persistence_ok_ = persistToNvs();
        return persistence_ok_ ? RevokeResult::Ok : RevokeResult::PersistenceFailed;
    }
    return RevokeResult::NotFound;
}

VerifyResult CapabilityToken::verify(
    const char* wire, size_t wire_len, Scope required_scope,
    TokenRecord** out_record
) {
    if (out_record) *out_record = nullptr;
    if (!initialized_ || !wire || !clockValid() || unsigned(required_scope) >= unsigned(Scope::_Count))
        return VerifyResult::MalformedFrame;

    // Split "ost-cap.v1.<payload>.<tag>" — three '.' separators.
    static const char* kPrefix = "ost-cap.v1.";
    const size_t plen = strlen(kPrefix);
    if (wire_len < plen || memcmp(wire, kPrefix, plen) != 0)
        return VerifyResult::MalformedFrame;

    const char* payload_b64 = wire + plen;
    const char* dot = nullptr;
    for (size_t i = plen; i < wire_len; ++i) {
        if (wire[i] == '.') { dot = wire + i; break; }
    }
    if (!dot) return VerifyResult::MalformedFrame;
    size_t payload_b64_len = (size_t)(dot - payload_b64);
    const char* tag_b64 = dot + 1;
    size_t tag_b64_len = wire_len - (size_t)(tag_b64 - wire);
    if (payload_b64_len == 0 || tag_b64_len == 0)
        return VerifyResult::MalformedFrame;

    // Decode payload and tag.
    uint8_t payload[512];
    int payload_len = b64urlDecode(payload_b64, payload_b64_len,
                                   payload, sizeof(payload));
    if (payload_len < 0) return VerifyResult::MalformedFrame;
    uint8_t tag[32];
    int tag_len = b64urlDecode(tag_b64, tag_b64_len, tag, sizeof(tag));
    if (tag_len != (int)kTokenTagBytes) return VerifyResult::MalformedFrame;

    // Recompute HMAC and constant-time compare.
    uint8_t expected[32];
    if (!hmacSha256(capability_key_, kCapabilityKeyBytes,
                    payload, (size_t)payload_len, expected)
        || constantTimeCompare(expected, tag, kTokenTagBytes) != 0)
        return VerifyResult::BadTag;

    // Cross-check payload fields.
    char scratch[64];
    int n = jsonCopyString((const char*)payload, (size_t)payload_len,
                           "aud", scratch, sizeof(scratch));
    if (n < 0 || strcmp(scratch, "ost-relay/1") != 0) return VerifyResult::WrongAudience;

    n = jsonCopyString((const char*)payload, (size_t)payload_len,
                       "tid", scratch, sizeof(scratch));
    if (n < 0 || strcmp(scratch, deps_.identity->timer_id()) != 0)
        return VerifyResult::WrongTimer;

    n = jsonCopyString((const char*)payload, (size_t)payload_len,
                       "boot", scratch, sizeof(scratch));
    if (n < 0 || strcmp(scratch, deps_.identity->current_boot_id()) != 0)
        return VerifyResult::StaleBoot;

    // The payload's expiry is the link's initial value; the table record is
    // authoritative (an owner may extend a link without changing its URL).
    int64_t exp_up_ms = 0;
    if (!jsonGetInt((const char*)payload, (size_t)payload_len,
                    "exp_up_ms", &exp_up_ms))
        return VerifyResult::MalformedFrame;
    if (exp_up_ms < 0 || exp_up_ms > UINT32_MAX) return VerifyResult::MalformedFrame;

    // Look up fp in the table. The payload's fp is base64url of the
    // raw 16 bytes; we need those raw bytes for comparison.
    n = jsonCopyString((const char*)payload, (size_t)payload_len,
                       "fp", scratch, sizeof(scratch));
    if (n < 0) return VerifyResult::MalformedFrame;
    uint8_t fp_raw[kTokenFingerprintBytes];
    int fp_len = b64urlDecode(scratch, (size_t)n, fp_raw, sizeof(fp_raw));
    if (fp_len != (int)kTokenFingerprintBytes) return VerifyResult::MalformedFrame;

    for (size_t i = 0; i < kTokenTableCapacity; ++i) {
        TokenRecord& r = table_[i];
        if (memcmp(r.fp, fp_raw, kTokenFingerprintBytes) != 0) continue;
        if (r.revoked) return VerifyResult::Revoked;
        if (r.boot_ref != encodeBootRef(deps_.identity->current_boot_id())) return VerifyResult::StaleBoot;
        if (r.exp_up_ms < uint32_t(exp_up_ms)) return VerifyResult::MalformedFrame;  // never shortened
        if (r.exp_up_ms <= deps_.clock->uptime_ms()) return VerifyResult::Expired;
        if (!(r.scope_mask & scopeMaskBit(required_scope)))
            return VerifyResult::ScopeMissing;
        if (out_record) *out_record = &r;
        return VerifyResult::Ok;
    }
    return VerifyResult::Unknown;
}

TokenRecord* CapabilityToken::liveByHash(const char* fp_hash) {
    if (!initialized_ || !fp_hash || strlen(fp_hash)!=22 || !clockValid()) return nullptr;
    const uint32_t now=deps_.clock->uptime_ms();
    const uint64_t boot=encodeBootRef(deps_.identity->current_boot_id());
    for (auto& r:table_) {
        char hash[24];
        if (!r.revoked && r.exp_up_ms>now && r.boot_ref==boot
            && fingerprintHash(r.fp,hash) && strcmp(hash,fp_hash)==0) return &r;
    }
    return nullptr;
}

size_t CapabilityToken::listLive(TokenRecord* out, size_t cap) const {
    if (!initialized_ || !clockValid()) return 0;
    const uint32_t now = deps_.clock->uptime_ms();
    const uint64_t boot = encodeBootRef(deps_.identity->current_boot_id());
    size_t n = 0;
    for (size_t i = 0; i < kTokenTableCapacity && n < cap; ++i) {
        const TokenRecord& r = table_[i];
        bool live = false;
        for (size_t j = 0; j < kTokenFingerprintBytes; ++j) {
            if (r.fp[j]) { live = true; break; }
        }
        if (live && !r.revoked && r.exp_up_ms > now && r.boot_ref == boot) out[n++] = r;
    }
    return n;
}

bool CapabilityToken::fingerprintHash(
    const uint8_t fp[kTokenFingerprintBytes], char out[24]) {
    uint8_t hash[32];
    out[0] = 0;
    return sha256(fp, kTokenFingerprintBytes, hash)
        && b64urlEncode(hash, 16, out, 24) == 22;
}

// ---------------------------------------------------------------------------
// Persistence + rate-limit internals
// ---------------------------------------------------------------------------

bool CapabilityToken::clockValid() const {
    uint32_t now = deps_.clock->uptime_ms();
    if (now < last_clock_ms_) clock_exhausted_ = true;
    last_clock_ms_ = now;
    return !clock_exhausted_;
}

bool CapabilityToken::loadFromNvs() {
    size_t len = sizeof(table_);
    if (!deps_.nvs->readBlob(kNsTokens, kKeyTokens,
                             (uint8_t*)table_, &len)) {
        memset(table_, 0, sizeof(table_));
        return true;  // first boot; not an error
    }
    if (len != sizeof(table_)) {
        // Written by a firmware with another layout, hence in an earlier
        // boot: every entry is already void (tokens are boot-scoped). Never
        // interpret it; replace it so the relay keeps working after an OTA.
        memset(table_, 0, sizeof(table_));
        persistence_ok_ = persistToNvs();
        return persistence_ok_;
    }
    return true;
}

bool CapabilityToken::persistToNvs() {
    const bool ok = deps_.nvs->writeBlob(kNsTokens, kKeyTokens,
                                         (const uint8_t*)table_, sizeof(table_));
    if (ok) changes_unsaved_ = false;  // the whole table, revoked flags included
    return ok;
}

bool CapabilityToken::saveChanges() {
    if (!changes_unsaved_) return true;
    persistence_ok_ = persistToNvs();
    return persistence_ok_;
}

void CapabilityToken::sweepExpiredByBoot() {
    const uint64_t cur = encodeBootRef(deps_.identity->current_boot_id());
    bool dirty = false;
    for (size_t i = 0; i < kTokenTableCapacity; ++i) {
        TokenRecord& r = table_[i];
        bool live = false;
        for (size_t j = 0; j < kTokenFingerprintBytes; ++j) {
            if (r.fp[j]) { live = true; break; }
        }
        if (!live) continue;
        if (r.boot_ref != cur) {
            memset(&r, 0, sizeof(r));
            dirty = true;
        }
    }
    if (dirty) persistence_ok_ = persistToNvs();
}

bool CapabilityToken::rateLimitCheckAndRecord(uint32_t now_up_ms) {
    // Count valid slots whose stored timestamp is within the window.
    // `age = now - stored` under uint32 wraparound: while the timer
    // uptime stays monotonic (all real slots were written with an
    // uptime ≤ now), an age >= kMintRateWindowMs falls outside the
    // window unambiguously.
    size_t count = 0;
    for (size_t i = 0; i < kMintRateMax; ++i) {
        if (!(recent_mints_valid_ & (1u << i))) continue;
        uint32_t age = now_up_ms - recent_mints_[i];
        if (age < kMintRateWindowMs) ++count;
    }
    if (count >= kMintRateMax) return false;
    recent_mints_[recent_mints_head_] = now_up_ms;
    recent_mints_valid_ |= (1u << recent_mints_head_);
    recent_mints_head_ = (recent_mints_head_ + 1) % kMintRateMax;
    return true;
}

}  // namespace relay
}  // namespace opensmarttimer
