"""Compile the real request validator against both cores' argument conventions."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/WebRequests.cpp').read_text()
methods = '\n'.join(re.search(r'(?:bool|void|String) WebApplication::' + name + r'\([^\n]*\)(?: const)? \{.*?^\}',
                               source, re.MULTILINE | re.DOTALL)[0]
                    for name in ('validateRequestFields', 'sendRequestFieldError', 'requestValue'))
assert '"Content-Type"' in (ROOT / 'src/WebApplication.cpp').read_text().split('server_.collectHeaders', 1)[0]
stub = r'''
#include "HttpRequestFields.h"
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
struct String : std::string {
  using std::string::string;
  String(const std::string& s) : std::string(s) {}
  bool startsWith(const char* prefix) const { return rfind(prefix, 0) == 0; }
  int indexOf(char c) const { const auto p = find(c); return p == npos ? -1 : static_cast<int>(p); }
  String substring(unsigned from, unsigned to) const { return substr(from, to - from); }
  void trim() {
    const auto first = find_first_not_of(" \t\r\n");
    if (first == npos) { clear(); return; }
    *this = substr(first, find_last_not_of(" \t\r\n") - first + 1);
  }
};
struct Server {
  std::vector<std::pair<String, String>> fields;
  String contentType, rawBody, path = "/api/v1/ota/prepare";
  bool hasPlain = false;
  // ESP8266 keeps its synthetic plain argument outside args(); ESP32 includes it.
  bool plainCounted = false;
  unsigned args() const { return fields.size() + (hasPlain && plainCounted ? 1 : 0); }
  bool hasArg(const String& name) const {
    if (name == "plain" && hasPlain) return true;
    for (const auto& field : fields) if (field.first == name) return true;
    return false;
  }
  String argName(unsigned i) const { return i < fields.size() ? fields[i].first : String("plain"); }
  String arg(unsigned i) const { return i < fields.size() ? fields[i].second : rawBody; }
  String arg(const String& name) const {
    if (name == "plain" && hasPlain) return rawBody;
    for (const auto& field : fields) if (field.first == name) return field.second;
    return {};
  }
  String header(const String& name) const { return name == "Content-Type" ? contentType : String(); }
  String uri() const { return path; }
};
struct WebApplication {
  Server server_;
  HttpRequestFields requestFields_;
  bool requestIsJson_ = false;
  void* capture_ = nullptr;
  const char* captureSecret_ = nullptr;
  unsigned captureSecretLength_ = 0;
  int status = 0;
  String code;
  void sendError(int s, const String&, const String& c) { status = s; code = c; }
  bool validateRequestFields();
  void sendRequestFieldError();
  String requestValue(const String&) const;
};
'''
checks = r'''
int main() {
  for (const bool v1 : {true, false}) {
    WebApplication app;
    auto& server = app.server_;
    server.plainCounted = !v1;
    server.contentType = "application/x-www-form-urlencoded;charset=UTF-8";
    server.fields = {{"image_size", "459328"}, {"image_crc32", "25b2d8a2"}};
    server.hasPlain = v1;
    server.rawBody = "image_size=459328&image_crc32=25b2d8a2";
    assert(app.validateRequestFields() && !app.requestIsJson_);
    assert(app.requestValue("image_size") == "459328");
    assert(app.requestValue("image_crc32") == "25b2d8a2");
    // Form/query duplicates must still fail rather than choosing a precondition.
    server.fields.push_back({"image_size", "1"});
    assert(!app.validateRequestFields() && app.code == "duplicate_field");
    server.fields.pop_back();
    server.fields[0].second = std::string(2049, 'x');
    assert(!app.validateRequestFields() && app.status == 413);
    server.fields = {{"image_size", "459328"}, {"image_crc32", "25b2d8a2"}};
    server.contentType = "multipart/form-data; boundary=example";
    server.hasPlain = false;
    assert(app.validateRequestFields() && !app.requestIsJson_);
    // JSON requests work in both core conventions, with and without parameters.
    for (const char* type : {"application/json", "application/json; charset=utf-8"}) {
      server.contentType = type;
      server.fields.clear();
      server.hasPlain = true;
      server.rawBody = "{\"image_size\":459328,\"image_crc32\":\"25b2d8a2\"}";
      assert(app.validateRequestFields() && app.requestIsJson_);
      assert(app.requestValue("image_size") == "459328");
      server.fields.push_back({"image_size", "1"});
      assert(!app.validateRequestFields() && app.code == "ambiguous_request");
      server.fields.clear();
      server.rawBody = "{\"image_size\":459328,\"image_size\":1}";
      assert(!app.validateRequestFields() && app.code == "duplicate_field");
      server.rawBody = "image_size=459328";
      assert(!app.validateRequestFields() && app.code == "invalid_json");
    }
    server.contentType = "";
    server.hasPlain = false;
    server.fields = {{"image_size", "459328"}, {"image_crc32", "25b2d8a2"}};
    assert(app.validateRequestFields() && !app.requestIsJson_);
    assert(app.requestValue("image_crc32") == "25b2d8a2");
    server.fields.clear();
    assert(app.validateRequestFields() && !app.requestIsJson_);
  }
}
'''
with tempfile.TemporaryDirectory() as directory:
    binary = str(Path(directory) / 'request-transport')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'include'), str(ROOT / 'src/HttpRequestFields.cpp'),
                    '-x', 'c++', '-', '-o', binary], input=stub + methods + checks,
                   text=True, check=True)
    subprocess.run([binary], check=True)
print('Actual V1/V2 request validator accepts forms/JSON and rejects mixed, duplicate or oversized inputs')
