"""Persistent local imports, without touching the user's sound library."""
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from sound_import import SoundStore, import_folder, sound_name


def audio(value=0):
    output = io.BytesIO()
    with wave.open(output, 'wb') as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(44100)
        file.writeframes(value.to_bytes(2, 'little', signed=True) * 64)
    return output.getvalue()


class ImportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.environment = patch.dict(os.environ, {'XDG_CONFIG_HOME': str(self.root / 'config')})
        self.environment.start()

    def tearDown(self):
        self.environment.stop()
        self.temp.cleanup()

    def test_persistence_order_and_replacement(self):
        with SoundStore() as store:
            store.import_file('drums', 'z.wav', audio())
            store.import_file('drums', 'A.wav', audio(1))
        with SoundStore() as store:
            entries = store._index()['sounds']['drums']
            self.assertEqual([item['label'] for item in entries], ['A.wav', 'z.wav'])
            old = entries[0]['file']
            store.import_file('drums', 'A.wav', audio(2))
            entries = store._index()['sounds']['drums']
            self.assertEqual(len(entries), 2)
            self.assertNotEqual(entries[0]['file'], old)
            self.assertEqual((self.root / 'config/tompouce/sounds/assets' / entries[0]['file']).read_bytes(), audio(2))

    def test_folder_groups_skip_links(self):
        folder = self.root / 'My Drums'
        folder.mkdir()
        (folder / 'a.wav').write_bytes(audio())
        (folder / 'linked.wav').symlink_to(folder / 'a.wav')
        (folder / 'linked-dir').symlink_to(folder, target_is_directory=True)
        self.assertEqual(import_folder(folder), 1)
        with SoundStore() as store:
            self.assertEqual(list(store._index()['sounds']), ['my_drums'])
        self.assertEqual(sound_name('123 boom'), 'sound_123_boom')

    def test_legacy_library_and_new_directory_precedence(self):
        legacy = self.root / 'config/strudelterm'
        with SoundStore() as store:
            store.import_file('legacy', 'a.wav', audio())
        (self.root / 'config/tompouce').rename(legacy)
        with SoundStore() as store:
            self.assertIn('legacy', store._index()['sounds'])
        self.assertTrue((self.root / 'config/tompouce/sounds/index.json').is_file())
        self.assertFalse(legacy.exists())
        with SoundStore() as store:
            self.assertIn('legacy', store._index()['sounds'])
        legacy.mkdir()
        with SoundStore() as store:
            self.assertIn('legacy', store._index()['sounds'])
        self.assertTrue(legacy.exists())

    def test_refuse_invalid_input_and_linked_storage(self):
        with SoundStore() as store:
            for name, label, data in [('..', 'a.wav', audio()), ('ok', '../a.wav', audio()),
                                      ('ok', 'a.wav', b'invalid'), ('ok', 'a.txt', audio()),
                                      ('ok', 'a.wav', b'x' * (8 * 1024 * 1024 + 1))]:
                with self.assertRaises(ValueError):
                    store.import_file(name, label, data)
        linked = self.root / 'linked'
        linked.symlink_to(self.root / 'config', target_is_directory=True)
        with patch.dict(os.environ, {'XDG_CONFIG_HOME': str(linked)}):
            with self.assertRaises(OSError):
                SoundStore()

