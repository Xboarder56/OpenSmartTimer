// capability_token_test.cpp — end-to-end mint + verify tests.
// Built and run by relay/tests/check.sh (no test framework needed).

#include <algorithm>
#include <vector>
#include "CapabilityToken.h"
#include "Crypto.h"

#include "Fakes.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace opensmarttimer::relay;
using namespace opensmarttimer::relay::test;
namespace opensmarttimer { namespace relay {
extern bool failHostSha256;
extern bool failHostHmac;
}}

namespace {

int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { \
        ++failures; \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

CapabilityToken makeInstance(FakeNvsStore& nvs, FakeClock& clock,
                             FakeEntropy& rng, FakeIdentity& id) {
    CapabilityToken::Deps d{&nvs, &clock, &rng, &id};
    return CapabilityToken(d);
}

// -- 1. Fresh device: begin() generates capability_key and persists it --
void test_capability_key_bootstrap() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(tok.begin());
    EXPECT(nvs.bytesStored() > 0);
}

// -- 2. Successful mint + verify round trip --
void test_mint_and_verify_ok() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(tok.begin());

    uint16_t scope = (uint16_t)(1 << (int)Scope::View)
                   | (uint16_t)(1 << (int)Scope::PauseResume);
    char wire[512]; size_t wire_len = 0;
    MintResult mr = tok.mint(scope, /*ttl_ms*/ 60 * 1000, "test",
                             wire, sizeof(wire), &wire_len);
    EXPECT(mr == MintResult::Ok);
    EXPECT(wire_len > 0);
    EXPECT(std::strncmp(wire, "ost-cap.v1.", 11) == 0);

    TokenRecord* rec = nullptr;
    VerifyResult vr = tok.verify(wire, wire_len, Scope::PauseResume, &rec);
    EXPECT(vr == VerifyResult::Ok);
    EXPECT(rec != nullptr);
}

// -- 3. Scope enforcement: verify against a scope the token lacks --
void test_verify_scope_missing() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View)
                   | (uint16_t)(1 << (int)Scope::PauseResume);
    char wire[512]; size_t wire_len = 0;
    tok.mint(scope, 60 * 1000, "", wire, sizeof(wire), &wire_len);
    VerifyResult vr = tok.verify(wire, wire_len, Scope::Release, nullptr);
    EXPECT(vr == VerifyResult::ScopeMissing);
}

// -- 4. Expiry against current-boot monotonic clock --
void test_verify_expiry() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    tok.mint(scope, /*ttl*/ 1000, "", wire, sizeof(wire), &wire_len);
    clock.advance(2000);
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Expired);
}

// -- 5. StaleBoot when boot_id changes --
void test_verify_stale_boot() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    tok.mint(scope, 60 * 1000, "", wire, sizeof(wire), &wire_len);
    id.setBootId("11223344");
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::StaleBoot);
}

// -- 6. WrongTimer when tid differs --
void test_verify_wrong_timer() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    tok.mint(scope, 60 * 1000, "", wire, sizeof(wire), &wire_len);
    id.setTimerId("timer_test_b");
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::WrongTimer);
}

// -- 7. BadTag on any single-bit flip --
void test_verify_bad_tag() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    tok.mint(scope, 60 * 1000, "", wire, sizeof(wire), &wire_len);
    // Flip the last character (tag).
    // Alter meaningful tag bits while keeping canonical base64url padding.
    wire[wire_len - 22] = wire[wire_len - 22] == 'A' ? 'B' : 'A';
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::BadTag);
}

// -- 8. Refuse-mint-on-full: never evict a live entry --
void test_mint_table_full_refuses() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    size_t ok_count = 0;
    // Fill up to kTokenTableCapacity (32) — capped further by
    // rate-limit (kMintRateMax = 20). Verify we hit either the
    // rate limiter or the table-full rule; never a silent eviction.
    for (size_t i = 0; i < kTokenTableCapacity + 4; ++i) {
        MintResult mr = tok.mint(scope, 60 * 60 * 1000, "", wire, sizeof(wire), &wire_len);
        if (mr == MintResult::Ok) ++ok_count;
        else EXPECT(mr == MintResult::TableFull || mr == MintResult::RateLimited);
    }
    EXPECT(ok_count <= kMintRateMax);
    EXPECT(ok_count <= kTokenTableCapacity);
}

