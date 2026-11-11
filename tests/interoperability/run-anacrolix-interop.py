#!/usr/bin/env python3
"""Verify Transmission <-> anacrolix over WebRTC with a local WS tracker only.

No public tracker, STUN/TURN server, DHT, PEX, webseed, TCP or uTP peer
transport is configured. Each test uses its own daemon, tracker, peer and data.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


FILE_NAME = "interop.bin"
PAYLOAD_SIZE = 256 * 1024
CASE_TIMEOUT = 180


def bencode(value):
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, (bytes, str)):
        encoded = value.encode() if isinstance(value, str) else value
        return str(len(encoded)).encode() + b":" + encoded
    if isinstance(value, list):
        return b"l" + b"".join(map(bencode, value)) + b"e"
    if isinstance(value, dict):
        return b"d" + b"".join(
            bencode(k) + bencode(v)
            for k, v in sorted(value.items(), key=lambda item: item[0])
        ) + b"e"
    raise TypeError(type(value))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def make_fixture(folder, tracker_port):
    folder.mkdir(parents=True, exist_ok=True)
    payload = bytes((i * 73 + (i // 257) * 19) % 256 for i in range(PAYLOAD_SIZE))
    piece_length = 16 * 1024
    info = {
        b"length": len(payload),
        b"name": FILE_NAME,
        b"piece length": piece_length,
        b"pieces": b"".join(
            hashlib.sha1(payload[i:i + piece_length]).digest()
            for i in range(0, len(payload), piece_length)
        ),
    }
    torrent = folder / "fixture.torrent"
    torrent.write_bytes(bencode({
        b"announce": f"ws://127.0.0.1:{tracker_port}/announce",
        b"info": info,
    }))
    return torrent, payload


def post_rpc(port, method, arguments=None):
    url = f"http://127.0.0.1:{port}/transmission/rpc"
    body = json.dumps({"method": method, "arguments": arguments or {}}).encode()
    headers = {"Content-Type": "application/json"}
    for _ in range(2):
        req = urllib.request.Request(url, data=body, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=3) as response:
                answer = json.load(response)
                if answer.get("result") != "success":
                    raise RuntimeError(f"Transmission RPC {method}: {answer}")
                return answer.get("arguments", {})
        except urllib.error.HTTPError as exc:
            if exc.code != 409:
                raise
            headers["X-Transmission-Session-Id"] = exc.headers["X-Transmission-Session-Id"]
    raise RuntimeError("Transmission RPC could not negotiate a session ID")


def download_statistics(port, torrent_id):
    args = {"ids": [torrent_id], "fields": [
        "id", "name", "percentDone", "status", "peersConnected",
        "uploadedEver", "downloadedEver", "errorString",
    ]}
    torrents = post_rpc(port, "torrent-get", args).get("torrents", [])
    if len(torrents) != 1:
        raise RuntimeError(f"Torrent not found: {torrents!r}")
    return torrents[0]


def tracker_statistics(port):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/status", timeout=2) as response:
        return json.load(response)


class ManagedProcess:
    def __init__(self, argv, log_path):
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_path = log_path
        self.log = log_path.open("wb")
        self.proc = subprocess.Popen(argv, stdout=self.log, stderr=subprocess.STDOUT)

    def check(self):
        result = self.proc.poll()
        if result is not None:
            raise RuntimeError(f"Process exited early with {result}: {self.log_path}")

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.log.close()


def wait_for(description, fn, deadline, processes):
    last_error = None
    while time.monotonic() < deadline:
        for proc in processes:
            proc.check()
        try:
            result = fn()
            if result:
                return result
        except (OSError, TimeoutError, urllib.error.URLError, KeyError) as exc:
            last_error = exc
        time.sleep(0.4)
    raise TimeoutError(f"Timed out waiting for {description}. Last error: {last_error}")


def assert_file(path, payload):
    if not path.is_file():
        return False
    if path.stat().st_size != len(payload):
        return False
    return hashlib.sha256(path.read_bytes()).digest() == hashlib.sha256(payload).digest()


def run_case(case_dir, daemon, peer, direction):
    print(f"CASE {direction}: starting local tracker and independent peers", flush=True)
    tracker_port, rpc_port = free_port(), free_port()
    while tracker_port == rpc_port:
        rpc_port = free_port()

    torrent_file, payload = make_fixture(case_dir, tracker_port)
    go_data = case_dir / "go-data"
    tr_data = case_dir / "transmission-data"
    go_data.mkdir()
    tr_data.mkdir()
    if direction == "go-to-transmission":
        (go_data / FILE_NAME).write_bytes(payload)
    else:
        (tr_data / FILE_NAME).write_bytes(payload)

    config_dir = case_dir / "transmission-config"
    config_dir.mkdir()
    settings = {
        "rpc-enabled": True, "rpc-port": rpc_port, "rpc-bind-address": "127.0.0.1",
        "rpc-whitelist-enabled": False, "rpc-host-whitelist-enabled": False,
        "rpc-authentication-required": False,
        "webtorrent_enabled": True, "webtorrent_ice_servers": [],
        "ip_endpoints_ipv4": [], "ip_endpoints_ipv6": [],
        "tcp-enabled": False, "utp-enabled": False,
        "dht-enabled": False, "pex-enabled": False, "lpd-enabled": False,
        "port-forwarding-enabled": False,
        "download-queue-enabled": False, "seed-queue-enabled": False,
        "download-dir": str(tr_data),
        "rename-partial-files": False,
    }
    (config_dir / "settings.json").write_text(json.dumps(settings))

    processes = []
    deadline = time.monotonic() + CASE_TIMEOUT
    try:
        tracker = ManagedProcess(
            [str(peer), "-mode", "tracker", "-address", f"127.0.0.1:{tracker_port}"],
            case_dir / "tracker.log",
        )
        processes.append(tracker)
        wait_for("local tracker", lambda: tracker_statistics(tracker_port), deadline, processes)

        transmission = ManagedProcess(
            [str(daemon), "--foreground", "--config-dir", str(config_dir)],
            case_dir / "transmission.log",
        )
        processes.append(transmission)
        session = wait_for("Transmission RPC", lambda: post_rpc(rpc_port, "session-get"), deadline, processes)
        for transport in ("tcp-enabled", "utp-enabled", "dht-enabled", "pex-enabled", "lpd-enabled"):
            if session.get(transport) is not False:
                raise RuntimeError(f"Expected {transport}=false; refusing fallback transport test")
        # WebTorrent is not currently exposed by session-get RPC; verified RTC
        # signaling and the no-fallback data transfer prove it was enabled.

        def start_go(mode):
            go = ManagedProcess([
                str(peer), "-mode", mode,
                "-torrent", str(torrent_file), "-data", str(go_data),
                "-size", str(len(payload)),
            ], case_dir / "anacrolix.log")
            processes.append(go)
            return go

        def add_transmission():
            result = post_rpc(rpc_port, "torrent-add", {
                "metainfo": base64.b64encode(torrent_file.read_bytes()).decode(),
                "download-dir": str(tr_data),
                "paused": True,
            })
            added = result.get("torrent-added", result.get("torrent-duplicate"))
            if not added or not added.get("id"):
                raise RuntimeError(f"Transmission refused torrent: {result}")
            return added["id"]

        if direction == "go-to-transmission":
            go = start_go("seed")
            # A zero completed-byte count before verification cannot seed a test.
            wait_for("anacrolix seeding readiness",
                     lambda: "READY seed" in go.log_path.read_text(errors="replace"),
                     deadline, processes)
            tid = add_transmission()
            post_rpc(rpc_port, "torrent-start", {"ids": [tid]})
            wait_for("Transmission verified download",
                     lambda: (download_statistics(rpc_port, tid)["percentDone"] >= 1
                              and assert_file(tr_data / FILE_NAME, payload)),
                     deadline, processes)
        else:
            tid = add_transmission()
            post_rpc(rpc_port, "torrent-verify", {"ids": [tid]})
            wait_for("Transmission seed verification",
                     lambda: download_statistics(rpc_port, tid)["percentDone"] >= 1,
                     deadline, processes)
            post_rpc(rpc_port, "torrent-start", {"ids": [tid]})
            go = start_go("download")
            wait_for("anacrolix verified download",
                     lambda: (go.proc.poll() == 0 and
                              assert_file(go_data / FILE_NAME, payload)),
                     deadline, [transmission, tracker])

        stats = tracker_statistics(tracker_port)
        torrent_stats = download_statistics(rpc_port, tid)
        print(f"RESULT {direction}: tracker={stats}, transmission={torrent_stats}", flush=True)
        if stats["offers"] < 1 or stats["answers"] < 1:
            raise AssertionError("No successful bidirectional RTC signaling; test cannot pass")
        path = tr_data / FILE_NAME if direction == "go-to-transmission" else go_data / FILE_NAME
        if not assert_file(path, payload):
            raise AssertionError(f"Downloaded file content mismatch: {path}")
        print(f"PASS {direction}: {len(payload)} verified bytes via WebRTC", flush=True)
    except Exception:
        try:
            print(f"TRACKER STATE: {tracker_statistics(tracker_port)}", flush=True)
        except Exception as exc:
            print(f"TRACKER STATE unavailable: {exc}", flush=True)
        if "tid" in locals():
            try:
                print(f"TORRENT STATE: {download_statistics(rpc_port, tid)}", flush=True)
            except Exception as exc:
                print(f"TORRENT STATE unavailable: {exc}", flush=True)
        for process in processes:
            print(f"--- {process.log_path} (last 80 lines) ---", flush=True)
            if process.log_path.exists():
                print("\n".join(process.log_path.read_text(errors="replace").splitlines()[-80:]),
                      flush=True)
        raise
    finally:
        for process in reversed(processes):
            process.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", type=Path, required=True)
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument("--workdir", type=Path, required=True)
    args = parser.parse_args()
    daemon, peer = args.daemon.resolve(), args.peer.resolve()
    args.workdir.mkdir(parents=True, exist_ok=True)
    for name in ("go-to-transmission", "transmission-to-go"):
        run_case(args.workdir / name, daemon, peer, name)
    print("PASS Transmission <-> anacrolix: two independent verified WebRTC transfers", flush=True)


if __name__ == "__main__":
    main()
