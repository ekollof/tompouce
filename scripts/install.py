"""Build and install Tompouce and pinned sound catalogs on Linux."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import secrets
import stat
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from urllib.error import HTTPError
from urllib.parse import quote, unquote, urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler

SOURCE = Path(__file__).resolve().parents[1]
TREE_URL = 'https://codeload.github.com/tree-sitter/tree-sitter/tar.gz/refs/tags/v0.25.10'
TREE_SHA = 'ad5040537537012b16ef6e1210a572b927c7cdc2b99d1ee88d44a7dcdc3ff44c'
PACKAGES = {
    'arch': ['gcc', 'make', 'cmake', 'ninja', 'pkgconf', 'xxd', 'notcurses', 'portaudio', 'libpipewire', 'libpulse', 'libsamplerate', 'fluidsynth', 'soundfont-fluid', 'curl', 'cjson', 'leveldb', 'alsa-lib', 'tree-sitter', 'python', 'unzip', 'tar', 'ca-certificates'],
    'debian': ['build-essential', 'cmake', 'ninja-build', 'pkg-config', 'xxd', 'libnotcurses-dev', 'portaudio19-dev', 'libpipewire-0.3-dev', 'libpulse-dev', 'libsamplerate0-dev', 'libfluidsynth-dev', 'fluid-soundfont-gm', 'libcurl4-openssl-dev', 'libcjson-dev', 'libleveldb-dev', 'libasound2-dev', 'libtree-sitter-dev', 'python3', 'curl', 'unzip', 'tar', 'ca-certificates'],
    'fedora': ['gcc', 'gcc-c++', 'make', 'cmake', 'ninja-build', 'pkgconf-pkg-config', 'xxd', 'notcurses-devel', 'portaudio-devel', 'pipewire-devel', 'pulseaudio-libs-devel', 'libsamplerate-devel', 'fluidsynth-devel', 'fluid-soundfont-gm', 'libcurl-devel', 'cjson-devel', 'leveldb-devel', 'alsa-lib-devel', 'tree-sitter-devel', 'python3', 'curl', 'unzip', 'tar', 'ca-certificates'],
}
RUNTIME_FILES = [
    'tompouce', 'README.md', 'LICENSE', 'NOTICE', 'scripts/companion.py', 'scripts/sound_import.py',
    'web/index.html', 'web/companion.js', 'web/sounds.js', 'web/companion.css',
    'web/dist/editor.js', 'web/dist/licenses.txt', 'js/dist/licenses.txt',
    'thirdparty/STRUDEL-LICENSE', 'thirdparty/quickjs/LICENSE',
    'thirdparty/tree-sitter-javascript/LICENSE', 'thirdparty/strudel-csound/README.md',
    'thirdparty/minimp3.h', 'thirdparty/dr_flac.h',
    'thirdparty/native-synth/README.md', 'thirdparty/native-synth/LICENSE-ZzFX',
    'thirdparty/native-synth/LICENSE-Strudel', 'thirdparty/native-synth/LICENSE-Kabelsalat',
    'thirdparty/native-synth/LICENSE-Apache-2.0',
    'thirdparty/native-synth/LICENSE-Phaze',
    'thirdparty/native-synth/LICENSE-FFT',
    'thirdparty/waveforms/NOTICE',
]


def safe_relative(value):
    if not isinstance(value, str) or not value or urlsplit(value).scheme or '\\' in value or any(ord(c) < 32 for c in value):
        raise ValueError('unsafe relative path')
    path = PurePosixPath(unquote(value))
    if path.is_absolute() or '..' in path.parts or '\\' in str(path) or any(ord(c) < 32 for c in str(path)):
        raise ValueError('unsafe relative path')
    return path


def directory(path):
    """Reject symlink ancestors, including for installer-controlled destinations."""
    path = Path(path).absolute()
    if '..' in path.parts:
        raise ValueError('directory traversal refused')
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        if current.is_symlink():
            raise ValueError(f'refusing linked installation directory: {current}')
        current.mkdir(exist_ok=True)
        if not current.is_dir():
            raise ValueError(f'not a directory: {current}')
    return path


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def fetch(url, maximum=2 * 1024 * 1024):
    if urlsplit(url).scheme != 'https':
        raise ValueError('downloads require HTTPS')
    request = Request(url, headers={'User-Agent': 'Tompouce-installer/1'})
    class HTTPSRedirect(HTTPRedirectHandler):
        def redirect_request(self, request, fp, code, msg, headers, newurl):
            if urlsplit(newurl).scheme != 'https':
                raise ValueError('non-HTTPS redirect refused')
            return super().redirect_request(request, fp, code, msg, headers, newurl)
    opener = build_opener(HTTPSRedirect())
    last = None
    for attempt in range(4):
        try:
            with opener.open(request, timeout=45) as response:
                if urlsplit(response.url).scheme != 'https':
                    raise ValueError('non-HTTPS redirect refused')
                declared = response.headers.get('Content-Length')
                if declared is not None and (not declared.isdigit() or int(declared) > maximum):
                    raise ValueError(f'invalid download length: {url}')
                data = response.read(maximum + 1)
                if declared is not None and len(data) != int(declared):
                    raise OSError('incomplete download')
            if len(data) > maximum:
                raise ValueError(f'download too large: {url}')
            return data
        except OSError as error:
            if isinstance(error, HTTPError) and error.code < 500 and error.code not in (408, 429):
                raise
            last = error
            if attempt < 3:
                time.sleep(2 ** attempt)
    raise OSError(f'could not download {url}: {last}')


def atomic_bytes(path, data, mode=0o644):
    directory(path.parent)
    if path.is_symlink():
        raise ValueError(f'refusing linked output: {path}')
    temporary = path.with_name('.' + path.name + '-' + secrets.token_hex(8))
    try:
        with temporary.open('xb') as file:
            file.write(data)
            file.flush()
            os.fsync(file.fileno())
        temporary.chmod(mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def raw_url(source, path):
    path = safe_relative(path)
    return f"https://raw.githubusercontent.com/{source['repo']}/{source['commit']}/{quote(str(path), safe='/')}"


def sample_paths(value):
    if isinstance(value, str):
        yield str(safe_relative(value))
    elif isinstance(value, list):
        for item in value:
            yield from sample_paths(item)
    elif isinstance(value, dict):
        for name, item in value.items():
            if not name.startswith('_'):
                yield from sample_paths(item)
    else:
        raise ValueError('invalid sample catalog value')


class BankInstaller:
    def __init__(self, root, jobs=8):
        self.root = directory(root)
        self.jobs = jobs
        self.receipt_path = self.root / '.install-receipt.json'
        if self.receipt_path.is_symlink():
            raise ValueError('linked bank receipt refused')
        if self.receipt_path.exists():
            metadata = self.receipt_path.stat()
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > 16 * 1024 * 1024:
                raise ValueError('invalid bank receipt file')
        self.receipt = json.loads(self.receipt_path.read_text()) if self.receipt_path.exists() else {}
        if not isinstance(self.receipt, dict):
            raise ValueError('invalid bank receipt')
        for record in self.receipt.values():
            if (not isinstance(record, dict) or not isinstance(record.get('url'), str) or
                    not isinstance(record.get('sha256'), str) or not re.fullmatch(r'[0-9a-f]{64}', record['sha256'])):
                raise ValueError('invalid bank receipt entry')

    def save(self):
        atomic_bytes(self.receipt_path, json.dumps(self.receipt, sort_keys=True).encode())

    def download(self, relative, url, audio=False, data=None):
        relative = str(safe_relative(relative))
        path = self.root / relative
        directory(path.parent)
        if path.is_symlink() or path.exists() and not path.is_file():
            raise ValueError(f'linked or invalid bank file: {relative}')
        prior = self.receipt.get(relative)
        if path.exists():
            current = digest(path)
            if isinstance(prior, dict) and prior.get('url') == url and prior.get('sha256') == current:
                return relative, prior
            if prior and prior.get('sha256') != current:
                raise ValueError(f'locally modified bank file: {relative}; use another install prefix')
        if data is None:
            data = fetch(url, 128 * 1024 * 1024 if audio else 2 * 1024 * 1024)
        if audio:
            extension = path.suffix.lower()
            valid = (extension == '.wav' and data[:4] == b'RIFF' and data[8:12] == b'WAVE' or
                     extension == '.flac' and data[:4] == b'fLaC' or
                     extension == '.mp3' and (data[:3] == b'ID3' or len(data) > 1 and data[0] == 255 and data[1] & 224 == 224))
            if not valid:
                raise ValueError(f'invalid audio header: {relative}')
        checksum = hashlib.sha256(data).hexdigest()
        if path.exists() and not prior and digest(path) != checksum:
            raise ValueError(f'unmanaged bank file differs: {relative}; use another install prefix')
        atomic_bytes(path, data)
        return relative, {'url': url, 'sha256': checksum}

    def batch(self, tasks, contents=None):
        tasks = dict(tasks)
        total = len(tasks)
        with ThreadPoolExecutor(max_workers=self.jobs) as pool:
            futures = [pool.submit(self.download, path, url, True, contents[path] if contents is not None else None)
                       for path, url in tasks.items()]
            try:
                for count, future in enumerate(as_completed(futures), 1):
                    path, record = future.result()
                    self.receipt[path] = record
                    if count % 25 == 0 or count == total:
                        self.save()
                        print(f'  {count}/{total} samples verified', flush=True)
            finally:
                for future in futures:
                    future.cancel()
                self.save()

    @staticmethod
    def archive_contents(source, wanted):
        """Read only requested regular files from a checksum-pinned archive.

        Small wavetable banks contain thousands of tiny files; one archive
        avoids thousands of HTTP round trips while preserving per-file receipts.
        No archive paths are extracted to disk.
        """
        checksum = source['archive_sha256']
        if not re.fullmatch(r'[0-9a-f]{64}', checksum):
            raise ValueError('invalid source archive checksum')
        url = f"https://codeload.github.com/{source['repo']}/tar.gz/{source['commit']}"
        data = fetch(url, 128 * 1024 * 1024)
        if hashlib.sha256(data).hexdigest() != checksum:
            raise ValueError('source archive checksum mismatch')
        wanted = {str(safe_relative(path)) for path in wanted}
        prefix = f"{source['repo'].split('/')[-1]}-{source['commit']}/"
        contents, expanded = {}, 0
        with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
            for count, member in enumerate(archive):
                if count >= 200000:
                    raise ValueError('too many source archive entries')
                if not member.name.startswith(prefix):
                    continue
                path = member.name[len(prefix):]
                if path not in wanted:
                    continue
                if path in contents or not member.isfile() or member.size < 0 or member.size > 128 * 1024 * 1024:
                    raise ValueError('invalid source archive member')
                expanded += member.size
                if expanded > 256 * 1024 * 1024:
                    raise ValueError('source archive expansion limit exceeded')
                file = archive.extractfile(member)
                payload = file.read(member.size + 1)
                if len(payload) != member.size:
                    raise ValueError('truncated source archive member')
                contents[path] = payload
        if contents.keys() != wanted:
            raise ValueError('source archive is missing catalog files')
        return contents

    def install(self, lock, all_banks=True):
        used = set()
        for catalog in lock['catalogs']:
            if catalog['profile'] == 'all' and not all_banks:
                continue
            print(f"Installing {catalog['name']}…", flush=True)
            source = lock['sources'][catalog['manifest_source']]
            path, record = self.download(catalog['destination'], raw_url(source, catalog['manifest']))
            self.receipt[path] = record
            manifest = json.loads((self.root / path).read_text())
            aliases = {}
            prefix = catalog.get('alias_prefix', '')
            if prefix:
                if not isinstance(prefix, str) or not re.fullmatch(r'[a-z0-9_]{1,64}', prefix):
                    raise ValueError('invalid catalog alias prefix')
                aliases.update({prefix + name: value for name, value in manifest.items()
                                if not name.startswith('_')})
            extra = catalog.get('aliases', {})
            if not isinstance(extra, dict) or len(extra) > 1024:
                raise ValueError('invalid catalog aliases')
            for name, selection in extra.items():
                if (not re.fullmatch(r'[A-Za-z0-9_]{1,128}', name) or
                        not isinstance(selection, list) or len(selection) != 2 or
                        not isinstance(selection[0], str) or not isinstance(selection[1], int)):
                    raise ValueError('invalid catalog alias selection')
                values = manifest.get(selection[0])
                if not isinstance(values, list) or not 0 <= selection[1] < len(values):
                    raise ValueError('catalog alias index out of range')
                aliases[name] = [values[selection[1]]]
            if aliases:
                # Preserve pinned manifests and their historical names. The
                # sidecar shares the sample directory, so aliases work offline.
                if '_base' in manifest:
                    aliases['_base'] = manifest['_base']
                data = json.dumps(aliases, sort_keys=True).encode()
                relative = str(PurePosixPath(catalog['samples']) / 'web-repl-aliases.json')
                url = raw_url(source, catalog['manifest']) + '#aliases-' + hashlib.sha256(data).hexdigest()
                path, record = self.download(relative, url, False, data)
                self.receipt[path] = record
            audio_source = lock['sources'][catalog['audio_source']]
            tasks = []
            remote_paths = {}
            for sample in sorted(set(sample_paths(manifest))):
                remote = str(PurePosixPath(catalog['audio_prefix']) / sample)
                for old, new in audio_source.get('path_prefixes', {}).items():
                    if remote.startswith(old):
                        remote = new + remote[len(old):]
                        break
                destination = str(PurePosixPath(catalog['samples']) / sample)
                tasks.append((destination, raw_url(audio_source, remote)))
                remote_paths[destination] = remote
            contents = None
            if audio_source.get('archive_sha256'):
                verified = all((self.root / path).is_file() and not (self.root / path).is_symlink() and
                               self.receipt.get(path, {}).get('url') == url and
                               self.receipt.get(path, {}).get('sha256') == digest(self.root / path)
                               for path, url in tasks)
                if verified:
                    contents = {path: b'' for path in remote_paths}  # download rechecks each receipt before using data
                else:
                    files = self.archive_contents(audio_source, remote_paths.values())
                    contents = {path: files[remote] for path, remote in remote_paths.items()}
            self.batch(tasks, contents)
            used.update([catalog['manifest_source'], catalog['audio_source']])
        # Preserve upstream attribution, including license files inside sample folders.
        for key in sorted(used):
            source = lock['sources'][key]
            for credit in source['credits']:
                relative = f"licenses/{key}/{safe_relative(credit)}"
                path, record = self.download(relative, raw_url(source, credit))
                self.receipt[path] = record
            self.save()


def install_gm_webaudiofont(banks_root, jobs=8, all_banks=True):
    """Prefetch the pinned gm_* webaudiofont packs so gm sounds play offline
    (about 138 MiB for all variants). Without these the engine fetches packs on first play and
    falls back to the local SoundFont offline."""
    banks_root = Path(banks_root)
    path = SOURCE / 'scripts/gm-webaudiofont.lock.json'
    if path.is_symlink():
        raise ValueError('linked gm pack lock refused')
    gm_lock = json.loads(path.read_text())
    if (not isinstance(gm_lock, dict) or gm_lock.get('version') != 1 or
            not isinstance(gm_lock.get('source'), dict) or not isinstance(gm_lock.get('packs'), list)):
        raise ValueError('invalid gm pack lock file')
    source = gm_lock['source']
    for key in ('repo', 'branch', 'commit', 'license'):
        if not isinstance(source.get(key), str) or not source[key]:
            raise ValueError('invalid gm pack lock source')
    if not re.fullmatch(r'[0-9a-f]{40}', source['commit']):
        raise ValueError('invalid gm pack lock commit')
    packs = gm_lock['packs']
    if all_banks:
        variants = gm_lock.get('variants', [])
        if not isinstance(variants, list):
            raise ValueError('invalid gm variant list')
        packs = packs + variants
    if not packs or len(packs) > 2048:
        raise ValueError('invalid gm pack list')
    base = f"https://raw.githubusercontent.com/{source['repo']}/{source['commit']}/sound"
    total = 0
    for pack in packs:
        if (not isinstance(pack, dict) or not re.fullmatch(r'gm_[a-z0-9_]+', pack.get('name', '')) or
                not re.fullmatch(r'[A-Za-z0-9_-]+_sf2_file\.js', pack.get('file', ''), re.IGNORECASE) or
                not re.fullmatch(r'[0-9a-f]{64}', pack.get('sha256', '')) or
                not isinstance(pack.get('bytes'), int) or not 1 <= pack['bytes'] <= 2 * 1024 * 1024):
            raise ValueError('invalid gm pack lock entry')
        total += pack['bytes']
    if total > 256 * 1024 * 1024:
        raise ValueError('gm pack lock too large')
    installer = BankInstaller(banks_root, jobs)
    print(f'Installing gm webaudiofont packs ({len(packs)} packs)…', flush=True)
    tasks = {}
    for pack in packs:
        relative = f"gm-webaudiofont/{pack['file']}"
        url = f"{base}/{quote(pack['file'], safe='/')}"
        local = banks_root / relative
        record = installer.receipt.get(relative)
        if (local.is_file() and not local.is_symlink() and isinstance(record, dict) and
                record.get('url') == url and record.get('sha256') == pack['sha256'] == digest(local)):
            continue
        tasks[relative] = url
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(fetch, url, 2 * 1024 * 1024): (relative, url)
                   for relative, url in tasks.items()}
        try:
            for count, future in enumerate(as_completed(futures), 1):
                relative, url = futures[future]
                data = future.result()
                if hashlib.sha256(data).hexdigest() != next(p['sha256'] for p in packs
                                            if f"gm-webaudiofont/{p['file']}" == relative):
                    raise ValueError(f'pinned gm pack checksum mismatch: {relative}')
                path, record = installer.download(relative, url, False, data)
                installer.receipt[path] = record
                if count % 25 == 0 or count == len(tasks):
                    installer.save()
                    print(f'  {count}/{len(tasks)} gm packs verified', flush=True)
        finally:
            for future in futures:
                future.cancel()
            installer.save()
    credit = gm_lock.get('license', {})
    if (not isinstance(credit, dict) or not re.fullmatch(r'[A-Za-z0-9_.-]+', credit.get('file', '')) or
            not re.fullmatch(r'[0-9a-f]{64}', credit.get('sha256', ''))):
        raise ValueError('invalid gm pack license entry')
    license_url = f"https://raw.githubusercontent.com/{source['repo']}/{source['commit']}/{quote(credit['file'], safe='/')}"
    path, record = installer.download(f"licenses/webaudiofontdata/{credit['file']}", license_url)
    if record['sha256'] != credit['sha256']:
        (banks_root / path).unlink(missing_ok=True)
        raise ValueError('pinned gm pack license checksum mismatch')
    installer.receipt[path] = record
    installer.save()


def distro():
    release = platform.freedesktop_os_release()
    names = (release.get('ID', '') + ' ' + release.get('ID_LIKE', '')).split()
    for name in names:
        if name in ('arch', 'omarchy', 'manjaro', 'endeavouros'):
            return 'arch', release
        if name in ('debian', 'ubuntu', 'linuxmint', 'pop'):
            return 'debian', release
        if name == 'fedora':
            return 'fedora', release
    raise ValueError('Supported package managers: Arch/Omarchy, Debian/Ubuntu and Fedora. Use --skip-deps after installing the documented dependencies elsewhere.')


def dependency_command(family, release, browser=True):
    packages = PACKAGES[family].copy()
    if browser and not any(shutil.which(name) for name in ('chromium', 'chromium-browser', 'google-chrome', 'google-chrome-stable')):
        packages.append('chromium-browser' if family == 'debian' and release.get('ID') in ('ubuntu', 'pop') else 'chromium')
    command = {'arch': ['pacman', '-S', '--needed', '--noconfirm'], 'debian': ['apt-get', 'install', '-y'], 'fedora': ['dnf', 'install', '-y']}[family]
    return command + packages


def run(command, **kwargs):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def install_tree_sitter(prefix, jobs):
    archive = fetch(TREE_URL, 8 * 1024 * 1024)
    if hashlib.sha256(archive).hexdigest() != TREE_SHA:
        raise ValueError('tree-sitter archive checksum mismatch')
    import io
    with tempfile.TemporaryDirectory(prefix='tompouce-tree-') as temporary:
        root = Path(temporary)
        with tarfile.open(fileobj=io.BytesIO(archive), mode='r:gz') as tar:
            for member in tar:
                relative = safe_relative(member.name)
                parts = relative.parts[1:]
                if not parts or parts[0] not in ('lib', 'Makefile', 'LICENSE'):
                    continue
                path = root.joinpath(*parts)
                if member.isdir():
                    directory(path)
                elif member.isfile() and member.size <= 16 * 1024 * 1024:
                    file = tar.extractfile(member)
                    if file is None:
                        raise ValueError('missing tree-sitter archive file')
                    with file:
                        atomic_bytes(path, file.read())
                else:
                    raise ValueError('unsafe tree-sitter archive entry')
        directory(prefix)
        run(['make', f'-j{jobs}', f'PREFIX={prefix}', 'install'], cwd=root)
        shutil.copy2(root / 'LICENSE', prefix / 'LICENSE')


def copy_runtime(build, application, binary=None):
    # Explicit allowlist: do not copy accounts, credentials, caches or personal state.
    for relative in RUNTIME_FILES:
        source = binary if relative == 'tompouce' and binary is not None else build / relative
        atomic_bytes(application / relative, source.read_bytes(), 0o755 if relative == 'tompouce' else 0o644)
    for name in ('arpoon.json', 'belldub.json', 'bassfugue.json', 'flatrave.json', 'juxundtollerei.json', 'strudeltutor.json'):
        if (SOURCE / name).is_file():
            atomic_bytes(application / 'examples' / name, (SOURCE / name).read_bytes())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, default=Path.home() / '.local', help='installation prefix (default: ~/.local)')
    banks = parser.add_mutually_exclusive_group()
    banks.add_argument('--all-banks', dest='all_banks', action='store_true', help='install all REPL catalogs and GM variants (default; about 3 GiB)')
    banks.add_argument('--core-banks', dest='all_banks', action='store_false', help='smaller install without VCSL, mridangam or additional GM variants')
    parser.set_defaults(all_banks=True)
    parser.add_argument('--skip-deps', action='store_true', help='use already installed system build dependencies')
    parser.add_argument('--skip-banks', action='store_true', help='build/install without downloading banks')
    parser.add_argument('--no-browser', action='store_true', help='do not install Chromium')
    parser.add_argument('--csound-prefix', type=Path, help='reuse an existing Csound 7 installation')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 4), help='parallel build/download jobs, 1..16')
    parser.add_argument('--dry-run', action='store_true', help='print the plan without changes or downloads')
    args = parser.parse_args(argv)
    if sys.version_info < (3, 10):
        parser.error('Python 3.10 or newer is required')
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        parser.error('this installer requires Linux x86_64 (pinned Csound 7 distribution)')
    if not 1 <= args.jobs <= 16:
        parser.error('--jobs must be 1..16')
    prefix = args.prefix.expanduser().absolute()
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', str(prefix)) or '..' in prefix.parts:
        parser.error('use an absolute prefix without spaces, shell metacharacters or .. (private dependency build path limitation)')
    application = prefix / 'lib/tompouce'
    family, release = distro() if not args.skip_deps else (None, {})
    csound = (args.csound_prefix.expanduser().absolute() if args.csound_prefix else application / 'deps/csound7')
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', str(csound)) or '..' in csound.parts:
        parser.error('unsafe Csound prefix')
    lock = json.loads((SOURCE / 'scripts/banks.lock.json').read_text())
    print(f'Install location: {application}\nCommand: {prefix / "bin/tompouce"}', flush=True)
    print('Banks: ' + ('skipped (--skip-banks)' if args.skip_banks else 'all REPL catalogs + GM variants (~3 GiB)' if args.all_banks else 'core drums, piano, drum machines, wavetables and first GM fonts (~473 MiB)'))
    if family:
        print('System packages: ' + ' '.join(dependency_command(family, release, not args.no_browser)))
    if args.dry_run:
        print('Plan: dependencies → private Csound 7 → compatible tree-sitter → isolated CMake/Ninja build → pinned banks + licenses → install → MCP smoke check')
        for item in lock['catalogs']:
            if not args.skip_banks and (item['profile'] == 'core' or args.all_banks):
                print('  bank: ' + item['name'])
        if not args.skip_banks:
            print('  bank: gm webaudiofont packs')
        return 0
    launcher = prefix / 'bin/tompouce'
    if launcher.exists() and not launcher.is_symlink():
        raise ValueError(f'{launcher} already exists as a regular file; move it before installing')
    required = ['CMakeLists.txt', 'cmake/Embed.cmake', 'strudeltutor.json', 'src/main.c', 'thirdparty/quickjs/quickjs.c', 'js/dist/runtime.js', 'js/dist/completions.h', 'tests/test_install_runtime.c']
    required += [relative for relative in RUNTIME_FILES if relative != 'tompouce']
    for relative in required:
        if not (SOURCE / relative).is_file():
            raise ValueError(f'missing distribution file: {relative}; use the complete project tree')
    directory(application)
    guard_fd = os.open(application / '.install.lock',os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC,0o600)
    with os.fdopen(guard_fd,'a') as guard:
        fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if family:
            privilege = [] if os.geteuid() == 0 else ['sudo']
            if privilege and not shutil.which('sudo'):
                raise ValueError('sudo is needed to install system packages; alternatively install them as root and use --skip-deps')
            if family == 'debian':
                run(privilege + ['apt-get', 'update'])
            run(privilege + dependency_command(family, release, not args.no_browser))
        run(['pkg-config', '--atleast-version=3.0.9', 'notcurses-core'])
        run(['pkg-config', '--exists', 'notcurses-core', 'portaudio-2.0', 'libcurl', 'libcjson', 'alsa'])
        if args.csound_prefix:
            header = csound / 'include/csound/version.h'
            if not header.is_file() or not re.search(r'#define\s+CS_VERSION\s+7\b', header.read_text()):
                raise ValueError('--csound-prefix must contain Csound 7 headers and libraries')
        elif not all((csound / name).is_file() for name in ('.tompouce-beta18', 'include/csound/version.h', 'lib/libcsound64.so')):
            directory(csound)
            run(['bash', SOURCE / 'scripts/install-csound7.sh', csound])
            atomic_bytes(csound / '.tompouce-beta18', b'7.0.0-beta.18\n')
        environment = os.environ.copy()
        tree = application / 'deps/tree-sitter'
        version = subprocess.run(['pkg-config', '--modversion', 'tree-sitter'], capture_output=True, text=True)
        match = re.match(r'(\d+)\.(\d+)', version.stdout)
        needs_tree = version.returncode != 0 or not match or tuple(map(int, match.groups())) < (0, 25)
        if needs_tree:
            if not all((tree / name).is_file() for name in ('include/tree_sitter/api.h', 'lib/libtree-sitter.so', 'lib/pkgconfig/tree-sitter.pc', 'LICENSE')):
                install_tree_sitter(tree,args.jobs)
            environment['PKG_CONFIG_PATH'] = str(tree / 'lib/pkgconfig') + os.pathsep + environment.get('PKG_CONFIG_PATH', '')
        with tempfile.TemporaryDirectory(prefix='tompouce-build-') as temporary:
            build = Path(temporary)
            # Each installation has a new source root. Normalize compiler paths
            # within it so unchanged sources can reuse the user's ccache.
            environment.setdefault('CCACHE_BASEDIR', str(build))
            for name in ('CMakeLists.txt', 'README.md', 'LICENSE', 'NOTICE', 'strudeltutor.json'):
                shutil.copy2(SOURCE / name, build / name)
            ignore = shutil.ignore_patterns('*.o', '*.d', '*.inc', 'node_modules', '__pycache__', '.git')
            for relative in ('cmake', 'src', 'include', 'thirdparty', 'js/dist', 'web'):
                shutil.copytree(SOURCE / relative, build / relative, ignore=ignore)
            directory(build / 'tests')
            shutil.copy2(SOURCE / 'tests/test_install_runtime.c',build / 'tests/test_install_runtime.c')
            binary_dir = build / 'build'
            command = ['cmake', '-S', str(build), '-B', str(binary_dir), '-G', 'Ninja',
                       '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF', f'-DCSOUND_PREFIX={csound}']
            if needs_tree:
                command.append(f'-DTREE_SITTER_PREFIX={tree}')
            run(command, cwd=build, env=environment)
            run(['cmake', '--build', binary_dir, '--parallel', str(args.jobs),
                 '--target', 'tompouce', 'test_install_runtime'], cwd=build, env=environment)
            if not args.skip_banks:
                available = shutil.disk_usage(application).free
                minimum = (128 * 1024 ** 2 if (application / 'assets/banks/.install-receipt.json').exists()
                           else (4 if args.all_banks else 1) * 1024 ** 3)
                if available < minimum:
                    raise ValueError('not enough free disk space for the selected sound banks')
                BankInstaller(application / 'assets/banks',args.jobs).install(lock,args.all_banks)
                install_gm_webaudiofont(application / 'assets/banks',args.jobs,args.all_banks)
            check_home = directory(build / 'check-home')
            check_env = dict(environment, HOME=str(check_home), XDG_CONFIG_HOME=str(check_home), XDG_CACHE_HOME=str(check_home),
                             TOMPOUCE_BANKS=str(application / 'assets/banks'), https_proxy='http://127.0.0.1:9',
                             HTTP_PROXY='http://127.0.0.1:9', ALL_PROXY='http://127.0.0.1:9', NO_PROXY='', no_proxy='')
            run([binary_dir / 'tests/test_install_runtime'] + (['--no-banks'] if args.skip_banks else []),cwd=build,env=check_env)
            directory(build / 'scripts')
            for name in ('companion.py', 'sound_import.py'):
                shutil.copy2(SOURCE / 'scripts' / name, build / 'scripts' / name)
            copy_runtime(build,application,binary_dir / 'tompouce')
        bin_dir = directory(prefix / 'bin')
        launcher = bin_dir / 'tompouce'
        if launcher.exists() and not launcher.is_symlink():
            raise ValueError(f'{launcher} already exists as a regular file; move it before installing')
        temporary_link = bin_dir / ('.tompouce-' + secrets.token_hex(8))
        try:
            temporary_link.symlink_to(application / 'tompouce')
            os.replace(temporary_link,launcher)
        finally:
            temporary_link.unlink(missing_ok=True)
        requests = [{'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {}}]
        if not args.skip_banks:
            for number, category, query in [(2, 'samples', 'piano'), (3, 'drum-machines', 'RolandTR909_sd')]:
                requests.append({'jsonrpc': '2.0', 'id': number, 'method': 'tools/call', 'params': {
                    'name': 'music_sounds', 'arguments': {'category': category, 'query': query, 'limit': 1}}})
        with tempfile.TemporaryDirectory(prefix='tompouce-smoke-') as smoke_root:
            smoke_env = dict(os.environ, HOME=smoke_root, XDG_CONFIG_HOME=smoke_root, XDG_CACHE_HOME=smoke_root,
                             TOMPOUCE_BANKS=str(application / 'assets/banks'))
            smoke = subprocess.run([application / 'tompouce', '--mcp'],
                input=''.join(json.dumps(request)+'\n' for request in requests), capture_output=True,
                text=True, timeout=30, cwd=smoke_root, env=smoke_env)
        replies = {reply.get('id'): reply for reply in map(json.loads, smoke.stdout.splitlines())}
        if smoke.returncode or replies.get(1, {}).get('result', {}).get('serverInfo', {}).get('name') != 'tompouce':
            raise ValueError('installed engine failed its MCP startup check: ' + smoke.stderr[-1000:])
        if not args.skip_banks:
            for number in (2, 3):
                result = replies.get(number, {}).get('result', {})
                if result.get('isError') or not result.get('content') or not json.loads(result['content'][0]['text']).get('items'):
                    raise ValueError('installed piano/drum-machine catalog failed its startup check')
    label = 'Installed (sound banks skipped)' if args.skip_banks else 'Ready'
    print(f'\n{label}: {launcher}\nCompanion: press F2, enter /companion, then Enter.\nExamples: {application / "examples"}')
    if str(prefix / 'bin') not in os.environ.get('PATH', '').split(os.pathsep):
        print(f'Your PATH does not include {prefix / "bin"}; use the absolute command above.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'Installation failed: {error}\nFix the issue and run the same command again; verified downloads are reused.', file=sys.stderr)
        raise SystemExit(1)
