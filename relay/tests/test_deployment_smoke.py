"""Regression for an HTML page whose shared assets fail behind a proxy."""
import importlib.util
from pathlib import Path
import pytest
import re

spec = importlib.util.spec_from_file_location('deployment_smoke',Path(__file__).resolve().parents[1]/'tools/deployment_smoke.py')
smoke=importlib.util.module_from_spec(spec);spec.loader.exec_module(smoke)


def test_referenced_css_failure_is_not_hidden_by_successful_html(monkeypatch):
    def fetch(url, context, expected=200):
        if url.endswith('/assets/timer-ui.css'):raise ValueError('expected HTTP 200, got 404')
        return b'<html><link rel="stylesheet" href="/assets/timer-ui.css"></html>',{'Content-Type':'text/html'}
    monkeypatch.setattr(smoke,'fetch',fetch)
    with pytest.raises(ValueError,match='got 404'):smoke.page_assets('https://relay.example/',None)


def test_html_in_place_of_javascript_is_refused(monkeypatch):
    def fetch(url, context, expected=200):
        body=b'<script src="/assets/appearance.js"></script>'
        return body,{'Content-Type':'text/html'}
    monkeypatch.setattr(smoke,'fetch',fetch)
    with pytest.raises(ValueError,match='incorrect asset type'):smoke.page_assets('https://relay.example/',None)


def test_origin_refuses_credentials_paths_fragments_and_plain_http():
    for url in ('http://relay.example','https://user:secret@relay.example','https://relay.example/s/','https://relay.example#secret'):
        with pytest.raises(Exception):smoke.origin(url)
    assert smoke.origin('https://relay.example:8444/')=='https://relay.example:8444'


def test_https_fixture_and_operator_proxy_share_public_route_policy():
    root=Path(__file__).resolve().parents[1]
    policies=[]
    for path in (root/'tests/Caddyfile',root/'ops/Caddyfile'):
        line=re.search(r'^\s*@public path (.+)$',path.read_text(),re.M)[1]
        policies.append(set(line.split()))
    assert policies[0]==policies[1], 'HTTPS fixture must exercise the shipped operator allowlist'
    assert '/assets/*' in policies[0] and not any(p.startswith('/admin') or p.startswith('/s') and not p.startswith('/setup') for p in policies[0])