// -- 9. Revoke marks entry as unavailable --
void test_revoke_blocks_verify() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    tok.mint(scope, 60 * 1000, "", wire, sizeof(wire), &wire_len);

    // Recover the fingerprint from the wire token so we can revoke.
    // Easiest path: enumerate the table.
    TokenRecord snapshot[kTokenTableCapacity];
    size_t n = tok.listLive(snapshot, kTokenTableCapacity);
    EXPECT(n == 1);
    EXPECT(tok.revoke(snapshot[0].fp));
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Revoked);
}

// -- 10. revokeAll returns the count and produces fp_hashes --
void test_revoke_all_flow() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    size_t minted = 0;
    for (const char* label : {"one", "two", "three"})
        minted += tok.mint(scope, 60 * 1000, label, wire, sizeof(wire), &wire_len) == MintResult::Ok;
    EXPECT(minted == std::min<size_t>(3, kTokenTableCapacity));

    char hashes[kTokenTableCapacity][24];
    size_t revoked = tok.revokeAll(hashes, kTokenTableCapacity);
    EXPECT(revoked == minted);
    // Each hash is 22 base64url chars.
    for (size_t i = 0; i < revoked; ++i) EXPECT(std::strlen(hashes[i]) == 22);
    // A second call yields zero.
    size_t revoked_again = tok.revokeAll(hashes, kTokenTableCapacity);
    EXPECT(revoked_again == 0);
}

// -- 11. Mint payload includes the vk (P-256 verify key) field (RELAY_PROTOCOL.md §4)
void test_mint_payload_includes_vk() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint16_t scope = (uint16_t)(1 << (int)Scope::View);
    char wire[512]; size_t wire_len = 0;
    tok.mint(scope, 60 * 1000, "", wire, sizeof(wire), &wire_len);

    // Split "ost-cap.v1.<payload>.<tag>" and base64url-decode payload.
    const char* p = strchr(wire + strlen("ost-cap.v1."), '.');
    EXPECT(p != nullptr);
    const size_t prefix = strlen("ost-cap.v1.");
    const size_t payload_len_b64 = (size_t)(p - (wire + prefix));

    uint8_t payload[512];
    int payload_len = b64urlDecode(wire + prefix, payload_len_b64,
                                   payload, sizeof(payload));
    EXPECT(payload_len > 0);
    // vk is 44 base64url chars: a 33-byte SEC1 compressed P-256 point
    // equal to the signing public key.
    payload[payload_len] = 0;
    const char* vk = strstr((const char*)payload, "\"vk\":\"");
    EXPECT(vk != nullptr);
    uint8_t decoded[33];
    EXPECT(vk && vk[6 + 44] == '"');
    EXPECT(vk && b64urlDecode(vk + 6, 44, decoded, sizeof(decoded)) == 33);
    EXPECT(decoded[0] == 0x02 || decoded[0] == 0x03);
    EXPECT(memcmp(decoded, tok.signKeyPublic(), 33) == 0);
    // Full-scope tokens stay within the 512-byte wire limit (RELAY_PROTOCOL.md §4).
    char full[1024]; size_t full_len = 0;
    char label[33]; memset(label, 'x', 32); label[32] = 0;
    char ended[kTokenTableCapacity][24];
    tok.revokeAll(ended, kTokenTableCapacity);  // free the single link slot
    EXPECT(tok.mint(kPartnerScopeMask, 24u * 3600u * 1000u, label, full, sizeof(full), &full_len) == MintResult::Ok);
    EXPECT(full_len <= 512);
}

// -- 11b. A stored scalar that is not a valid key fails closed instead of
// silently rotating the identity partners already trust.
void test_corrupt_sign_key_fails_closed() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    {
        CapabilityToken tok = makeInstance(nvs, clock, rng, id);
        EXPECT(tok.begin());
    }
    uint8_t zero[32] = {};
    EXPECT(nvs.writeBlob("relay.signkey", "p256", zero, sizeof(zero)));
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(!tok.begin());
}

