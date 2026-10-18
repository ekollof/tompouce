"""Owned silent streams and temporary ALSA PCMs; never change desktop settings."""
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import unittest

PROBE = './tests/probe_audio_output'
RUNTIME = Path(os.environ.get('XDG_RUNTIME_DIR', '/nonexistent'))
BINARY = os.environ.get('TOMPOUCE_TEST_BINARY', './tompouce')


class AudioBackendsTest(unittest.TestCase):
    def probe(self, expected, environment=None, binary=PROBE):
        result = subprocess.run([binary, expected], env=environment or os.environ,
                                capture_output=True, text=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-2000:])
        self.assertEqual(result.stdout.count('backend=' + expected), 2)
        print(result.stdout.strip())

    @unittest.skipUnless((RUNTIME / 'pipewire-0').exists(), 'no native PipeWire socket')
    def test_pipewire_first(self):
        self.probe('PipeWire')

    @unittest.skipUnless((RUNTIME / 'pipewire-0').exists() and shutil.which('pw-dump'),
                         'requires a native PipeWire socket and pw-dump')
    def test_pipewire_automatically_routes_stereo(self):
        # Callback activity alone cannot establish that a stream reaches speakers.
        # Inspect fresh owned streams without creating links or changing defaults.
        for attempt in range(2):
            with self.subTest(attempt=attempt), subprocess.Popen(
                    [PROBE, 'PipeWire', '--inspect'], stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as child:
                try:
                    self.assertTrue(select.select([child.stdout], [], [], 12)[0], 'probe startup timed out')
                    self.assertEqual(child.stdout.readline().strip(), 'ready backend=PipeWire')
                    graph = json.loads(subprocess.check_output(['pw-dump'], text=True, timeout=5))
                    clients = {item['id'] for item in graph if item['type'].endswith(':Client')
                               and str(item['info']['props'].get('application.process.id')) == str(child.pid)}
                    nodes = [item for item in graph if item['type'].endswith(':Node')
                             and item['info']['props'].get('client.id') in clients]
                    self.assertEqual(len(nodes), 1)
                    node = nodes[0]
                    self.assertEqual(node['info']['props'].get('media.class'), 'Stream/Output/Audio')
                    self.assertIn(node['info']['props'].get('node.autoconnect'), (True, 'true'))
                    links = [item['info'] for item in graph if item['type'].endswith(':Link')
                             and item['info']['output-node-id'] == node['id']]
                    self.assertEqual(len(links), 2, 'stereo stream must connect without manual routing')
                    self.assertTrue(all(link['state'] == 'active' for link in links))
                    self.assertEqual(len({link['output-port-id'] for link in links}), 2)
                    sinks = {item['id']: item['info']['props'].get('media.class') for item in graph
                             if item['type'].endswith(':Node')}
                    self.assertTrue(all(sinks.get(link['input-node-id']) == 'Audio/Sink' for link in links))
                    self.assertEqual(len({link['input-node-id'] for link in links}), 1)
                finally:
                    try:
                        output, errors = child.communicate('\n', timeout=12)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.communicate()
                        raise
                self.assertEqual(child.returncode, 0, output + errors[-2000:])
                self.assertIn('backend=PipeWire active=1', output)

    @unittest.skipUnless((RUNTIME / 'pipewire-0').exists(), 'no native PipeWire socket')
    def test_mcp_reports_backend(self):
        with tempfile.TemporaryDirectory(prefix='tompouce-audio-mcp-') as root:
            environment = dict(os.environ, XDG_CONFIG_HOME=root, XDG_CACHE_HOME=root, TOMPOUCE_BANKS=root)
            requests = [dict(jsonrpc='2.0', id=1, method='initialize'),
                        dict(jsonrpc='2.0', id=2, method='tools/call', params={'name': 'music_health', 'arguments': {}})]
            result = subprocess.run([BINARY, '--mcp'], input=''.join(json.dumps(item) + '\n' for item in requests),
                                    capture_output=True, text=True, env=environment, timeout=12)
            self.assertEqual(result.returncode, 0, result.stderr[-2000:])
            replies = {item['id']: item for item in map(json.loads, result.stdout.splitlines())}
            health = replies[2]['result']['content'][0]['text']
            self.assertIn('stream=up backend=PipeWire', health)

    @unittest.skipUnless((RUNTIME / 'pulse/native').exists(), 'no PulseAudio socket')
    def test_pulse_after_pipewire_failure(self):
        self.probe('PulseAudio', dict(os.environ, PIPEWIRE_REMOTE='tompouce-missing-audio-server'))

    def test_alsa_and_no_output(self):
        with tempfile.TemporaryDirectory(prefix='tompouce-output-') as root:
            config = Path(root) / 'alsa.conf'
            environment = dict(os.environ, ALSA_CONFIG_PATH=str(config),
                               PIPEWIRE_REMOTE='tompouce-missing-audio-server',
                               PULSE_SERVER='unix:' + root + '/missing-pulse',
                               JACK_DEFAULT_SERVER='tompouce-missing-jack-server')
            config.write_text('pcm.!default { type null }\n')
            self.probe('ALSA', environment)
            config.write_text('pcm.!default { type tompouce_missing_pcm_type }\n')
            self.probe('none', environment)

    @unittest.skipUnless(os.environ.get('TOMPOUCE_TEST_JACK') == '1', 'set TOMPOUCE_TEST_JACK=1 with a running JACK-compatible server')
    def test_jack_without_native_backends(self):
        self.probe('JACK', binary='./tests/probe_audio_portaudio')


if __name__ == '__main__':
    unittest.main()
