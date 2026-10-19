#!/usr/bin/env python3
"""Read-only SSE bench qualification. Does not start/release/reconfigure a timer."""

import argparse
import http.client
import json
import time
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Timer origin, e.g. http://192.168.4.1")
    parser.add_argument("--skip-idle-refresh", action="store_true")
    args = parser.parse_args()
    origin = urlsplit(args.url)
    if origin.scheme != "http" or not origin.hostname or origin.path not in {"", "/"}:
        parser.error("Use the timer's plain HTTP origin without a path")

    def connection(timeout=4):
        return http.client.HTTPConnection(origin.hostname, origin.port or 80, timeout=timeout)

    def get_json(path):
        client = connection()
        try:
            start = time.monotonic()
            client.request("GET", path)
            response = client.getresponse()
            body = json.loads(response.read())
            assert response.status == 200, (response.status, body)
            return body, time.monotonic() - start
        finally:
            client.close()

    def chunk_bytes(response):
        while True:
            header = response.fp.readline(65)
            assert header.endswith(b"\r\n") and len(header) <= 64, "Malformed chunk header"
            size = int(header[:-2].split(b";", 1)[0], 16)
            assert 0 < size <= 4096, "Ended or excessive SSE chunk"
            payload = response.fp.read(size)
            assert len(payload) == size, "Truncated chunk"
            assert response.fp.read(2) == b"\r\n", "Missing chunk terminator"
            yield from payload

    def snapshot(stream, deadline_seconds):
        deadline = time.monotonic() + deadline_seconds
        frame = bytearray()
        while time.monotonic() < deadline:
            frame.append(next(stream))
            assert len(frame) <= 4096, "Unbounded or malformed SSE frame"
            if frame.endswith(b"\n\n"):
                lines = frame.decode("utf-8").splitlines()
                frame.clear()
                if "event: status" not in lines:
                    continue  # Initial retry directive is its own SSE block.
                ids = [line[4:] for line in lines if line.startswith("id: ")]
                data = [line[6:] for line in lines if line.startswith("data: ")]
                assert len(ids) == len(data) == 1, "Missing SSE ID or data"
                status = json.loads(data[0])
                assert status["state"] in {"open", "running", "paused"}
                assert isinstance(status["emergency_input_active"], bool)
                boot, event, revision = ids[0].split(":")
                assert int(boot, 16) == int(status["boot_id"], 16)
                assert int(event) == status["latest_event_id"]
                assert int(revision) == status["state_revision"]
                return status
        raise AssertionError("No complete status snapshot before deadline")

    baseline, _ = get_json("/api/v1/system/diagnostics")
    assert baseline["sse_clients"] == 0, "Close all timer UI/SSE pages before this test"
    limit = baseline["sse_client_limit"]
    chip = get_json("/api/v1/status")[0].get("chip_family")
    expected = 2 if chip == "ESP8266" else 3  # V1 two live viewers, V2 three
    assert limit == expected, f"Unexpected SSE limit {limit} for {chip}"
    clients = []
    responses = []
    streams = []
    try:
        for index in range(limit):
            client = connection()
            clients.append(client)
            client.request("GET", "/api/v1/events/stream", headers={"Accept": "text/event-stream"})
            response = client.getresponse()
            responses.append(response)
            assert response.status == 200, f"Client {index + 1}: HTTP {response.status}"
            assert response.getheader("Content-Type", "").startswith("text/event-stream")
            assert response.getheader("Transfer-Encoding", "").lower() == "chunked"
            assert response.getheader("Cache-Control") == "no-store"
            stream = chunk_bytes(response)
            streams.append(stream)
            state = snapshot(stream, 5)
            client.sock.settimeout(20)
            print(f"Client {index + 1}: headers/chunks/first status OK; boot={state['boot_id']}", flush=True)

        extra = connection()
        try:
            extra.request("GET", "/api/v1/events/stream")
            response = extra.getresponse()
            assert response.status == 503
            assert json.loads(response.read())["code"] == "sse_client_limit"
            print("Fourth client: bounded 503 rejection OK", flush=True)
        finally:
            extra.close()

        latencies = []
        for _ in range(5):
            _, elapsed = get_json("/api/v1/status")
            latencies.append(elapsed)
        print(f"REST with three streams: five reads OK; max={max(latencies):.3f}s", flush=True)
        if not args.skip_idle_refresh:
            snapshot(streams[0], 20)
            print("Next full status received within 20s (idle refresh or state change)", flush=True)
    finally:
        for response in responses:
            response.close()
        for client in clients:
            client.close()

    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        diagnostics, _ = get_json("/api/v1/system/diagnostics")
        if diagnostics["sse_clients"] == 0:
            print("Cleanup: zero SSE clients; read-only transport qualification passed", flush=True)
            return
        time.sleep(0.1)
    raise AssertionError("SSE slots did not return to zero within five seconds")


if __name__ == "__main__":
    main()
