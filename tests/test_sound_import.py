"""Persistent local imports, without touching the user's sound library."""
import io
import json
import os
from pathlib import Path
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


if __name__ == '__main__':
    unittest.main()
