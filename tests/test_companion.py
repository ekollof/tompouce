"""Exercise the companion bridge against a disposable MCP stub, never live audio."""
import json
import os
import tempfile
import re
import select
import socketserver
import subprocess
import threading
import unittest
import urllib.error
import urllib.request


class HydraTest(unittest.TestCase):
    """The /hydra bridge exposes one bounded analysis tool and nothing else."""
    @classmethod
    def setUpClass(cls):
        cls.calls = []

        class MCP(socketserver.StreamRequestHandler):
            def handle(self):
                message = json.loads(self.rfile.readline())
                assert message.get("auth") == "a" * 64
                cls.calls.append(message["params"]["name"])
                reply = {"jsonrpc": "2.0", "id": message["id"], "result": {
                    "content": [{"type": "text", "text": '{"scope":[0,0.1],"bpm":120,"playing":1}'}]}}
                self.wfile.write((json.dumps(reply) + "\n").encode())

        cls.mcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), MCP)
        cls.worker = threading.Thread(target=cls.mcp.serve_forever, daemon=True)
        cls.worker.start()
        cls.storage = tempfile.TemporaryDirectory()
        cls.process = subprocess.Popen(["python3", "scripts/companion.py", "--hydra", "--mcp-port",
                                        str(cls.mcp.server_address[1])], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True,
                                       env=dict(os.environ, XDG_CONFIG_HOME=cls.storage.name,
                                                TOMPOUCE_MCP_TOKEN="a" * 64))
        if not select.select([cls.process.stdout], [], [], 5)[0]:
            cls.process.kill()
            raise AssertionError("hydra startup timed out")
        line = cls.process.stdout.readline()
        assert "Tompouce hydra" in line
        cls.url = re.search(r"http://127\.0\.0\.1:\d+", line).group()
        html = urllib.request.urlopen(cls.url).read().decode()
        assert "music_audio_scope" in html and "music_audio_spectrum" in html
        assert "COMPANION_TOKEN" not in html
        assert 'sandbox="allow-scripts"' in html and 'src="/hydra-frame.html"' in html
        assert 'src="/hydra.js"' not in html
        frame = urllib.request.urlopen(cls.url + "/hydra-frame.html").read().decode()
        assert 'src="/hydra.js"' in frame and 'src="/patterns.js"' in frame
        assert "COMPANION_TOKEN" not in frame
        assert "const token" not in frame and "X-Companion-Token" not in frame
        script = urllib.request.urlopen(cls.url + "/hydra.js").read().decode()
        for name in ("osc", "shape", "solid", "noise", "src", "rotate", "scale", "repeatX", "out"):
            assert name in script
        patterns = urllib.request.urlopen(cls.url + "/patterns.js").read().decode()
        assert "TompouceHydraPatterns" in patterns and "strudel" in patterns.lower()
        cls.token = re.search(r"const token = '([^']+)'", html).group(1)

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.communicate(timeout=5)
        cls.mcp.shutdown()
        cls.mcp.server_close()
        cls.worker.join(timeout=5)
        cls.storage.cleanup()

    def post(self, path, name, token=None, origin=None):
        return urllib.request.urlopen(urllib.request.Request(self.url + path,
            json.dumps({"name": name, "arguments": {"limit": 32}}).encode(), headers={
                "Origin": origin or self.url, "X-Companion-Token": token or self.token,
                "Content-Type": "application/json"}), timeout=5)

    def test_only_analysis_tool(self):
        reply = json.loads(self.post("/api/tool", "music_audio_scope").read())
        assert "scope" in reply["result"]["content"][0]["text"]
        spectrum = json.loads(self.post("/api/tool", "music_audio_spectrum").read())
        assert spectrum["result"] is not None
        assert self.calls == ["music_audio_scope", "music_audio_spectrum"]
        for name in ("music_evaluate",):
            with self.assertRaises(urllib.error.HTTPError):
                self.post("/api/tool", name)
        with self.assertRaises(urllib.error.HTTPError):
            self.post("/api/import", "music_audio_scope")
        with self.assertRaises(urllib.error.HTTPError):
            self.post("/api/tool", "music_audio_scope", token="wrong")
        with self.assertRaises(urllib.error.HTTPError):
            self.post("/api/tool", "music_audio_scope", origin="http://evil.example")

    def test_inline_page_script_is_allowed(self):
        html = urllib.request.urlopen(self.url).read().decode()
        nonce = re.search(r'<style nonce="([^"]+)"', html).group(1)
        with urllib.request.urlopen(self.url) as response:
            csp = response.headers["Content-Security-Policy"]
            permissions = response.headers["Permissions-Policy"]
        assert f"script-src 'self' 'nonce-{nonce}'" in csp
        assert "script-src" in csp and "'unsafe-inline'" not in csp
        # The parent shell never evaluates code: eval lives only in the frame.
        assert "'unsafe-eval'" not in csp
        assert "microphone=()" in permissions and "camera=()" in permissions
        with urllib.request.urlopen(self.url + "/hydra-frame.html") as response:
            frame = response.read().decode()
            frame_csp = response.headers["Content-Security-Policy"]
        frame_nonce = re.search(r'<style nonce="([^"]+)"', frame).group(1)
        assert f"script-src 'self' 'nonce-{frame_nonce}' 'unsafe-eval'" in frame_csp
        assert "connect-src 'none'" in frame_csp
        assert "frame-ancestors 'self'" in frame_csp


class CompanionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.calls = []

        class MCP(socketserver.StreamRequestHandler):
            def handle(self):
                message = json.loads(self.rfile.readline())
                assert message.get("auth") == "a" * 64
                cls.calls.append(message["params"])
                reply = {"jsonrpc": "2.0", "id": message["id"], "result": {
                    "content": [{"type": "text", "text": "stub response"}]}}
                self.wfile.write((json.dumps(reply) + "\n").encode())

        cls.mcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), MCP)
        cls.worker = threading.Thread(target=cls.mcp.serve_forever, daemon=True)
        cls.worker.start()
        cls.storage = tempfile.TemporaryDirectory()
        cls.process = subprocess.Popen(["python3", "scripts/companion.py", "--mcp-port",
                                        str(cls.mcp.server_address[1])], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True,
                                       env=dict(os.environ, XDG_CONFIG_HOME=cls.storage.name, TOMPOUCE_MCP_TOKEN="a"*64))
        if not select.select([cls.process.stdout], [], [], 5)[0]:
            cls.process.kill()
            raise AssertionError("companion startup timed out")
        line = cls.process.stdout.readline()
        cls.url = re.search(r"http://127\.0\.0\.1:\d+", line).group()
        html = urllib.request.urlopen(cls.url).read().decode()
        cls.token = re.search(r'name="companion-token" content="([^"]+)"', html).group(1)

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.communicate(timeout=5)
        cls.mcp.shutdown()
        cls.mcp.server_close()
        cls.worker.join(timeout=5)
        cls.storage.cleanup()

    def post(self, name, arguments=None, token=None, origin=None):
        return urllib.request.urlopen(urllib.request.Request(self.url + "/api/tool",
            json.dumps({"name": name, "arguments": arguments or {}}).encode(), headers={
                "Origin": origin or self.url, "X-Companion-Token": token or self.token,
                "Content-Type": "application/json"}), timeout=5)

    def test_read_and_control_tools(self):
        for name, args in [("music_get_status", {}), ("music_play", {}), ("music_stop", {}),
                           ("music_set_bpm", {"bpm": 133}),
                           ("music_set_code", {"code": "note('c4')\n// <script>"})]:
            with self.post(name, args) as response:
                self.assertEqual(json.load(response)["result"]["content"][0]["text"], "stub response")
            self.assertEqual(self.calls[-1], {"name": name, "arguments": args})

    def test_refuse_cross_origin_and_missing_auth(self):
        before = len(self.calls)
        for headers in [{}, {"Origin": "https://example.com", "X-Companion-Token": self.token},
                        {"Origin": self.url, "X-Companion-Token": "wrong"}]:
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(urllib.request.Request(self.url + "/api/tool",
                    b'{"name":"music_play"}', headers=headers))
            self.assertEqual(error.exception.code, 403)
            error.exception.close()
        self.assertEqual(len(self.calls), before)

    def test_engine_capability_is_private_and_pipe_launch_works(self):
        env = {key: value for key, value in os.environ.items() if key != "TOMPOUCE_MCP_TOKEN"}
        process = subprocess.Popen(["python3", "scripts/companion.py", "--mcp-port",
                                    str(self.mcp.server_address[1]), "--mcp-token-stdin"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, env=env)
        try:
            process.stdin.write("a" * 64)
            process.stdin.close()
            process.stdin = None
            self.assertTrue(select.select([process.stdout], [], [], 5)[0])
            url = re.search(r"http://127\.0\.0\.1:\d+", process.stdout.readline()).group()
            with urllib.request.urlopen(url) as response:
                html = response.read().decode()
            self.assertNotIn("a" * 64, html)
            web_token = re.search(r'name="companion-token" content="([^"]+)"', html).group(1)
            request = urllib.request.Request(url + "/api/tool",
                json.dumps({"name": "music_get_code"}).encode(), headers={
                    "Origin": url, "X-Companion-Token": web_token, "Content-Type": "application/json"})
            with urllib.request.urlopen(request, timeout=5) as response:
                self.assertIn("stub response", response.read().decode())
        finally:
            process.terminate()
            process.communicate(timeout=5)

    def test_missing_engine_capability_refuses_startup(self):
        env = {key: value for key, value in os.environ.items() if key != "TOMPOUCE_MCP_TOKEN"}
        result = subprocess.run(["python3", "scripts/companion.py"], env=env,
                                capture_output=True, text=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("http://", result.stdout)

    def test_refuse_unknown_tool_and_paths(self):
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.post("music_ai", {"prompt": "do not send"})
        self.assertEqual(error.exception.code, 502)
        error.exception.close()
        for suffix in ["/../AGENTS.md", "/api/token", "/web/index.html"]:
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(self.url + suffix)
            self.assertEqual(error.exception.code, 404)
            error.exception.close()

    def test_assets_and_host_validation(self):
        html = urllib.request.urlopen(self.url).read().decode()
        nonce = re.search(r'name="editor-style-nonce" content="([^"]+)"', html).group(1)
        for suffix in ["/", "/companion.js", "/companion.css", "/editor.js", "/sounds.js"]:
            with urllib.request.urlopen(self.url + suffix) as response:
                self.assertIn("frame-ancestors 'none'", response.headers["Content-Security-Policy"])
                self.assertIn(f"'nonce-{nonce}'", response.headers["Content-Security-Policy"])
                self.assertNotIn("'unsafe-inline'", response.headers["Content-Security-Policy"])
                self.assertEqual(response.headers["Cache-Control"], "no-store")
                self.assertTrue(response.read())
        with self.assertRaises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(urllib.request.Request(self.url, headers={"Host": "example.com"}))
        self.assertEqual(error.exception.code, 403)
        error.exception.close()


    def test_import_saved_and_reloaded(self):
        from test_sound_import import audio
        request = urllib.request.Request(self.url + '/api/import', audio(), headers={
            'Origin': self.url, 'X-Companion-Token': self.token,
            'X-Sound-Name': 'testdrums', 'X-Sound-File': 'kick.wav',
            'Content-Type': 'application/octet-stream'})
        with urllib.request.urlopen(request) as response:
            self.assertEqual(response.status, 200)
        with open(self.storage.name + '/tompouce/sounds/index.json') as file:
            self.assertEqual(len(json.load(file)['sounds']['testdrums']), 1)
        self.assertEqual(self.calls[-1]['name'], 'music_reload_sounds')


if __name__ == "__main__":
    unittest.main()
