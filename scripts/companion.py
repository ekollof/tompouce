"""Loopback browser companion for an existing Tompouce MCP session."""
import argparse
import hmac
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import shutil
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote
from sound_import import SoundStore, MAX_FILE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mcp-port", type=int, default=18789)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--open", action="store_true", help="open a Chromium tab")
    parser.add_argument("--parent-pid", type=int, help="exit if the launching engine exits")
    args = parser.parse_args()
    if not 1 <= args.mcp_port <= 65535 or not 0 <= args.port <= 65535:
        parser.error("invalid port")
    if args.parent_pid is not None and args.parent_pid < 1:
        parser.error("invalid parent pid")
    token = secrets.token_urlsafe(32)
    style_nonce = secrets.token_urlsafe(24)
    assets = Path(__file__).resolve().parent.parent / "web"
    tools = {"music_get_status", "music_get_code", "music_get_console", "music_debug",
             "music_health", "music_play", "music_stop", "music_evaluate",
             "music_set_bpm", "music_set_code"}
    tools.update({"music_sounds", "music_reload_sounds"})

    def call_mcp(name, arguments):
        message = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                   "params": {"name": name, "arguments": arguments}}
        with socket.create_connection(("127.0.0.1", args.mcp_port), timeout=5) as connection:
            connection.settimeout(10)
            connection.sendall((json.dumps(message) + "\n").encode())
            with connection.makefile("rb") as reply:
                raw = reply.readline(4 * 1024 * 1024 + 1)
        if not raw.endswith(b"\n") or len(raw) > 4 * 1024 * 1024:
            raise ValueError("invalid MCP response size")
        return json.loads(raw)

    class Handler(BaseHTTPRequestHandler):
        def setup(self):
            super().setup()
            self.connection.settimeout(10)

        def send(self, status, content, kind="application/json"):
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(content)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", f"default-src 'self'; style-src 'self' 'nonce-{style_nonce}'; frame-ancestors 'none'; object-src 'none'")
            self.end_headers()
            self.wfile.write(content)

        def host_ok(self):
            return self.headers.get("Host") == f"127.0.0.1:{self.server.server_port}"

        def do_GET(self):
            if not self.host_ok():
                self.send(403, b'{"error":"host refused"}')
                return
            files = {"/": ("index.html", "text/html; charset=utf-8"),
                     "/companion.js": ("companion.js", "text/javascript"),
                     "/companion.css": ("companion.css", "text/css")}
            files["/editor.js"] = ("dist/editor.js", "text/javascript")
            files["/sounds.js"] = ("sounds.js", "text/javascript")
            if self.path not in files:
                self.send(404, b'{}')
                return
            name, kind = files[self.path]
            content = (assets / name).read_bytes()
            if name == "index.html":
                content = content.replace(b"COMPANION_TOKEN", token.encode())
                content = content.replace(b"EDITOR_STYLE_NONCE", style_nonce.encode())
            self.send(200, content, kind)

        def do_POST(self):
            origin = f"http://127.0.0.1:{self.server.server_port}"
            if (not self.host_ok() or self.path not in {"/api/tool", "/api/import"} or
                    self.headers.get("Origin") != origin or
                    not hmac.compare_digest(self.headers.get("X-Companion-Token", ""), token)):
                self.send(403, b'{"error":"request refused"}')
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                maximum = MAX_FILE if self.path == "/api/import" else 2 * 1024 * 1024
                if not 0 < length <= maximum:
                    raise ValueError("request too large or empty")
                payload = self.rfile.read(length)
                if len(payload) != length:
                    raise ValueError("incomplete request")
                if self.path == "/api/import":
                    if self.headers.get("Content-Type") != "application/octet-stream":
                        raise ValueError("audio upload needs application/octet-stream")
                    name = unquote(self.headers.get("X-Sound-Name", ""), errors="strict")
                    label = unquote(self.headers.get("X-Sound-File", ""), errors="strict")
                    with SoundStore() as store:
                        summary = store.import_file(name, label, payload)
                    reloaded = call_mcp("music_reload_sounds", {})
                    if reloaded.get("error") or reloaded.get("result", {}).get("isError"):
                        raise ValueError("files saved, but engine could not reload sounds; restart onto the current build")
                    self.send(200, json.dumps(summary).encode())
                    return
                request = json.loads(payload)
                name, arguments = request.get("name"), request.get("arguments", {})
                if name not in tools or not isinstance(arguments, dict):
                    raise ValueError("unsupported tool")
                response = call_mcp(name, arguments)
                self.send(200, json.dumps(response).encode())
            except (OSError, ValueError, TypeError, AttributeError) as error:
                self.send(502, json.dumps({"error": str(error)}).encode())

        def log_message(self, *_):
            pass

    class Server(ThreadingHTTPServer):
        def service_actions(self):
            if args.parent_pid is not None and os.getppid() != args.parent_pid:
                raise SystemExit(0)

    server = Server(("127.0.0.1", args.port), Handler)
    server.daemon_threads = True
    url = f"http://127.0.0.1:{server.server_port}/"
    print(f"Tompouce companion: {url} (MCP 127.0.0.1:{args.mcp_port})", flush=True)
    if args.open:
        try:
            browser = next((path for name in ('chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable') if (path := shutil.which(name))), 'chromium')
            subprocess.Popen([browser, url], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)
        except OSError as error:
            print(f"Could not open Chromium: {error}. Open {url} manually.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