// -- 12. Signing key is stable across begin() calls on the same NVS
void test_sign_key_persists_across_begin() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    uint8_t first[33];
    {
        CapabilityToken tok = makeInstance(nvs, clock, rng, id);
        tok.begin();
        memcpy(first, tok.signKeyPublic(), 33);
    }
    // Fresh instance, same NVS: public key must be identical.
    {
        CapabilityToken tok2 = makeInstance(nvs, clock, rng, id);
        tok2.begin();
        EXPECT(memcmp(first, tok2.signKeyPublic(), 33) == 0);
    }
}

// -- 13. Wipe rotates the signing key
void test_wipe_rotates_sign_key() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    tok.begin();
    uint8_t before[33];
    memcpy(before, tok.signKeyPublic(), 33);

    tok.wipeCapabilityKey();
    // A fresh begin() after wipe should generate a new key.
    rng.resetSeed(99);   // change the RNG state so the new seed differs
    tok.begin();
    EXPECT(memcmp(before, tok.signKeyPublic(), 33) != 0);
}

// -- 14. Persistence: reload finds the same tokens --
void test_persistence_round_trip() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    char wire[512]; size_t wire_len = 0;
    {
        CapabilityToken tok = makeInstance(nvs, clock, rng, id);
        tok.begin();
        uint16_t scope = (uint16_t)(1 << (int)Scope::View);
        tok.mint(scope, 60 * 1000, "persist", wire, sizeof(wire), &wire_len);
    }
    // A second instance sees the same table.
    {
        CapabilityToken tok2 = makeInstance(nvs, clock, rng, id);
        tok2.begin();
        EXPECT(tok2.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Ok);
    }
}

// -- Deferred revocation: refused at once, saved later --
void test_revoke_without_persist_blocks_then_saves() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(tok.begin()); char a[1024]; size_t a_len = 0;
    EXPECT(tok.mint(0x7f, 60000, "one", a, sizeof(a), &a_len) == MintResult::Ok);
    TokenRecord* rec = nullptr;
    EXPECT(tok.verify(a, a_len, Scope::View, &rec) == VerifyResult::Ok && rec);
    char hash[24]; EXPECT(CapabilityToken::fingerprintHash(rec->fp, hash));
    // Flash writes are fenced during a session; the revoke must not need one.
    nvs.writes_ok = false;
    EXPECT(tok.revokeByHash(hash, false) == RevokeResult::Ok);
    EXPECT(tok.changesUnsaved());
    EXPECT(tok.verify(a, a_len, Scope::Start, nullptr) == VerifyResult::Revoked);
    EXPECT(!tok.saveChanges() && tok.changesUnsaved());
    nvs.writes_ok = true;
    EXPECT(tok.saveChanges() && !tok.changesUnsaved());
    EXPECT(tok.saveChanges());  // nothing left: no write needed
    // A reload in the same boot (relay task restart) keeps the revocation.
    CapabilityToken reloaded = makeInstance(nvs, clock, rng, id);
    EXPECT(reloaded.begin());
    EXPECT(reloaded.verify(a, a_len, Scope::View, nullptr) == VerifyResult::Revoked);
}

// -- Live-link cap: revoked slots are reused, never a live link --
void test_single_link_renews_after_revoke() {
    static_assert(kTokenTableCapacity == 1, "one partner share link at a time");
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(tok.begin()); char wire[1024], old_wire[1024]; size_t wire_len = 0, old_len = 0;
    EXPECT(tok.mint(0x7f, 60000, "Alex", old_wire, sizeof(old_wire), &old_len) == MintResult::Ok);
    EXPECT(tok.mint(0x7f, 60000, "second", wire, sizeof(wire), &wire_len) == MintResult::TableFull);
    TokenRecord live[kTokenTableCapacity];
    EXPECT(tok.listLive(live, kTokenTableCapacity) == 1);
    char hash[24]; EXPECT(CapabilityToken::fingerprintHash(live[0].fp, hash));
    // Renew = revoke, then mint a fresh link (new URL) into the freed slot.
    EXPECT(tok.revokeByHash(hash) == RevokeResult::Ok);
    EXPECT(tok.mint(0x7f, 60000, "Alex", wire, sizeof(wire), &wire_len) == MintResult::Ok);
    EXPECT(tok.listLive(live, kTokenTableCapacity) == 1);
    EXPECT(tok.verify(old_wire, old_len, Scope::View, nullptr) != VerifyResult::Ok);
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Ok);
    // An expired link frees its slot too.
    clock.advance(60001);
    EXPECT(tok.mint(0x7f, 60000, "later", wire, sizeof(wire), &wire_len) == MintResult::Ok);
}

