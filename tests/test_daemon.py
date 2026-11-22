"""Isolated resident daemon over a named Unix socket; no TCP port or user state."""
import json
import os
import select
import secrets
import signal
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

BIN = os.environ.get("TOMPOUCE_TEST_BINARY", "./build/tompouce")


class DaemonTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.sock = self.root / "nested" / "mcp.sock"  # parent is created by daemon
        self.token = secrets.token_hex(32)

    def tearDown(self):
        process = getattr(self, "process", None)
        if process:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            for stream in (process.stdout, process.stderr):
                if stream:
                    stream.close()
        self.temp.cleanup()

    def start(self, code='note(60).s("sine").gain(.2)'):
        env = dict(os.environ, TOMPOUCE_MCP_TOKEN=self.token,
                   XDG_CONFIG_HOME=str(self.root / "config"),
                   XDG_CACHE_HOME=str(self.root / "cache"))
        self.process = subprocess.Popen(
            [BIN, "--mcp-socket", str(self.sock), "--bpm", "120", "--code", code],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        deadline = time.time() + 15
        while time.time() < deadline:
            if self.sock.exists():
                return
            if self.process.poll() is not None:
                self.fail(f"daemon exited: {self.process.communicate()[1][:400]}")
            time.sleep(0.1)
        self.fail("daemon socket did not appear")

    def rpc(self, payload, auth=True, raw=None):
        message = raw if raw is not None else dict(
            jsonrpc="2.0", id=1, auth=self.token if auth else "00" * 32,
            method="tools/call", params=payload)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(5)
            client.connect(str(self.sock))
            client.sendall((json.dumps(message) + "\n").encode())
            chunks = []
            while True:
                chunk = client.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"\n" in chunk:
                    break
        return json.loads(b"".join(chunks).split(b"\n", 1)[0])

    def test_socket_auth_and_lifecycle(self):
        self.start()
        self.assertEqual(oct(self.sock.stat().st_mode & 0o777), "0o600")
        reply = self.rpc({"name": "music_get_status"})
        self.assertIn("playing=yes", reply["result"]["content"][0]["text"])
        # unauthenticated frames are refused (no response)
        with self.assertRaises((ValueError, socket.timeout)):
            self.rpc({"name": "music_get_status"}, auth=False)
        # a second instance can use a different path without a port conflict
        other = self.root / "other" / "mcp.sock"
        env = dict(os.environ, TOMPOUCE_MCP_TOKEN=self.token,
                   XDG_CONFIG_HOME=str(self.root / "config2"),
                   XDG_CACHE_HOME=str(self.root / "cache2"))
        second = subprocess.Popen(
            [BIN, "--mcp-socket", str(other), "--bpm", "90", "--code", "note(64)"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        try:
            deadline = time.time() + 15
            while time.time() < deadline and not other.exists():
                time.sleep(0.1)
            self.assertTrue(other.exists())
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(5)
                client.connect(str(other))
                message = dict(jsonrpc="2.0", id=1, auth=self.token,
                               method="tools/call",
                               params={"name": "music_get_status"})
                client.sendall((json.dumps(message) + "\n").encode())
                reply = json.loads(client.recv(65536).split(b"\n", 1)[0])
            self.assertIn("bpm=90", reply["result"]["content"][0]["text"])
        finally:
            second.send_signal(signal.SIGTERM)
            try:
                second.wait(timeout=5)
            except subprocess.TimeoutExpired:
                second.kill()
                second.wait()

    def test_sigterm_removes_socket(self):
        self.start()
        self.process.send_signal(signal.SIGTERM)
        self.process.wait(timeout=5)
        self.assertFalse(self.sock.exists())


if __name__ == "__main__":
    unittest.main()
