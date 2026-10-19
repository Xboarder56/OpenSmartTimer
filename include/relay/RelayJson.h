#pragma once

// Fixed-buffer JSON encoder + minimal reader for ost-relay/1 frames.
//
// Deliberately does not depend on ArduinoJson or nlohmann — we know
// the exact set of frame shapes we send (§ 7 of the spec) and their
// upper size bounds, so a small purpose-built encoder wins on
// footprint and predictable heap.
//
// The reader is intentionally minimal: it locates a top-level key
// and returns a view into the original buffer. This is enough for
// dispatching by `type`, extracting `downstream_seq`, and reading
// `cap_token` / `op_id` / `params`. Anything nested inside `params`
// is pulled out by re-scanning with a nested reader on the slice.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace opensmarttimer { namespace relay {

// ---------------------------------------------------------------------------
// Writer — appends into a caller-owned buffer with overflow checking.
// ---------------------------------------------------------------------------

class JsonWriter {
public:
    JsonWriter(char* buf, size_t capacity)
        : buf_(buf), cap_(capacity), pos_(0), first_(true), overflowed_(false) {}

    // Object / array scope. Nested constructs count as values in the
    // surrounding scope, so we run maybeComma() on entry and mark the
    // outer scope as "not first any more" on exit. Without this,
    // array-of-objects and array-of-arrays lose their separating
    // commas (regression tested).
    void beginObject() { maybeComma(); emit('{'); first_ = true; }
    void endObject()   { emit('}'); first_ = false; }
    void beginArray()  { maybeComma(); emit('['); first_ = true; }
    void endArray()    { emit(']'); first_ = false; }

    // Key/value writes. Comma is inserted automatically as needed.
    void key(const char* k) {
        maybeComma();
        emit('"'); emitStr(k); emit('"'); emit(':');
        first_ = true;
    }
    void valueString(const char* s) {
        maybeComma();
        emit('"'); emitStrEscaped(s); emit('"');
        first_ = false;
    }
    void valueStringN(const char* s, size_t n) {
        maybeComma();
        emit('"'); emitStrEscapedN(s, n); emit('"');
        first_ = false;
    }
    void valueUint(uint64_t v)  { maybeComma(); emitUint(v);  first_ = false; }
    void valueInt(int64_t v)    { maybeComma(); emitInt(v);   first_ = false; }
    void valueBool(bool v)      { maybeComma(); emitStr(v ? "true" : "false"); first_ = false; }
    void valueNull()            { maybeComma(); emitStr("null"); first_ = false; }
    // Splice a raw already-encoded JSON fragment (e.g. an inner object
    // the caller assembled separately). Caller warrants well-formedness.
    void valueRaw(const char* s, size_t n) {
        maybeComma();
        for (size_t i = 0; i < n; ++i) emit(s[i]);
        first_ = false;
    }

    size_t length() const { return pos_; }
    bool overflowed() const { return overflowed_; }
    // Finalize; returns number of bytes written including a trailing
    // NUL. Zero if overflowed.
    size_t finalize() {
        if (overflowed_ || pos_ >= cap_) { overflowed_ = true; return 0; }
        buf_[pos_] = '\0';
        return pos_ + 1;
    }

private:
    void emit(char c) {
        if (pos_ + 1 > cap_) { overflowed_ = true; return; }
        buf_[pos_++] = c;
    }
    void emitStr(const char* s) { while (*s && !overflowed_) emit(*s++); }
    void emitStrEscaped(const char* s) { emitStrEscapedN(s, s ? strlen(s) : 0); }
    void emitStrEscapedN(const char* s, size_t n) {
        for (size_t i = 0; i < n && !overflowed_; ++i) {
            unsigned char c = (unsigned char)s[i];
            switch (c) {
                case '"':  emit('\\'); emit('"'); break;
                case '\\': emit('\\'); emit('\\'); break;
                case '\n': emit('\\'); emit('n'); break;
                case '\r': emit('\\'); emit('r'); break;
                case '\t': emit('\\'); emit('t'); break;
                default:
                    if (c < 0x20) {
                        static const char* hex = "0123456789abcdef";
                        emit('\\'); emit('u'); emit('0'); emit('0');
                        emit(hex[(c >> 4) & 0xF]); emit(hex[c & 0xF]);
                    } else {
                        emit((char)c);
                    }
            }
        }
    }
    void emitUint(uint64_t v) {
        char tmp[24]; int i = 0;
        if (v == 0) { emit('0'); return; }
        while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
        while (i--) emit(tmp[i]);
    }
    void emitInt(int64_t v) {
        if (v < 0) { emit('-'); emitUint(uint64_t(-(v + 1)) + 1); }
        else emitUint((uint64_t)v);
    }
    void maybeComma() {
        if (!first_) emit(',');
        first_ = false;
    }

    char*  buf_;
    size_t cap_;
    size_t pos_;
    bool   first_;
    bool   overflowed_;
};

// ---------------------------------------------------------------------------
// Reader — locates top-level keys in a JSON object without materializing
// the whole tree. Returns views into the caller-owned buffer.
// ---------------------------------------------------------------------------

struct JsonView {
    const char* data;    // NOT nul-terminated; use `len`
    size_t      len;
    bool        found;
    bool        is_string;   // "…" quotes stripped in `data`
    bool        is_number;
    bool        is_object;
    bool        is_array;
    bool        is_bool_true;
    bool        is_bool_false;
    bool        is_null;
};

// Finds `key` in a top-level JSON object. `body` must point at the
// object's opening `{` (or just after — leading whitespace is
// tolerated). Sets `out.found = false` if the key is absent. Bounded
// to `body_len` bytes so a truncated buffer cannot walk off the end.
bool jsonFindKey(const char* body, size_t body_len, const char* key,
                 JsonView* out);

// Convenience typed getters. Return false when the key is missing or
// the type doesn't match.
bool jsonGetString(const char* body, size_t body_len, const char* key,
                   const char** out, size_t* out_len);
bool jsonGetInt(const char* body, size_t body_len, const char* key,
                int64_t* out);
bool jsonGetBool(const char* body, size_t body_len, const char* key,
                 bool* out);

// Copy a jsonGetString view into a NUL-terminated buffer. Returns the
// number of copied bytes (excluding the NUL) or a negative value if
// the destination was too small.
int jsonCopyString(const char* body, size_t body_len, const char* key,
                   char* out, size_t out_capacity);

}  // namespace relay
}  // namespace opensmarttimer