class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.environment = patch.dict(os.environ, {'XDG_CONFIG_HOME': str(self.root / 'config')})
        self.environment.start()
        from sound_import import parse_manifest, note_midi
        self.parse_manifest = parse_manifest
        self.note_midi = note_midi

    def tearDown(self):
        self.environment.stop()
        self.temp.cleanup()

    def bank(self, files, manifest):
        folder = self.root / 'kit'
        folder.mkdir(exist_ok=True)
        for relative, data in files.items():
            path = folder / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (folder / 'strudel.json').write_text(json.dumps(manifest))
        return folder

    def test_note_midi_matches_upstream_grammar(self):
        self.assertEqual(self.note_midi('c4'), 60)
        self.assertEqual(self.note_midi('C#3'), 49)
        self.assertEqual(self.note_midi('db5'), 73)
        self.assertEqual(self.note_midi('c'), 48)
        self.assertEqual(self.note_midi('g9'), 127)
        self.assertEqual(self.note_midi('c#'), 49)
        for bad in ('x9', '', 'c-', 'h4', 'c10x', None, 60, ' Cb4 '):
            self.assertIsNone(self.note_midi(bad))
        self.assertIsNone(self.note_midi('c-2'))
        self.assertIsNone(self.note_midi('g#9'))

    def test_manifest_forms_order_and_pitch(self):
        from sound_import import import_folder
        folder = self.bank(
            {'sub/bd.wav': audio(), 'sub/sdx.wav': audio(1), 'sub/sdy.wav': audio(2),
             'sub/low.wav': audio(3), 'sub/high.wav': audio(4), 'sub/nest/deep.wav': audio(5)},
            {'_base': 'sub', 'bd': 'bd.wav', 'Shared': ['sdx.wav', 'sdy.wav'],
             'piano': {'c2': 'low.wav', 'C4': ['high.wav', 'nest/deep.wav']},
             '_hidden': 'bd.wav'})
        self.assertEqual(import_folder(folder), 6)
        with SoundStore() as store:
            index = store._index()
        sounds = index['sounds']
        self.assertEqual(index['version'], 2)
        self.assertNotIn('_hidden', sounds)
        self.assertEqual([item['label'] for item in sounds['bd']], ['bd.wav'])
        self.assertEqual([item['label'] for item in sounds['shared']], ['sdx.wav', 'sdy.wav'])
        piano = sounds['piano']
        self.assertEqual([(item.get('midi'), item['label']) for item in piano],
                         [(36, 'low.wav'), (60, 'high.wav'), (60, 'nest_deep.wav')])
        regions = [item['region'] for item in piano]
        self.assertEqual(len(set(regions)), 2)
        self.assertEqual([item['bank'] for item in piano], ['manifest:kit'] * 3)

    def test_manifest_base_forms(self):
        from sound_import import import_folder
        folder = self.bank(
            {'audio/kick.wav': audio(), 'other/snare.wav': audio(1)},
            {'kick': 'kick.wav', '_base': 'audio',
             'snare': {'_base': 'other', 'd2': 'snare.wav'}})
        self.assertEqual(import_folder(folder), 2)
        with SoundStore() as store:
            sounds = store._index()['sounds']
        self.assertIn('kick', sounds)
        self.assertEqual(sounds['snare'][0]['midi'], 38)

    def test_manifest_reimport_replaces_and_collects(self):
        from sound_import import import_folder
        folder = self.bank({'a.wav': audio(), 'b.wav': audio(1), 'c.wav': audio(2)},
                           {'bd': ['a.wav', 'b.wav'], 'sd': 'c.wav'})
        self.assertEqual(import_folder(folder), 3)
        with SoundStore() as store:
            before = store._index()
            old_files = {item['file'] for entries in before['sounds'].values() for item in entries}
        (folder / 'strudel.json').write_text(json.dumps({'bd': ['b.wav'], 'hh': 'c.wav'}))
        self.assertEqual(import_folder(folder), 2)
        with SoundStore() as store:
            sounds = store._index()['sounds']
        self.assertNotIn('sd', sounds)
        self.assertEqual([item['label'] for item in sounds['bd']], ['b.wav'])
        self.assertIn('hh', sounds)
        assets = set(os.listdir(self.root / 'config/tompouce/sounds/assets'))
        live = {item['file'] for entries in sounds.values() for item in entries}
        self.assertEqual(assets, live)
        self.assertTrue(old_files - live, "removed slots leave collected assets")

    def test_manifest_owned_names_refuse_loose_imports(self):
        from sound_import import import_folder
        folder = self.bank({'a.wav': audio()}, {'bd': 'a.wav'})
        import_folder(folder)
        with SoundStore() as store:
            with self.assertRaisesRegex(ValueError, 'imported bank'):
                store.import_file('bd', 'other.wav', audio(1))
            store.import_file('free', 'other.wav', audio(1))
            self.assertIn('free', store._index()['sounds'])

    def test_invalid_manifest_preserves_previous_bank(self):
        from sound_import import import_folder
        folder = self.bank({'a.wav': audio()}, {'bd': 'a.wav'})
        import_folder(folder)
        with SoundStore() as store:
            before = (self.root / 'config/tompouce/sounds/index.json').read_bytes()
        (folder / 'strudel.json').write_text(json.dumps({'bd': '../escape.wav'}))
        with self.assertRaises(ValueError):
            import_folder(folder)
        with SoundStore() as store:
            after = (self.root / 'config/tompouce/sounds/index.json').read_bytes()
        self.assertEqual(before, after)
        with SoundStore() as store:
            self.assertIn('bd', store._index()['sounds'])

    def test_manifest_rejects_escapes_and_remote_refs(self):
        from sound_import import import_folder
        cases = [
            {'bd': '../escape.wav'}, {'bd': '/abs.wav'}, {'bd': 'https://example.com/a.wav'},
            {'bd': 'github:user/repo/a.wav'}, {'bd': 'sub/../../escape.wav'},
            {'_base': '../outside', 'bd': 'a.wav'}, {'_base': 'https://example.com/', 'bd': 'a.wav'},
        ]
        for manifest in cases:
            folder = self.root / 'evil'
            if folder.exists():
                shutil.rmtree(folder)
            folder.mkdir()
            (folder / 'a.wav').write_bytes(audio())
            (folder / 'strudel.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError, msg=json.dumps(manifest)):
                import_folder(folder)

    def test_manifest_rejects_symlink_escape(self):
        from sound_import import import_folder
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'evil.wav').write_bytes(audio())
        folder = self.root / 'kit'
        folder.mkdir()
        (folder / 'sub').symlink_to(outside, target_is_directory=True)
        (folder / 'strudel.json').write_text(json.dumps({'bd': 'sub/evil.wav'}))
        with self.assertRaises(ValueError):
            import_folder(folder)
        folder2 = self.root / 'kit2'
        folder2.mkdir()
        (folder2 / 'linked.wav').symlink_to(outside / 'evil.wav')
        (folder2 / 'strudel.json').write_text(json.dumps({'bd': 'linked.wav'}))
        with self.assertRaises(ValueError):
            import_folder(folder2)

    def test_manifest_rejects_bad_content(self):
        from sound_import import import_folder
        folder = self.root / 'kit'
        folder.mkdir()
        (folder / 'a.wav').write_bytes(audio())
        bad = ['not json{', '[1,2]', '42', '"str"', '{"bd": 42}', '{"bd": []}',
               '{"bd": {}}', '{"bd": {"xx": "a.wav"}}', '{"bd": {"c4": []}}',
               '{"bd": {"c4": {"deep": "a.wav"}}}', '{"bd": [["a.wav"]]}',
               '{"bd": "missing.wav"}', '{"bd": "a.txt"}']
        (folder / 'a.txt').write_bytes(audio())
        for text in bad:
            (folder / 'strudel.json').write_text(text)
            with self.assertRaises(ValueError, msg=text):
                import_folder(folder)
        (folder / 'strudel.json').write_bytes(b'\xff\xfe invalid')
        with self.assertRaises(ValueError):
            import_folder(folder)

    def test_manifest_partial_failure_stages_nothing(self):
        from sound_import import import_folder
        folder = self.bank({'a.wav': audio(), 'b.wav': audio(1)}, {'bd': ['a.wav', 'b.wav']})
        real_replace = os.replace

        def swap_fail(src, dst, *args, **kwargs):
            if dst == 'index.json':
                raise OSError('boom')
            return real_replace(src, dst, *args, **kwargs)

        with patch('os.replace', side_effect=swap_fail):
            with self.assertRaises(OSError):
                import_folder(folder)
        assets = set(os.listdir(self.root / 'config/tompouce/sounds/assets')) if (
            self.root / 'config/tompouce/sounds/assets').exists() else set()
        self.assertEqual(assets, set())
        self.assertFalse((self.root / 'config/tompouce/sounds/index.json').exists())

    def test_manifest_quotas_and_invalid_base_entry(self):
        from sound_import import import_folder
        folder = self.root / 'kit'
        folder.mkdir()
        (folder / 'a.wav').write_bytes(audio())
        manifest = {f'bank{i}': 'a.wav' for i in range(257)}
        (folder / 'strudel.json').write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            import_folder(folder)
        with SoundStore() as store:
            self.assertNotIn('bank0', store._index()['sounds'])
        manifest = {'bd': ['a.wav'] * 1025}
        (folder / 'strudel.json').write_text(json.dumps(manifest))
        with self.assertRaises(ValueError):
            import_folder(folder)
        (folder / 'strudel.json').write_text(json.dumps(
            {'ok': 'a.wav', 'remote': {'_base': 'https://example.com/', 'c4': 'a.wav'}}))
        self.assertEqual(import_folder(folder), 1)

    def test_folder_without_manifest_stays_flat(self):
        from sound_import import import_folder
        folder = self.root / 'plain'
        folder.mkdir()
        (folder / 'a.wav').write_bytes(audio())
        self.assertEqual(import_folder(folder), 1)
        with SoundStore() as store:
            sounds = store._index()['sounds']
        self.assertEqual([item['label'] for item in sounds['plain']], ['a.wav'])
        self.assertTrue(all('midi' not in item and 'bank' not in item for item in sounds['plain']))

    def test_symlinked_manifest_keeps_flat_import(self):
        from sound_import import import_folder
        folder = self.root / 'kit'
        folder.mkdir()
        (folder / 'a.wav').write_bytes(audio())
        (folder / 'other.json').write_text(json.dumps({'bd': 'a.wav'}))
        (folder / 'strudel.json').symlink_to(folder / 'other.json')
        self.assertEqual(import_folder(folder), 1)
        with SoundStore() as store:
            sounds = store._index()['sounds']
        self.assertIn('kit', sounds)
        self.assertNotIn('bd', sounds)

    def test_cli_manifest_import_matches_helper_spawn(self):
        script = Path(__file__).resolve().parents[1] / 'scripts' / 'sound_import.py'
        folder = self.root / 'kit'
        folder.mkdir()
        (folder / 'a.wav').write_bytes(audio())
        (folder / 'strudel.json').write_text(json.dumps({'bd': {'c4': 'a.wav'}}))
        env = dict(os.environ, XDG_CONFIG_HOME=str(self.root / 'config'))
        done = subprocess.run(
            [sys.executable, str(script), '--folder', str(folder),
             '--parent-pid', str(os.getpid())],
            env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn('Imported 1', done.stdout)
        with SoundStore() as store:
            sounds = store._index()['sounds']
        self.assertEqual(sounds['bd'][0]['midi'], 60)
        (folder / 'strudel.json').write_text(json.dumps({'bd': '../escape.wav'}))
        done = subprocess.run(
            [sys.executable, str(script), '--folder', str(folder)],
            env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn('Sound import failed', done.stderr)
        with SoundStore() as store:
            self.assertEqual(sounds, store._index()['sounds'])

class GitHubBankTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.environment = patch.dict(os.environ, {'XDG_CONFIG_HOME': str(self.root / 'config')})
        self.environment.start()
        from sound_import import parse_github_spec
        self.parse_github_spec = parse_github_spec

    def tearDown(self):
        self.environment.stop()
        self.temp.cleanup()

    def test_github_spec_parsing_mirrors_upstream_defaults(self):
        url, base, bank = self.parse_github_spec('github:tidalcycles/dirt-samples')
        self.assertEqual(url, 'https://raw.githubusercontent.com/tidalcycles/dirt-samples/main/strudel.json')
        self.assertEqual(base, 'https://raw.githubusercontent.com/tidalcycles/dirt-samples/main/')
        self.assertEqual(bank, 'github:tidalcycles_dirt-samples')
        url, base, bank = self.parse_github_spec('github:user/repo/branch/sub/dir')
        self.assertEqual(url, 'https://raw.githubusercontent.com/user/repo/branch/sub/dir/strudel.json')
        self.assertEqual(bank, 'github:user_repo_branch_sub_dir')
        for bad in ('https://example.com/x', 'github:', 'github:onlyuser!', 'github:a/b/c/d/e/f/g',
                    'github:a//b', 'github:a/b/', 'github:/b', '', None, 42):
            with self.assertRaises(ValueError, msg=str(bad)):
                self.parse_github_spec(bad)

    def test_github_redirect_policy_without_sockets(self):
        from urllib.request import Request
        from sound_import import _GitHubRedirect, _http_get, GITHUB_HOST
        redirect = _GitHubRedirect({GITHUB_HOST})
        request = Request('https://raw.githubusercontent.com/a/b/main/strudel.json')
        for newurl in ('http://raw.githubusercontent.com/a',
                       'https://evil.example/a',
                       'https://user@raw.githubusercontent.com/a',
                       'https://raw.githubusercontent.com:444/a'):
            with self.assertRaises(ValueError, msg=newurl):
                redirect.redirect_request(request, None, 302, 'Found', None, newurl)
        for _ in range(4):
            redirect.redirect_request(request, None, 302, 'Found', None,
                                      'https://raw.githubusercontent.com/a')
        with self.assertRaises(ValueError):
            redirect.redirect_request(request, None, 302, 'Found', None,
                                      'https://raw.githubusercontent.com/a')
        with self.assertRaises(ValueError):
            _http_get('http://raw.githubusercontent.com/a', 64)

    def fetched_bank(self, fetch):
        from sound_import import import_github_bank
        with patch('sound_import._http_get', side_effect=fetch) as get:
            slots = import_github_bank('github:owner/kit', parent_pid=os.getppid())
        return slots, get

    def test_github_bank_imports_offline_like_local(self):
        manifest = {'_base': 'audio', 'bd': 'bd.wav',
                    'keys': {'c2': 'low.wav', 'C4': ['hi.wav', 'sub/deep.wav']}}
        blobs = {'https://raw.githubusercontent.com/owner/kit/main/strudel.json':
                 json.dumps(manifest).encode(),
                 'https://raw.githubusercontent.com/owner/kit/main/audio/bd.wav': audio(),
                 'https://raw.githubusercontent.com/owner/kit/main/audio/low.wav': audio(1),
                 'https://raw.githubusercontent.com/owner/kit/main/audio/hi.wav': audio(2),
                 'https://raw.githubusercontent.com/owner/kit/main/audio/sub/deep.wav': audio(3)}

        def fetch(url, maximum):
            return blobs[url]

        slots, get = self.fetched_bank(fetch)
        self.assertEqual(slots, 4)
        self.assertEqual(get.call_count, 5)
        with SoundStore() as store:
            sounds = store._index()['sounds']
        self.assertEqual([item['label'] for item in sounds['bd']], ['bd.wav'])
        self.assertEqual([(item.get('midi'), item['label']) for item in sounds['keys']],
                         [(36, 'low.wav'), (60, 'hi.wav'), (60, 'sub_deep.wav')])
        self.assertEqual({item['bank'] for entries in sounds.values() for item in entries},
                         {'github:owner_kit'})

    def test_github_rejects_escapes_missing_and_bad_audio(self):
        from urllib.error import HTTPError
        from sound_import import import_github_bank

        def fetch(url, maximum):
            if url.endswith('/strudel.json'):
                return json.dumps({'bd': '../escape.wav'}).encode()
            return audio()

        with self.assertRaises(ValueError):
            self.fetched_bank(fetch)
        with SoundStore() as store:
            self.assertNotIn('bd', store._index()['sounds'])

        def missing(url, maximum):
            if url.endswith('/strudel.json'):
                return json.dumps({'bd': 'gone.wav'}).encode()
            raise HTTPError(url, 404, 'Not Found', None, None)

        with self.assertRaises(ValueError):
            self.fetched_bank(missing)

        def corrupt(url, maximum):
            if url.endswith('/strudel.json'):
                return json.dumps({'bd': 'bd.wav'}).encode()
            return b'not audio at all'

        with self.assertRaises(ValueError):
            self.fetched_bank(corrupt)

        def huge(url, maximum):
            if url.endswith('/strudel.json'):
                return json.dumps({'bd': 'bd.wav'}).encode()
            return b'RIFF....WAVE' + b'x' * (8 * 1024 * 1024)

        with self.assertRaises(ValueError):
            self.fetched_bank(huge)

    def test_github_reimport_replaces_and_owns_names(self):
        from sound_import import import_github_bank
        first = {'bd': ['a.wav'], 'sd': 'b.wav'}
        audios = {'a.wav': audio(), 'b.wav': audio(1), 'c.wav': audio(2)}

        def fetch(url, maximum):
            name = url.rsplit('/', 1)[-1]
            if name == 'strudel.json':
                return json.dumps(first).encode()
            if name in audios:
                return audios[name]
            from urllib.error import HTTPError
            raise HTTPError(url, 404, 'Not Found', None, None)

        with patch('sound_import._http_get', side_effect=fetch):
            self.assertEqual(import_github_bank('github:owner/kit'), 2)
            first.clear()
            first.update({'bd': ['a.wav'], 'hh': 'c.wav'})
            self.assertEqual(import_github_bank('github:owner/kit'), 2)
        with SoundStore() as store:
            sounds = store._index()['sounds']
            self.assertNotIn('sd', sounds)
            self.assertEqual([item['label'] for item in sounds['bd']], ['a.wav'])
            self.assertIn('hh', sounds)
            with self.assertRaisesRegex(ValueError, 'imported bank'):
                store.import_file('bd', 'other.wav', audio(3))


if __name__ == '__main__':
    unittest.main()
