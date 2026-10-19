// relay_json_test.cpp — bounded JSON reader/writer regressions.
// Built and run by relay/tests/check.sh.

#include "RelayJson.h"

#include <cstdio>
#include <cstring>

using namespace opensmarttimer::relay;

namespace {

int failures = 0;
#define EXPECT(cond) do { \
    if (!(cond)) { ++failures; \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } \
} while (0)
#define EXPECT_STREQ(a, b) do { \
    if (std::strcmp((a), (b)) != 0) { ++failures; \
        std::fprintf(stderr, "FAIL %s:%d expected '%s', got '%s'\n", \
                     __FILE__, __LINE__, (b), (a)); } \
} while (0)


// ---------------------------------------------------------------------------
// Regression: array-of-objects loses its separating comma (bug reproduced by
// the dev). Same class covers array-of-arrays and object-with-nested-
// object as a value.
// ---------------------------------------------------------------------------

void test_array_of_objects_has_commas() {
    char buf[128];
    JsonWriter jw(buf, sizeof(buf));
    jw.beginArray();
    for (int i = 0; i < 2; ++i) {
        jw.beginObject();
        jw.key("x"); jw.valueUint((uint64_t)i);
        jw.endObject();
    }
    jw.endArray();
    (void)jw.finalize();
    EXPECT_STREQ(buf, "[{\"x\":0},{\"x\":1}]");
}

void test_array_of_arrays_has_commas() {
    char buf[128];
    JsonWriter jw(buf, sizeof(buf));
    jw.beginArray();
    for (int i = 0; i < 3; ++i) {
        jw.beginArray();
        jw.valueUint(1); jw.valueUint(2);
        jw.endArray();
    }
    jw.endArray();
    (void)jw.finalize();
    EXPECT_STREQ(buf, "[[1,2],[1,2],[1,2]]");
}

void test_object_with_nested_object_value() {
    char buf[128];
    JsonWriter jw(buf, sizeof(buf));
    jw.beginObject();
    jw.key("a"); jw.valueUint(1);
    jw.key("nested");
    jw.beginObject();
    jw.key("b"); jw.valueUint(2);
    jw.endObject();
    jw.key("c"); jw.valueUint(3);   // comma after nested object matters
    jw.endObject();
    (void)jw.finalize();
    EXPECT_STREQ(buf, "{\"a\":1,\"nested\":{\"b\":2},\"c\":3}");
}


// ---------------------------------------------------------------------------
// Regression: jsonGetInt accepted "123oops" as 123 (bug reproduced by the dev).
// ---------------------------------------------------------------------------

