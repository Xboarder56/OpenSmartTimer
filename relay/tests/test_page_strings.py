"""Landing, setup and admin pages are bilingual: every string they show has a
Simplified Chinese entry, either in the shared timer table or in the relay's
own page strings."""
import json
import re
from html.parser import HTMLParser
from pathlib import Path

from ostrelay import admin_page, page_strings, setup_page

ROOT = Path(__file__).resolve().parents[2]
LITERAL = re.compile(r"'((?:[^'\\]|\\.)*)'")


def _tables():
    body = page_strings.PAGE_STRINGS_JS
    body = body[body.index("{"):body.rindex("}") + 1]
    own = json.loads(re.sub(r",\s*}\s*$", "}", body))
    shared = (ROOT / "relay/spa/i18n.js").read_text(encoding="utf-8")
    return own, shared


def _known(key, own, shared):
    return key in own or ("'" + key.replace("'", "\\'") + "':") in shared or ('"' + key + '":') in shared


def _script_keys(source):
    """String literals passed to t(...) or count(...), plus the error table."""
    keys = set()
    for call in re.finditer(r"\b(?:t|count)\(", source):
        depth, index = 1, call.end()
        while depth and index < len(source):
            char = source[index]
            if char == "'":
                match = LITERAL.match(source, index)
                keys.add(match.group(1).replace("\\'", "'"))
                index = match.end()
                continue
            depth += (char == "(") - (char == ")")
            index += 1
    if "const ERRORS = {" in source:
        start = source.index("const ERRORS = {")
        keys.update(m.group(1) for m in re.finditer(r":\s*" + LITERAL.pattern, source[start:source.index("};", start)]))
    return {k for k in keys if re.search(r"[A-Za-z]{2}", k) and not re.fullmatch(r"[a-z_]+", k)}


class _Text(HTMLParser):
    SKIP = ("code", "script", "style", "output", "svg", "title")

    def __init__(self):
        super().__init__()
        self.depth, self.found = 0, set()

    def handle_starttag(self, tag, attrs):
        self.depth += tag in self.SKIP
        self.found.update(v for k, v in attrs if k in ("aria-label", "placeholder") and v)

    def handle_endtag(self, tag):
        self.depth -= tag in self.SKIP and self.depth > 0

    def handle_data(self, data):
        text = data.strip()
        if text and not self.depth and re.search(r"[A-Za-z]{2}", text) and not re.fullmatch(r"\{[a-z]+\}|@[A-Z_]+@", text):
            self.found.add(text)


def test_every_script_message_has_a_translation():
    own, shared = _tables()
    keys = _script_keys(admin_page.ADMIN_JS) | _script_keys(setup_page.SETUP_JS)
    assert len(keys) > 80
    assert sorted(k for k in keys if not _known(k, own, shared)) == []


def test_every_static_string_has_a_translation():
    own, shared = _tables()
    source = (ROOT / "relay/src/ostrelay/setup_page.py").read_text(encoding="utf-8")
    cards = source[source.index("    action = ("):source.index("    page = (_HOME")]
    parser = _Text()
    for html in (admin_page.ADMIN_PAGE, setup_page._PAGE, setup_page._HOME,
                 "".join(re.findall(r"f?'((?:[^'\\]|\\.)*)'", cards)).replace("\\'", "'")):
        parser.feed(html)
    # The language switch labels itself and names both languages.
    expected_untranslated = {"EN", "Switch to Simplified Chinese"}
    assert sorted(k for k in parser.found - expected_untranslated if not _known(k, own, shared)) == []


def test_translations_keep_their_placeholders_and_do_not_shadow_the_shared_table():
    own, shared = _tables()
    for key, value in own.items():
        assert sorted(re.findall(r"\{\w+\}", key)) == sorted(re.findall(r"\{\w+\}", value)), key
        assert not _known(key, {}, shared), f"{key!r} is already in the shared table"


def test_pages_load_their_strings_before_the_translator():
    for page, strings in ((admin_page.ADMIN_PAGE, "/admin/assets/strings.js"),
                          (setup_page._PAGE, "/setup/assets/strings.js"), (setup_page._HOME, "/setup/assets/strings.js")):
        assert 'id="languageButton"' in page
        assert page.index(strings) < page.index("/assets/i18n.js")
    assert "...window.SmartTimerPageStrings," in (ROOT / "relay/spa/i18n.js").read_text(encoding="utf-8")
