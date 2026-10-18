"""Installer plans and confined/resumable downloads; no system packages changed."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import wave

spec = importlib.util.spec_from_file_location('tompouce_install', Path(__file__).resolve().parents[1] / 'scripts/install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def wav():
    data = io.BytesIO()
    with wave.open(data, 'wb') as file:
        file.setnchannels(1); file.setsampwidth(2); file.setframerate(44100)
        file.writeframes(b'\0\0' * 32)
    return data.getvalue()


class InstallerTest(unittest.TestCase):
    def test_dry_run_has_no_side_effects(self):
        with tempfile.TemporaryDirectory() as root:
            prefix = Path(root) / 'uncreated'
            result = subprocess.run(['bash', 'install.sh', '--dry-run', '--skip-deps', '--prefix', str(prefix)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('Salamander piano', result.stdout)
            self.assertIn(str(prefix / 'lib/tompouce'), result.stdout)
            self.assertIn(str(prefix / 'bin/tompouce'), result.stdout)
            self.assertFalse(prefix.exists())

    def test_distro_commands(self):
        for identity, family in [('omarchy', 'arch'), ('ubuntu', 'debian'), ('fedora', 'fedora')]:
            with patch.object(installer.platform, 'freedesktop_os_release', return_value={'ID': identity}):
                self.assertEqual(installer.distro()[0], family)
        with patch.object(installer.shutil, 'which', return_value=None):
            self.assertIn('chromium-browser', installer.dependency_command('debian', {'ID': 'ubuntu'}))
            self.assertIn('chromium', installer.dependency_command('debian', {'ID': 'debian'}))
            self.assertNotIn('chromium', installer.dependency_command('arch', {}, False))

    def test_paths_and_links_refused(self):
        for path in ['/etc/passwd', '../escape.wav', '%2e%2e/escape.wav', 'a\\b.wav', 'a\0.wav']:
            with self.assertRaises(ValueError):
                installer.safe_relative(path)
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            (root / 'linked').symlink_to(root, target_is_directory=True)
            with self.assertRaises(ValueError):
                installer.directory(root / 'linked' / 'new')
            self.assertFalse((root / 'new').exists())

    def test_verified_download_resume_and_modified_file(self):
        with tempfile.TemporaryDirectory() as root:
            store = installer.BankInstaller(Path(root))
            url = 'https://example.com/pinned.wav'
            with patch.object(installer, 'fetch', return_value=wav()) as download:
                path, record = store.download('kit/kick.wav', url, True)
                store.receipt[path] = record; store.save()
                self.assertEqual(record['sha256'], hashlib.sha256(wav()).hexdigest())
                resumed = installer.BankInstaller(Path(root))
                resumed.download('kit/kick.wav', url, True)
                self.assertEqual(download.call_count, 1)
                (Path(root) / path).write_bytes(b'modified by user')
                with self.assertRaisesRegex(ValueError, 'locally modified'):
                    resumed.download('kit/kick.wav', url, True)

    def test_audio_header_and_unmanaged_file(self):
        with tempfile.TemporaryDirectory() as root:
            store = installer.BankInstaller(Path(root))
            with patch.object(installer, 'fetch', return_value=b'not audio'):
                with self.assertRaises(ValueError):
                    store.download('bad.wav', 'https://example.com/a', True)
            self.assertFalse((Path(root) / 'bad.wav').exists())
            (Path(root) / 'saved.wav').write_bytes(b'user data')
            with patch.object(installer, 'fetch', return_value=wav()):
                with self.assertRaisesRegex(ValueError, 'unmanaged'):
                    store.download('saved.wav', 'https://example.com/a', True)

    def test_catalog_paths_order_and_license_install(self):
        source = {'repo': 'owner/sounds', 'commit': 'a' * 40, 'credits': ['LICENSE']}
        lock = {'sources': {'kit': source}, 'catalogs': [{
            'name': 'test kit', 'profile': 'core', 'manifest_source': 'kit', 'manifest': 'strudel.json',
            'destination': 'kit/strudel.json', 'audio_source': 'kit', 'audio_prefix': 'audio', 'samples': 'kit'}]}
        manifest = {'_base': 'https://example.com/', 'drums': ['B.wav', 'a%20name.wav'], 'piano': {'C4': ['C4.wav']}}
        def fetched(url, maximum=0):
            if url.endswith('/strudel.json'):
                return json.dumps(manifest).encode()
            if url.startswith('https://api.github.com/'):
                return b'{"tree":[{"type":"blob","path":"LICENSE"}],"truncated":false}'
            if url.endswith('/LICENSE'):
                return b'license text'
            return wav()
        with tempfile.TemporaryDirectory() as root, patch.object(installer, 'fetch', side_effect=fetched):
            installer.BankInstaller(Path(root), 2).install(lock)
            self.assertTrue((Path(root) / 'kit/a name.wav').is_file())
            self.assertEqual(json.loads((Path(root) / 'kit/strudel.json').read_text()), manifest)
            self.assertEqual((Path(root) / 'licenses/kit/LICENSE').read_text(), 'license text')


if __name__ == '__main__':
    unittest.main()
