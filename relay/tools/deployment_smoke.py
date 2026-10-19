#!/usr/bin/env python3
"""Read-only checks of the actual deployed HTTPS pages, assets and isolation.

Uses system TLS trust by default. Explicit CA files are for private deployments;
they do not establish public-browser trust. Never accepts insecure TLS or tokens.
"""
import argparse
import json
import ssl
from html.parser import HTMLParser
from urllib.error import HTTPError
from urllib.parse import urljoin, urlsplit
from urllib.request import Request, urlopen


class Assets(HTMLParser):
    def __init__(self):
        super().__init__(); self.paths = set()

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if tag == 'script' and values.get('src'):
            self.paths.add(values['src'])
        if tag == 'link' and set(values.get('rel', '').split()) & {'stylesheet', 'icon', 'manifest'}:
            if values.get('href'): self.paths.add(values['href'])


def origin(value):
    url = urlsplit(value)
    if (url.scheme != 'https' or not url.hostname or url.username or url.password
            or url.path not in ('', '/') or url.query or url.fragment):
        raise argparse.ArgumentTypeError('Supply an HTTPS origin without a path, credentials or fragment')
    return value.rstrip('/')


def fetch(url, context, expected=200):
    try:
        response = urlopen(Request(url, headers={'Cache-Control': 'no-cache'}), context=context, timeout=10)
    except HTTPError as error:
        response = error
    with response:
        body = response.read(1024 * 1024 + 1)
        if response.status != expected or len(body) > 1024 * 1024:
            raise ValueError(f'{urlsplit(url).path}: expected HTTP {expected}, got {response.status}; oversized={len(body)>1024*1024}')
        return body, response.headers


def page_assets(url, context):
    body, headers = fetch(url, context)
    if 'text/html' not in headers.get('Content-Type', ''):
        raise ValueError(f'{urlsplit(url).path}: expected HTML')
    parser = Assets(); parser.feed(body.decode('utf-8'))
    if not parser.paths: raise ValueError('Page has no appearance/script assets')
    checked = []
    for reference in sorted(parser.paths):
        absolute = urljoin(url, reference)
        target = urlsplit(absolute)
        if target.scheme != 'https' or target.netloc != urlsplit(url).netloc:
            raise ValueError('Page references assets outside its HTTPS origin')
        payload, asset_headers = fetch(absolute, context)
        media = asset_headers.get('Content-Type', '').split(';')[0]
        expected = {'.css': {'text/css'}, '.js': {'text/javascript', 'application/javascript'},
                    '.svg': {'image/svg+xml'}, '.webmanifest': {'application/manifest+json', 'application/json'}}
        suffix = '.' + target.path.rsplit('.', 1)[-1]
        if suffix in expected and media not in expected[suffix]:
            raise ValueError(f'{target.path}: incorrect asset type {media}')
        if not payload: raise ValueError(f'{target.path}: empty asset')
        checked.append(target.path)
    return checked


def check(relay, partner, relay_ca=None, partner_ca=None, landing_hidden=False):
    relay = origin(relay); partner = origin(partner)
    if relay == partner: raise ValueError('Relay and trusted page must have separate origins')
    backend_tls = ssl.create_default_context(cafile=relay_ca)
    frontend_tls = ssl.create_default_context(cafile=partner_ca)
    health, _ = fetch(relay+'/api/v1/health', backend_tls)
    if json.loads(health).get('protocol') != 'ost-relay/1': raise ValueError('Unexpected relay protocol')
    assets = []
    if landing_hidden: fetch(relay+'/', backend_tls, 404)
    else: assets += page_assets(relay+'/', backend_tls)
    assets += page_assets(partner+'/s/', frontend_tls)
    # Deep routes must serve the shell even without a capability; no control is authorized.
    fetch(partner+'/s/'+'0'*22, frontend_tls)
    for path in ('/admin/', '/admin/assets/admin.js', '/docs', '/openapi.json', '/s/assets/app.js', '/assets/app.js'):
        fetch(relay+path, backend_tls, 404)
    return {'passed': True, 'protocol': 'ost-relay/1', 'checked_assets': assets,
            'trust': 'explicit private CA' if relay_ca or partner_ca else 'system TLS trust',
            'scope': 'actual HTTPS landing/control shells, every referenced asset, deep route and public admin/partner-code isolation; no timer mutations or OIDC login'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--relay', required=True, type=origin)
    parser.add_argument('--partner', required=True, type=origin)
    parser.add_argument('--relay-ca'); parser.add_argument('--partner-ca')
    parser.add_argument('--landing-hidden', action='store_true')
    args = parser.parse_args()
    print(json.dumps(check(args.relay, args.partner, args.relay_ca, args.partner_ca, args.landing_hidden), indent=2))


if __name__ == '__main__':
    main()
