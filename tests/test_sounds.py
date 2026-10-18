"""Catalog integration against a fresh engine, with isolated imported sounds."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from test_sound_import import SoundStore, audio


class CatalogTest(unittest.TestCase):
    def test_catalog_import_reload_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, {'XDG_CONFIG_HOME': root, 'XDG_CACHE_HOME': root}):
            with SoundStore() as store:
                store.import_file('mydrums', 'B.wav', audio())
                store.import_file('mydrums', 'a.wav', audio(1))
            process = subprocess.Popen(['./tompouce', '--mcp'], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            try:
                def call(name, arguments=None):
                    process.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                        'params': {'name': name, 'arguments': arguments or {}}}) + '\n')
                    process.stdin.flush()
                    result = json.loads(process.stdout.readline())['result']
                    self.assertFalse(result.get('isError'), result)
                    return result['content'][0]['text']
                source = call('music_get_code')
                user = json.loads(call('music_sounds', {'category': 'user', 'query': 'mydrums'}))
                self.assertEqual(user['items'], [{'name': 'mydrums', 'category': 'user', 'slots': 2, 'code': 's("mydrums")'}])
                synth = json.loads(call('music_sounds', {'category': 'synths', 'query': 'SINE'}))
                self.assertEqual(synth['items'][0]['code'], 'note("c4").s("sine")')
                tables = json.loads(call('music_sounds', {'category': 'wavetables'}))
                self.assertGreater(tables['total'], 0)
                with SoundStore() as store:
                    store.import_file('mydrums', 'c.wav', audio(2))
                call('music_reload_sounds')
                user = json.loads(call('music_sounds', {'category': 'user', 'query': 'mydrums'}))
                self.assertEqual(user['items'][0]['slots'], 3)
                index = Path(root) / 'tompouce/sounds/index.json'
                index.write_bytes(b'broken')
                # Malformed on-disk indexes must retain the last usable catalog.
                process.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
                    'params': {'name': 'music_reload_sounds', 'arguments': {}}}) + '\n')
                process.stdin.flush()
                self.assertTrue(json.loads(process.stdout.readline())['result']['isError'])
                self.assertEqual(json.loads(call('music_sounds', {'category': 'user', 'query': 'mydrums'}))['items'][0]['slots'], 3)
                self.assertEqual(call('music_get_code'), source)
            finally:
                process.stdin.close()
                process.wait(timeout=10)
                process.stdout.close()


if __name__ == '__main__':
    unittest.main()
