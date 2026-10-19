#include "CheckedJsonWriter.h"
#include <cassert>
#include <iostream>
#include <string>

struct Buffer {
  std::string value;
  int failAt = -1, calls = 0;
  bool reserve(size_t) { return calls++ != failAt; }
  bool concat(const char* data, size_t length) {
    if (calls++ == failAt) return false;
    value.append(data, length); return true;
  }
  size_t length() const { return value.length(); }
};
bool build(Buffer& buffer, size_t limit = 2560) {
  CheckedJsonWriter<Buffer> json(buffer, limit);
  json.text("name", "a\"b\\c\n\t\x01中文");
  json.number("uptime", 0xffffffff);
  json.signedNumber("signal", -2147483647 - 1);
  json.boolean("open", true);
  json.null("session");
  json.object("mutation"); json.boolean("changed", false); json.endObject();
  return json.finish();
}
int main() {
  Buffer buffer;
  assert(build(buffer));
  assert(buffer.value == "{\"name\":\"a\\\"b\\\\c\\u000a\\u0009\\u0001中文\",\"uptime\":4294967295,\"signal\":-2147483648,\"open\":true,\"session\":null,\"mutation\":{\"changed\":false}}");
  for (int fail = 0; fail < buffer.calls; ++fail) {
    Buffer broken; broken.failAt = fail;
    assert(!build(broken));
    assert(broken.calls == fail + 1); // sticky: no more appends after failure
  }
  for (size_t limit = 0; limit < buffer.length(); ++limit) {
    Buffer bounded; assert(!build(bounded, limit));
    assert(bounded.length() <= limit);
  }
  Buffer exact; assert(build(exact, buffer.length()));
  Buffer alreadyOversize; alreadyOversize.value = "existing";
  assert(!build(alreadyOversize, 3)); // no unsigned subtraction wraparound
  assert(alreadyOversize.value == "existing");
  std::cout << "Checked status JSON escaping, boundaries and every allocation-failure point passed\n";
}
