// RFC 6979 A.2.5 (P-256, SHA-256) known answers, low-S normalization,
// tampering, invalid scalars and provider failure.
#include "Crypto.h"
#include "CapabilityToken.h"
#include "Fakes.h"
#include <openssl/evp.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace opensmarttimer::relay;
using namespace opensmarttimer::relay::test;
std::vector<uint8_t> hex(const char* text) {
    std::vector<uint8_t> bytes;
    for (size_t i=0; i<std::strlen(text); i+=2) {
        unsigned value=0; assert(std::sscanf(text+i, "%2x", &value)==1); bytes.push_back(uint8_t(value));
    }
    return bytes;
}
const char* kPrivate = "c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721";
const char* kPublic = "0360fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6";
void vector(const char* message_text, const char* r_hex, const char* low_s_hex) {
    auto priv=hex(kPrivate), expected_key=hex(kPublic), r=hex(r_hex), s=hex(low_s_hex);
    std::vector<uint8_t> message(message_text, message_text+std::strlen(message_text));
    uint8_t public_key[33], signature[64];
    assert(ecdsaP256PublicFromPrivate(priv.data(), public_key));
    assert(std::memcmp(public_key, expected_key.data(), 33)==0);
    assert(ecdsaP256Sign(priv.data(), message.data(), message.size(), signature));
    assert(std::memcmp(signature, r.data(), 32)==0);
    assert(std::memcmp(signature+32, s.data(), 32)==0);
    uint8_t again[64];
    assert(ecdsaP256Sign(priv.data(), message.data(), message.size(), again));
    assert(std::memcmp(signature, again, 64)==0);  // deterministic
    assert(ecdsaP256Verify(public_key, message.data(), message.size(), signature));
    signature[12] ^= 0x80;
    assert(!ecdsaP256Verify(public_key, message.data(), message.size(), signature));
    signature[12] ^= 0x80;
    public_key[5] ^= 1;
    assert(!ecdsaP256Verify(public_key, message.data(), message.size(), signature));
    public_key[5] ^= 1;
    message.push_back(0);
    assert(!ecdsaP256Verify(public_key, message.data(), message.size(), signature));
}
int main() {
    // "sample": RFC s is high (f7cb...); providers must emit n - s.
    vector("sample", "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716",
           "0834e36ad29a83bf2bc9385e491d6099c8fdf9d1ed67aa7ea5f51f93782857a9");
    vector("test", "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367",
           "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083");
    // The high-S twin of a valid signature is rejected (no malleability).
    {
        auto priv=hex(kPrivate), pub=hex(kPublic);
        auto high=hex("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                      "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8");
        const uint8_t sample[]={'s','a','m','p','l','e'};
        assert(!ecdsaP256Verify(pub.data(), sample, sizeof(sample), high.data()));
    }
    // Scalars outside [1, n-1] never become keys.
    uint8_t zero[32]={}, order[32], pub[33], sig[64];
    auto n=hex("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551");
    std::memcpy(order,n.data(),32);
    std::memset(pub,0xff,33);
    assert(!ecdsaP256PublicFromPrivate(zero,pub)); for (auto c:pub) assert(c==0);
    assert(!ecdsaP256PublicFromPrivate(order,pub));
    std::memset(sig,0xff,64);
    assert(!ecdsaP256Sign(order,zero,32,sig)); for (auto c:sig) assert(c==0);
    // An unavailable provider must fail, never leave a pretend key/signature.
    auto priv=hex(kPrivate);
    assert(EVP_set_default_properties(nullptr, "provider=missing") == 1);
    std::memset(pub,0xff,33);std::memset(sig,0xff,64);
    assert(!ecdsaP256PublicFromPrivate(priv.data(),pub)); assert(!ecdsaP256Sign(priv.data(),zero,32,sig));
    for (auto c:pub) assert(c==0);
    for (auto c:sig) assert(c==0);
    FakeNvsStore nvs; FakeClock clock; FakeEntropy rng; FakeIdentity identity;
    CapabilityToken tokens({&nvs,&clock,&rng,&identity}); assert(!tokens.begin());
    char wire[1024];size_t size;
    assert(tokens.mint(kPartnerScopeMask,60000,"",wire,sizeof(wire),&size)!=MintResult::Ok);
    assert(EVP_set_default_properties(nullptr, "") == 1);
    assert(tokens.begin());
    std::puts("ECDSA P-256 RFC 6979 vectors, low-S, invalid scalars and provider failures passed");
}
