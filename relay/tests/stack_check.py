#!/usr/bin/env python3
"""Exercise an isolated HTTPS/WSS/SSE Docker stack using only fake timers.

Creates a unique Compose project, temporary credentials, CA and named volume.
Never reads relay/.env, contacts a physical timer, or operates on other projects.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import datetime as dt
import hashlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import statistics
import subprocess
import sys
import tempfile
import time

import httpx
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID, ExtendedKeyUsageOID

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT.parent / "tmp/relay-tests"
sys.path.insert(0,str(ROOT / "mock-timer"))
from secure import BrowserOpen
sys.path.insert(0, str(ROOT / "tools"))
from deployment_smoke import check as deployment_check
spec = importlib.util.spec_from_file_location("ostrelay_mock", ROOT / "mock-timer/mock.py")
mock = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mock
spec.loader.exec_module(mock)


def certificates(path: Path) -> str:
    now = dt.datetime.now(dt.timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "OST isolated test CA")])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(now - dt.timedelta(minutes=1)).not_valid_after(now + dt.timedelta(days=1))
          .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
          .add_extension(x509.SubjectKeyIdentifier.from_public_key(ca_key.public_key()), critical=False)
          .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
          .add_extension(x509.KeyUsage(True, False, False, False, False, True, True, None, None), critical=True)
          .sign(ca_key, hashes.SHA256()))
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")]))
            .issuer_name(ca_name).public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - dt.timedelta(minutes=1)).not_valid_after(now + dt.timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
            .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()), critical=False)
            .add_extension(x509.KeyUsage(True, False, True, False, False, False, False, None, None), critical=True)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost"),
                x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
            .sign(ca_key, hashes.SHA256()))
    (path / "ca.pem").write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    (path / "fullchain.pem").write_bytes(cert.public_bytes(serialization.Encoding.PEM) + ca.public_bytes(serialization.Encoding.PEM))
    (path / "key.pem").write_bytes(key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    (path / "key.pem").chmod(0o600)
    spki = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return mock.b64url(hashlib.sha256(spki).digest())


def run(command: list[str], *, capture: bool = True, env=None) -> str:
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None, env=env)
    if result.returncode:
        # Compose output can name resources, but commands/secret env are never printed.
        raise RuntimeError((result.stderr or result.stdout or "command failed")[-5000:])
    return result.stdout or ""


def parse_authenticated_response(frame):
    # Called only after the timer-identity handshake and channel AEAD verification.
    assert set(frame) == {"type", "upstream_seq", "body_json"}, "unexpected encrypted response envelope"
    return json.loads(frame["body_json"])


async def wait_for(predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("timed out waiting for stack state")
        await asyncio.sleep(0.03)


async def exercise(url, partner_url, admin_url, admin_token, cert_dir, compose, report, browser):
    tls = ssl.create_default_context(cafile=str(cert_dir / "ca.pem"))
    timers, clients, tasks, infos, tokens, watches = [], [], [], [], [], []
    channels, captured = [], []
    received = {}
    latency = []
    owner_server = None
    owner_socket = None
    admin = httpx.AsyncClient(base_url=admin_url, headers={"Authorization": f"Bearer {admin_token}"}, trust_env=False)
    public = httpx.AsyncClient(base_url=url, verify=tls, trust_env=False)
    try:
        report["deployed_pages"] = deployment_check(url, partner_url,
            str(cert_dir/"ca.pem"), str(cert_dir/"partner/ca.pem"))
        health = await public.get("/api/v1/health")
        assert health.status_code == 200
        assert health.headers["referrer-policy"] == "no-referrer"
        assert (await public.get("/admin/timers", headers={"Authorization": f"Bearer {admin_token}"})).status_code == 404
        assert (await public.get("/docs")).status_code == 404
        assert (await public.get("/s/assets/app.js")).status_code == 404
        async with httpx.AsyncClient(verify=ssl.create_default_context(cafile=str(cert_dir/"partner/ca.pem")),trust_env=False) as trusted:
            shell=await trusted.get(partner_url+"/s/")
            assert shell.status_code==200 and url in shell.headers["content-security-policy"]
            assert "frame-ancestors 'none'" in shell.headers["content-security-policy"]
        report["https_static_admin_isolation"] = True

        for i in range(3):
            code = (await admin.post("/admin/enroll-code")).json()["enrollment_code"]
            cred = await mock.enroll(url, code, tls)
            timer = mock.MockTimerSession(cred, tls,partner_url)
            await timer.connect()
            timers.append(timer)
            tasks.extend([asyncio.create_task(timer.read_loop()), asyncio.create_task(timer.periodic_status(0.5))])
            wire, record = timer.keys.mint(
                    scopes=["view", "pause_resume", "adjust", "release", "start", "lock", "identify"],
                    ttl_ms=120000, boot_id=timer.timer.boot_id, now_up_ms=timer.uptime_ms(),
                    timer_id=cred.timer_id, label=f"stack-{i}")
            tokens.append((wire, record))
            await timer.send_token_mint(record)
            for _ in range(100):
                records = (await admin.get(f"/admin/tokens/{cred.timer_id}")).json()
                if any(item["fp_hash"] == record.fp_hash for item in records):
                    break
                await asyncio.sleep(0.03)
            else:
                raise AssertionError("mock token announcement was not registered")
            for _ in range(2):
                client = httpx.AsyncClient(base_url=url, verify=tls, trust_env=False, timeout=10)
                clients.append(client)
                await asyncio.sleep(1.05)
                opened=BrowserOpen(wire)
                request={k:v for k,v in opened.frame.items() if k!='type'}
                captured.append(request)
                response = await client.post("/api/v1/session", json=request)
                assert response.status_code == 200, f"session exchange: {response.status_code} {response.text}"
                info = response.json(); infos.append(info)
                client.headers["X-OST-Session"] = info["session_handle"]
                assert not response.cookies
                channels.append(opened.accept(info["frame"]))
                watches.append({"latest": None, "live": 0, "terminal": None})
                index = len(clients) - 1

                async def watch(index=index, timer=timer):
                    async with clients[index].stream("GET", "/api/v1/watch", params={"session_handle": infos[index]["session_handle"]}) as response:
                        assert response.status_code == 200
                        event = ""; data = ""
                        async for line in response.aiter_lines():
                            if line.startswith("event:"):
                                event = line[6:].strip()
                            elif line.startswith("data:"):
                                data += line[5:].strip()
                            elif not line:
                                if event == "e2e" and data:
                                    frame=json.loads(data)
                                    if frame["seq"] > channels[index].rx_seq:
                                        inner=channels[index].decrypt(frame)
                                        received[(index,frame["request_id"])]=inner
                                        if inner["type"]=="status":
                                            watches[index]["latest"]=parse_authenticated_response(inner)
                                            watches[index]["live"]+=1
                                elif event in ("token_revoked", "token_expired", "session_expired", "timer_gone"):
                                    watches[index]["terminal"] = event
                                event = ""; data = ""
                tasks.append(asyncio.create_task(watch()))
        for index,client in enumerate(clients):
            request='sync-'+str(index)
            frame=channels[index].encrypt({'type':'sync','cap_token':tokens[index//2][0]},request)
            captured.append(frame)
            response=await client.post('/api/v1/command',json=frame)
            assert response.status_code==200
            outer=response.json()
            if outer['seq'] > channels[index].rx_seq:
                inner=channels[index].decrypt(outer)
                received[(index,request)]=inner
                watches[index]['latest']=parse_authenticated_response(inner)
                watches[index]['live']+=1
        await wait_for(lambda: all(w['live'] for w in watches))
        report["timers"] = len(timers); report["sse_watchers"] = len(watches)

        async def command(index, op, params=None, intent=None):
            snap = watches[index]["latest"]
            intent = intent or {"op": op, "op_id": "check-" + secrets.token_hex(8), "params": params or {},
                "if_boot_id": snap["boot_id"], "if_session_id": snap["session_id"], "observed_uptime_ms": snap["uptime_ms"]}
            before = time.perf_counter()
            frame=channels[index].encrypt({**intent,'type':'command','cap_token':tokens[index//2][0],'expiry_uptime_ms':intent['observed_uptime_ms']+5000},intent['op_id'])
            captured.append(frame)
            response = await clients[index].post("/api/v1/command", json=frame)
            latency.append((time.perf_counter() - before) * 1000)
            assert response.status_code == 200, f"command: {response.status_code} {response.text}"
            outer=response.json()
            if outer['seq'] > channels[index].rx_seq:
                received[(index,intent['op_id'])]=channels[index].decrypt(outer)
            await wait_for(lambda:(index,intent['op_id']) in received)
            body = parse_authenticated_response(received[(index,intent['op_id'])])
            assert body["op_id"] == intent["op_id"] and body["op"] == op
            assert body["code"] == "applied", body["code"]
            watches[index]["latest"] = body["status"]
            return intent, body

        await asyncio.gather(*(command(i * 2, "start", {"duration_ms": 600000}) for i in range(3)))
        for _ in range(10):
            await asyncio.gather(*(command(i * 2, "adjust", {"delta_ms": 1000}) for i in range(3)))
            await asyncio.sleep(0.3)
        intent, result = await command(0, "adjust", {"delta_ms": 1000})
        revision = timers[0].timer.state_revision
        await command(0, "adjust", intent=intent)
        assert timers[0].timer.state_revision == revision
        lookup_frame=channels[0].encrypt({'type':'lookup','cap_token':tokens[0][0]},intent['op_id'])
        lookup = await clients[0].post("/api/v1/command",json=lookup_frame)
        assert lookup.status_code == 200
        assert lookup.json()["type"]=="e2e"
        assert (await clients[1].get(f"/api/v1/result/{intent['op_id']}")).status_code == 404
        report["dedup_and_private_result_lookup"] = True
        assert (await clients[0].get("/api/v1/session")).status_code == 200
        wrong={**lookup_frame,'client_id':infos[1]['frame']['client_id']}
        assert (await clients[0].post("/api/v1/command",json=wrong)).status_code==409
        report["session_refresh_and_tab_binding"] = True

        # Reject the obsolete body shape without leaking any live grant secret.
        assert (await public.post("/api/v1/session", json={"token": "synthetic-invalid-legacy-credential"})).status_code == 422
        report["legacy_bearer_exchange_rejected"] = True
        # A hello bound to another TLS key, as when something terminates TLS
        # between the timer and the relay, is refused by the relay. The
        # enrolled timer's real connection is not displaced by the attempt.
        interposed = mock.MockTimerSession(mock.MockCredentials.from_dict(timers[0].cred.to_dict()), tls)
        genuine_tag = mock.hello_tag
        mock.hello_tag = lambda *fields: genuine_tag(*fields[:-1], bytes(range(32)))
        try:
            try:
                await interposed.connect()
            except Exception:
                pass
            else:
                raise AssertionError("a hello bound to another TLS key was accepted")
        finally:
            mock.hello_tag = genuine_tag
        assert (await clients[0].get("/api/v1/session")).status_code == 200
        report["wss_foreign_tls_key_rejected"] = True

        # Same-boot WSS reconnect preserves browser session and token identity.
        await timers[0].ws.close()
        await asyncio.sleep(0.1)
        await timers[0].connect()
        tasks.extend([asyncio.create_task(timers[0].read_loop()), asyncio.create_task(timers[0].periodic_status(0.5))])
        await wait_for(lambda: timers[0].upstream_seq > 0)
        assert (await clients[0].get("/api/v1/session")).status_code == 200
        report["same_boot_reconnect"] = True

        # Server-side revoke must terminate only the affected token's streams.
        assert (await admin.post(f"/admin/tokens/{tokens[2][1].fp_hash}/revoke")).status_code == 200
        await wait_for(lambda: watches[4]["terminal"] is not None)
        assert watches[2]["terminal"] is None
        assert (await clients[4].get("/api/v1/session")).status_code == 401
        assert (await clients[2].delete("/api/v1/session")).status_code == 200
        await wait_for(lambda: watches[2]["terminal"] is not None)
        assert (await clients[3].get("/api/v1/session")).status_code == 200
        report["targeted_revoke_and_logout"] = True

        # Release timer 0 so the optional browser starts with an open mock.
        timers[0].timer.release()
        await timers[0].send_status()
        if browser:
            import uvicorn
            owner_spec = importlib.util.spec_from_file_location("ostrelay_test_owner", ROOT / "mock-timer/owner.py")
            owner_module = importlib.util.module_from_spec(owner_spec)
            owner_spec.loader.exec_module(owner_module)
            owner_socket = socket.socket()
            owner_socket.bind(("127.0.0.1", 0))
            owner_port = owner_socket.getsockname()[1]
            owner_secret = secrets.token_urlsafe(32)
            owner_app = owner_module.create_owner_app(timers[0], url, owner_secret, owner_port)
            owner_server = uvicorn.Server(uvicorn.Config(owner_app, log_level="error", access_log=False, lifespan="off"))
            tasks.append(asyncio.create_task(owner_server.serve(sockets=[owner_socket])))
            await wait_for(lambda: owner_server.started)
            # Lose the first grant-specific ACK on the real WSS read path.
            original_ack = timers[0].handle_token_ack
            dropped = []
            def lose_first_ack(frame):
                if frame.get("action") == "token_mint" and not dropped:
                    dropped.append(True)
                else:
                    original_ack(frame)
            timers[0].handle_token_ack = lose_first_ack
            from host_build import COMMON, compile_host
            from cpp_interop import write_browser_fixture
            cpp_binary = cert_dir / "interop_driver"
            compile_host("interop_driver", COMMON + ["../src/relay/RelayClient.cpp", "../src/relay/RelayClientSecure.cpp"], cpp_binary)
            cpp_fixture = cert_dir / "cpp-browser.json"
            write_browser_fixture(cpp_binary, cpp_fixture)
            env = os.environ.copy()
            env["OSTRELAY_TEST_CPP_FIXTURE"] = str(cpp_fixture)
            env["OSTRELAY_TEST_BROWSER_URL"] = partner_url + "/s/"+tokens[0][1].fp_hash+"#relay="+mock.b64url(url.encode())+"&cap=" + tokens[0][0]
            env["OSTRELAY_TEST_OWNER_URL"] = f"http://127.0.0.1:{owner_port}/#{owner_secret}"
            ARTIFACTS.mkdir(parents=True, exist_ok=True)
            env["OSTRELAY_TEST_SCREENSHOT"] = str(ARTIFACTS / "partner-smoke.png")
            browser_result = await asyncio.to_thread(run, ["node", str(ROOT / "tests/browser_smoke.cjs")], env=env)
            report["browser"] = json.loads(browser_result.strip().splitlines()[-1])
            assert dropped, "creator test must exercise a lost token ACK"
            report["creator_token_ack_loss_recovered"] = True

        for wire,record in tokens:
            assert wire not in json.dumps(captured)
            fp=json.loads(mock.unb64url(wire.split('.')[2]))['fp']
            assert fp not in json.dumps(captured)
        report['relay_transport_contains_no_bearer_or_fingerprint']=True
        report["command_requests"] = len(latency)
        report["command_latency_ms"] = {"median": round(statistics.median(latency), 2), "max": round(max(latency), 2)}
        report["identity_signed_handshake_verified"] = True
        report["aead_verified"] = True
        report["health_after_load"] = (await public.get("/api/v1/health")).status_code == 200
    finally:
        if owner_server:
            owner_server.should_exit = True
            await asyncio.sleep(0.2)
        if owner_socket:
            owner_socket.close()
        for timer in timers:
            if timer.ws: await timer.ws.close()
        for task in tasks: task.cancel()
        task_results = await asyncio.gather(*tasks, return_exceptions=True)
        failures = [r for r in task_results if isinstance(r, Exception) and not isinstance(r, asyncio.CancelledError)]
        for client in clients: await client.aclose()
        await public.aclose(); await admin.aclose()
        if failures:
            raise AssertionError(f"background transport failure: {failures[0]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--browser", action="store_true")
    parser.add_argument("--report", type=Path, default=ARTIFACTS / "stack-latest.json")
    args = parser.parse_args()
    project = "ostrelay-check-" + secrets.token_hex(6)
    report = {"timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "scope": "isolated mock-only development stack", "passed": False}
    with tempfile.TemporaryDirectory(prefix="ostrelay-check-") as directory:
        temp = Path(directory); pin = certificates(temp)
        (temp/"partner").mkdir();certificates(temp/"partner")
        # The partner container drops every capability, so its root can read
        # only what the host user's permissions allow. These disposable files
        # stay private inside the 0700 temporary directory.
        (temp/"partner").chmod(0o755); (temp/"partner"/"key.pem").chmod(0o644)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1",0));partner_port=sock.getsockname()[1]
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
        admin_token = secrets.token_hex(32)
        env = temp / "compose.env"
        env.write_text(f"OSTRELAY_TEST_PORT={port}\nOSTPARTNER_TEST_PORT={partner_port}\nOSTPARTNER_TEST_CERT_DIR={temp}/partner\nOSTRELAY_TEST_ADMIN={admin_token}\nOSTRELAY_TEST_COOKIE={secrets.token_hex(32)}\nOSTRELAY_TEST_PIN={pin}\nOSTRELAY_TEST_CERT_DIR={temp}\n")
        env.chmod(0o600)
        compose = ["docker", "compose", "--project-name", project, "--env-file", str(env), "-f", str(ROOT / "tests/compose.yml")]
        try:
            config_env = os.environ.copy()
            config_env.update(OSTRELAY_PUBLIC_URL="https://localhost", OSTRELAY_HOSTNAME="localhost",
                              OSTRELAY_ADMIN_TOKEN=admin_token, OSTRELAY_COOKIE_SECRET=secrets.token_hex(32),
                              OSTRELAY_SPKI_PIN=pin,OSTRELAY_PARTNER_ORIGIN=f"https://localhost:{partner_port}")
            run(["docker", "compose", "--project-name", project, "--env-file", str(env),
                 "-f", str(ROOT / "docker-compose.yml"), "--profile", "https", "config", "--quiet"], env=config_env)
            report["operator_compose_valid"] = True
            print("Starting isolated HTTPS stack", flush=True)
            run(compose + ["up", "-d", "--wait", "--wait-timeout", "60"] + ([] if args.no_build else ["--build"]))
            run(compose + ["run", "--rm", "--no-deps", "-e", "OSTRELAY_HOSTNAME=localhost",
                "-v", str(ROOT / "ops/Caddyfile") + ":/etc/caddy/operator.Caddyfile:ro",
                "caddy", "caddy", "validate", "--config", "/etc/caddy/operator.Caddyfile", "--adapter", "caddyfile"])
            report["operator_caddy_valid"] = True
            address = run(compose + ["port", "relay", "8080"]).strip()
            asyncio.run(exercise(f"https://localhost:{port}",f"https://localhost:{partner_port}", f"http://{address}", admin_token, temp, compose, report, args.browser))
            relay_id = run(compose + ["ps", "-q", "relay"]).strip()
            report["container_stats"] = json.loads(run(["docker", "stats", "--no-stream", "--format", "{{json .}}", relay_id]))
            report["runtime_non_root_uid"] = int(run(compose + ["exec", "-T", "relay", "id", "-u"]).strip())
            assert report["runtime_non_root_uid"] != 0
            run(compose + ["exec", "-T", "relay", "python", "/opt/ostrelay/ops/database.py", "backup", "/data/ostrelay.sqlite", "/data/snapshot.sqlite"])
            run(compose + ["stop", "relay"])
            run(compose + ["run", "--rm", "--no-deps", "--entrypoint", "python", "relay", "/opt/ostrelay/ops/database.py", "restore", "/data/snapshot.sqlite", "/data/restored.sqlite"])
            with env.open('a') as handle: handle.write("OSTRELAY_TEST_DB_PATH=/data/restored.sqlite\n")
            run(compose + ["up", "-d", "--wait", "--wait-timeout", "60", "relay"])
            restored_address = run(compose + ["port", "relay", "8080"]).strip()
            response = httpx.get(f"http://{restored_address}/admin/timers", headers={"Authorization": f"Bearer {admin_token}"}, trust_env=False)
            assert response.status_code == 200 and len(response.json()) == 3
            report["backup_restore_and_restart"] = True
            report["passed"] = True
        except Exception as exc:
            report["failure"] = str(exc)
            print(run(compose + ["logs", "--no-color", "--tail", "60"]), file=sys.stderr)
            raise
        finally:
            # Every resource named here belongs to the fresh random project.
            run(compose + ["down", "--volumes", "--remove-orphans"])
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