void test_int_rejects_trailing_garbage() {
    const char* body = "{\"n\":123oops}";
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_int_accepts_pure_negative() {
    const char* body = "{\"n\":-42}";
    int64_t out = 0;
    EXPECT(jsonGetInt(body, std::strlen(body), "n", &out));
    EXPECT(out == -42);
}

void test_int_rejects_float() {
    const char* body = "{\"n\":1.5}";
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_int_rejects_exponent() {
    const char* body = "{\"n\":1e2}";
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_int_rejects_plus_sign() {
    const char* body = "{\"n\":+1}";
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_int_rejects_lonely_minus() {
    const char* body = "{\"n\":-}";
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}


// ---------------------------------------------------------------------------
// Regression: finalize() overflow behavior. When the buffer is too small,
// finalize() must return 0 AND overflowed() must be true. Callers that
// used `finalize() - 1` unchecked previously produced SIZE_MAX-length
// garbage responses; the fail-closed helper depends on this contract.
// ---------------------------------------------------------------------------

void test_finalize_overflow_returns_zero_and_flags() {
    char buf[8];   // too small for the payload below
    JsonWriter jw(buf, sizeof(buf));
    jw.beginObject();
    jw.key("longer_key_than_fits"); jw.valueUint(1234567890);
    jw.endObject();
    const size_t n = jw.finalize();
    EXPECT(jw.overflowed());
    EXPECT(n == 0);
}

// ---------------------------------------------------------------------------
// Parser hardening
// ---------------------------------------------------------------------------

void test_int_rejects_int64_overflow() {
    const char* body = "{\"n\":9223372036854775808}";   // INT64_MAX + 1
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_int_rejects_leading_zero() {
    const char* body = "{\"n\":01}";
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_int_accepts_zero_alone() {
    const char* body = "{\"n\":0}";
    int64_t out = 99;
    EXPECT(jsonGetInt(body, std::strlen(body), "n", &out));
    EXPECT(out == 0);
}

void test_findkey_rejects_duplicate_keys() {
    const char* body = "{\"n\":1,\"n\":2}";
    int64_t out = 0;
    // Duplicate keys → fail-closed, not "last wins".
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_findkey_rejects_truncated_object() {
    const char* body = "{\"n\":1";   // no closing brace
    int64_t out = 0;
    EXPECT(!jsonGetInt(body, std::strlen(body), "n", &out));
}

void test_bool_rejects_lookalike() {
    // "trueX" satisfied the earlier flag-only check because
    // is_bool_true was set from first-char + length. The real value
    // is not a bool, so jsonGetBool must refuse.
    // We synthesize this by declaring a raw literal that would trip
    // the flag but not equal "true".
    const char* body = "{\"x\":truex}";
    bool out = false;
    EXPECT(!jsonGetBool(body, std::strlen(body), "x", &out));
}

void test_finalize_nonoverflow_returns_len_plus_nul() {
    char buf[64];
    JsonWriter jw(buf, sizeof(buf));
    jw.beginObject();
    jw.key("k"); jw.valueUint(1);
    jw.endObject();
    const size_t n = jw.finalize();
    EXPECT(!jw.overflowed());
    // Value returned includes the NUL, so string length is n-1.
    EXPECT(n == std::strlen(buf) + 1);
}

void test_strict_complete_json_and_unescaping() {
    const char* bad[] = {
        "{\"n\":1,}", "{\"n\":1}garbage", "{\"n\":1,\"x\":[1,]}",
        "{\"n\":1,\"x\":{\"a\":true,\"a\":false}}", "{\"n\":1,\"x\":tru}",
        "{\"n\":1,\"x\":\"\\q\"}", "{\"n\":1,\"x\":\"\\ud800\"}",
        "{\"n\":1,\"x\":\"newline\n\"}", "{\"n\":1,\"x\":[{]}}",
    };
    for (const char* body : bad) {
        int64_t n = 0; EXPECT(!jsonGetInt(body, strlen(body), "n", &n));
    }
    char out[64];
    const char* body = "{\"s\":\"Timer \\\"A\\\" \\u263a\"}";
    EXPECT(jsonCopyString(body, strlen(body), "s", out, sizeof(out)) > 0);
    EXPECT_STREQ(out, "Timer \"A\" \xe2\x98\xba");
    const char* nul = "{\"s\":\"a\\u0000b\"}";
    EXPECT(jsonCopyString(nul, strlen(nul), "s", out, sizeof(out)) < 0);
    JsonWriter jw(out, sizeof(out)); jw.valueInt(INT64_MIN); EXPECT(jw.finalize() > 0);
    EXPECT_STREQ(out, "-9223372036854775808");
}

}  // namespace

int main() {
    test_strict_complete_json_and_unescaping();
    test_array_of_objects_has_commas();
    test_array_of_arrays_has_commas();
    test_object_with_nested_object_value();
    test_int_rejects_trailing_garbage();
    test_int_accepts_pure_negative();
    test_int_rejects_float();
    test_int_rejects_exponent();
    test_int_rejects_plus_sign();
    test_int_rejects_lonely_minus();
    test_int_rejects_int64_overflow();
    test_int_rejects_leading_zero();
    test_int_accepts_zero_alone();
    test_findkey_rejects_duplicate_keys();
    test_findkey_rejects_truncated_object();
    test_bool_rejects_lookalike();
    test_finalize_overflow_returns_zero_and_flags();
    test_finalize_nonoverflow_returns_len_plus_nul();

    if (failures) {
        std::fprintf(stderr, "\n%d test(s) FAILED\n", failures);
        return 1;
    }
    std::fprintf(stdout, "all relay_json tests passed\n");
    return 0;
}