// -- A table written by a firmware with another layout (e.g. before an
//    OTA changed the capacity) is from an earlier boot: never interpreted,
//    never fatal, replaced by the next table write --
void test_other_layout_table_is_replaced_not_fatal() {
    for (size_t old_records : {size_t(32), size_t(1)}) {  // larger, smaller
        FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
        {
            CapabilityToken first = makeInstance(nvs, clock, rng, id);
            EXPECT(first.begin());
        }
        std::vector<uint8_t> old_layout(old_records * sizeof(TokenRecord), 0xA5);
        EXPECT(nvs.writeBlob("relay.tokens", "table", old_layout.data(), old_layout.size()));
        CapabilityToken tok = makeInstance(nvs, clock, rng, id);
        EXPECT(tok.begin());
        TokenRecord live[kTokenTableCapacity];
        EXPECT(tok.listLive(live, kTokenTableCapacity) == 0);
        char wire[1024]; size_t wire_len = 0;
        EXPECT(tok.mint(0x7f, 60000, "after ota", wire, sizeof(wire), &wire_len) == MintResult::Ok);
        std::vector<uint8_t> stored(64 * sizeof(TokenRecord)); size_t len = stored.size();
        EXPECT(nvs.readBlob("relay.tokens", "table", stored.data(), &len)
               && len == kTokenTableCapacity * sizeof(TokenRecord));
    }
}

// -- Extend keeps the URL: the record's expiry is authoritative --
void test_extend_keeps_url_and_reactivates() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(tok.begin()); char wire[1024]; size_t wire_len = 0;
    EXPECT(tok.mint(0x7f, 60000, "Alex", wire, sizeof(wire), &wire_len) == MintResult::Ok);
    TokenRecord live[kTokenTableCapacity];
    EXPECT(tok.listLive(live, kTokenTableCapacity) == 1);
    const uint32_t first = live[0].exp_up_ms;
    char hash[24]; EXPECT(CapabilityToken::fingerprintHash(live[0].fp, hash));
    // Mid-session (no flash lease): extended in RAM, saved later.
    nvs.writes_ok = false;
    uint32_t expires = 0;
    EXPECT(tok.extendByHash(hash, 30000, false, &expires) == RevokeResult::Ok);
    EXPECT(expires == first + 30000 && tok.changesUnsaved());
    clock.advance(70000);  // past the URL's original expiry, before the new one
    EXPECT(tok.verify(wire, wire_len, Scope::Start, nullptr) == VerifyResult::Ok);
    clock.advance(30000);
    EXPECT(tok.verify(wire, wire_len, Scope::Start, nullptr) == VerifyResult::Expired);
    // Reactivation within the same boot: the same URL works again.
    EXPECT(tok.extendByHash(hash, 60000, false, &expires) == RevokeResult::Ok);
    EXPECT(expires == clock.uptime_ms() + 60000);
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Ok);
    // Capped at 24 h from now; saved once flash writes are allowed.
    EXPECT(tok.extendByHash(hash, kMaxTtlMs, false, &expires) == RevokeResult::Ok);
    EXPECT(expires == clock.uptime_ms() + kMaxTtlMs);
    nvs.writes_ok = true;
    EXPECT(tok.saveChanges() && !tok.changesUnsaved());
    CapabilityToken reloaded = makeInstance(nvs, clock, rng, id);
    EXPECT(reloaded.begin());
    EXPECT(reloaded.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Ok);
    // A revoked link is never extended.
    EXPECT(tok.revokeByHash(hash) == RevokeResult::Ok);
    EXPECT(tok.extendByHash(hash, 60000, true, &expires) == RevokeResult::NotFound);
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Revoked);
}

void test_mint_expiry_and_persistence_failures() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(tok.begin()); char wire[1024]; size_t wire_len = 0;
    EXPECT(tok.mint(0x7f, 1000, "all", wire, sizeof(wire), &wire_len) == MintResult::Ok);
    TokenRecord live[32]; EXPECT(tok.listLive(live, 32) == 1);
    char hash[24]; EXPECT(CapabilityToken::fingerprintHash(live[0].fp, hash));
    nvs.writes_ok = false;
    EXPECT(tok.revokeByHash(hash) == RevokeResult::PersistenceFailed);
    EXPECT(tok.verify(wire, wire_len, Scope::View, nullptr) == VerifyResult::Revoked);
    nvs.writes_ok = true;
    EXPECT(tok.revokeByHash(hash) == RevokeResult::Ok);
    EXPECT(tok.revokeByHash(hash) == RevokeResult::Ok); // idempotent retry
    nvs.writes_ok = false;
    EXPECT(tok.mint(1, 1000, "lost", wire, sizeof(wire), &wire_len) == MintResult::NvsWriteFailed);
    EXPECT(wire_len == 0 && wire[0] == 0 && tok.listLive(live, 32) == 0);
    nvs.writes_ok = true;
    EXPECT(tok.mint(1, 1000, "expiry", wire, sizeof(wire), &wire_len) == MintResult::Ok);
    clock.advance(1000); EXPECT(tok.listLive(live, 32) == 0);
    clock.set(UINT32_MAX - 500);
    EXPECT(tok.mint(1, 1000, "wrap", wire, sizeof(wire), &wire_len) == MintResult::InvalidRequest);
    clock.advance(1000);
    EXPECT(tok.mint(1, 1000, "wrapped", wire, sizeof(wire), &wire_len) == MintResult::InvalidRequest);
    EXPECT(!tok.begin()); // stopping/starting relay cannot revive pre-wrap tokens
}

void test_provider_and_erase_failures() {
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity id;
    CapabilityToken tok = makeInstance(nvs, clock, rng, id);
    EXPECT(tok.begin());
    char wire[1024]; size_t len = 0;
    EXPECT(tok.mint(1, 60000, "ok", wire, sizeof(wire), &len) == MintResult::Ok);
    failHostHmac = true;
    EXPECT(tok.verify(wire, len, Scope::View, nullptr) == VerifyResult::BadTag);
    char zero_tag[32]; uint8_t zeros[kTokenTagBytes] = {};
    EXPECT(b64urlEncode(zeros, sizeof(zeros), zero_tag, sizeof(zero_tag)) > 0);
    std::string forged(wire, len);
    forged.replace(forged.rfind('.') + 1, std::string::npos, zero_tag);
    EXPECT(tok.verify(forged.data(), forged.size(), Scope::View, nullptr) == VerifyResult::BadTag);
    EXPECT(tok.mint(1, 60000, "fail", wire, sizeof(wire), &len) != MintResult::Ok);
    EXPECT(len == 0);
    failHostHmac = false;
    failHostSha256 = true;
    uint8_t fp[16] = {}; char hash[24] = "must be cleared";
    EXPECT(!CapabilityToken::fingerprintHash(fp, hash));
    EXPECT(hash[0] == 0);
    failHostSha256 = false;
    nvs.erases_ok = false;
    EXPECT(!tok.wipeAllTokens());
    EXPECT(!tok.lastPersistenceSucceeded());
    EXPECT(!tok.wipeCapabilityKey());
    EXPECT(!tok.lastPersistenceSucceeded());
    nvs.erases_ok = true;
    EXPECT(tok.wipeAllTokens());
    EXPECT(tok.wipeCapabilityKey());
}

}  // namespace

int main() {
    test_provider_and_erase_failures();
    test_mint_expiry_and_persistence_failures();
    test_capability_key_bootstrap();
    test_mint_and_verify_ok();
    test_verify_scope_missing();
    test_verify_expiry();
    test_verify_stale_boot();
    test_verify_wrong_timer();
    test_verify_bad_tag();
    test_mint_table_full_refuses();
    test_revoke_blocks_verify();
    test_revoke_all_flow();
    test_revoke_without_persist_blocks_then_saves();
    test_single_link_renews_after_revoke();
    test_extend_keeps_url_and_reactivates();
    test_other_layout_table_is_replaced_not_fatal();
    test_mint_payload_includes_vk();
    test_corrupt_sign_key_fails_closed();
    test_sign_key_persists_across_begin();
    test_wipe_rotates_sign_key();
    test_persistence_round_trip();

    if (failures) {
        std::fprintf(stderr, "\n%d test(s) FAILED\n", failures);
        return 1;
    }
    std::fprintf(stdout, "all capability_token tests passed\n");
    return 0;
}
