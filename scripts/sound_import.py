"""Confined, persistent local sample imports shared by the UI and companion.

Single files and flat folders import as before. A folder whose root holds a
strudel.json manifest imports as a bank instead: supported value forms are
single files, flat arrays and one-level pitch-keyed maps with top-level and
per-entry _base confined to the folder. The same bank flow imports pinned
`github:user/repo[/branch][/path]` manifests over HTTPS for offline use.
Nested JSON objects, remote bases and remote references are rejected; unknown
pitch keys are dropped. Manifest imports own the names they define (last
import wins per name); loose file imports into manifest-owned names are
refused. Reimports replace the bank's slots and collect unreferenced assets.
"""
import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import quote, urlsplit
from urllib.request import Request, build_opener, HTTPRedirectHandler
import re
import secrets
import stat

MAX_FILE = 8 * 1024 * 1024
MAX_STORAGE = 256 * 1024 * 1024
EXTENSIONS = {".wav", ".mp3", ".flac"}
MANIFEST_MAX_BYTES = 2 * 1024 * 1024
MANIFEST_MAX_REFS = 8192
MANIFEST_MAX_PATH = 1024
GITHUB_HOST = "raw.githubusercontent.com"
GITHUB_MAX_HOPS = 4
FETCH_TIMEOUT = 45

_CHROMA = {"c": 0, "d": 2, "e": 4, "f": 5, "g": 7, "a": 9, "b": 11}
_ACCIDENTAL = {"#": 1, "b": -1, "s": 1, "f": -1}
_NOTE_RE = re.compile(r"([a-gA-G])([#bsf]*)(-?[0-9]*)$")
_SCHEME_RE = re.compile(r"[A-Za-z][A-Za-z0-9+.-]*:")


def sound_name(label):
    name = re.sub(r"[^a-z0-9_-]+", "_", label.lower()).strip("_-")[:55]
    return name if name and name[0].isalpha() else "sound_" + name


def note_midi(key):
    """Upstream pitch-key grammar (tokenizeNote, default octave 3).

    Returns 0..127, or None for invalid/out-of-range keys, which manifest
    imports drop with the key documented as unsupported.
    """
    if not isinstance(key, str):
        return None
    match = _NOTE_RE.fullmatch(key)
    if not match or not match.group(1):
        return None
    pitch, accidentals, octave = match.groups()
    try:
        offset = sum(_ACCIDENTAL[char] for char in accidentals)
        number = 3 if octave == "" else int(octave)
    except ValueError:
        return None
    midi = (number + 1) * 12 + _CHROMA[pitch.lower()] + offset
    return midi if 0 <= midi <= 127 else None


def _check_rel(ref):
    """Validate a manifest-relative audio path without touching storage."""
    if (not isinstance(ref, str) or not 0 < len(ref) <= MANIFEST_MAX_PATH or
            any(ord(c) < 32 or ord(c) == 127 or c == "\\" for c in ref)):
        raise ValueError("invalid manifest audio path")
    if ref.startswith("/") or _SCHEME_RE.match(ref):
        raise ValueError("manifest audio paths stay inside the folder")
    parts = ref.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError("manifest audio paths stay inside the folder")
    return parts


def _check_base(base):
    """Validate a manifest _base as a relative subdirectory (or empty)."""
    if base is None:
        return []
    if not isinstance(base, str):
        raise ValueError("invalid manifest _base")
    if base == "":
        return []
    if base.startswith("/") or "\\" in base or _SCHEME_RE.match(base):
        raise ValueError("manifest _base stays inside the folder")
    parts = base.strip("/").split("/")
    if any(part in ("", ".", "..") or any(ord(c) < 32 or ord(c) == 127 for c in part) for part in parts):
        raise ValueError("manifest _base stays inside the folder")
    return parts


def _walk_parts(root, parts):
    """Open a validated segment list below root, refusing symlinks at use."""
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd, parts[-1]
    except BaseException:
        try:
            os.close(fd)
        except OSError:
            pass
        raise


def open_rooted(root, rel):
    """Read a validated relative path below root.

    Every component opens with O_NOFOLLOW, so symlinks, traversal and
    check/use races fail closed instead of escaping the folder.
    """
    parts = _check_rel(rel)
    fd, leaf = _walk_parts(root, parts)
    try:
        with os.fdopen(os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd), "rb") as file:
            metadata = os.fstat(file.fileno())
            if not stat.S_ISREG(metadata.st_mode):
                raise ValueError("manifest audio is not a regular file")
            if metadata.st_size > MAX_FILE:
                raise ValueError("manifest audio file exceeds 8 MiB")
            data = file.read(MAX_FILE + 1)
            if len(data) > MAX_FILE:
                raise ValueError("manifest audio file exceeds 8 MiB")
            return data
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def _validate_audio(label, data):
    """Shared header/size gate used by single-file and manifest imports."""
    extension = Path(label).suffix.lower()
    if extension not in EXTENSIONS or not 0 < len(data) <= MAX_FILE:
        raise ValueError("use WAV, MP3 or FLAC files up to 8 MiB")
    valid = ((extension == ".wav" and data[:4] == b"RIFF" and data[8:12] == b"WAVE") or
             (extension == ".flac" and data[:4] == b"fLaC") or
             (extension == ".mp3" and (data[:3] == b"ID3" or len(data) > 1 and data[0] == 255 and data[1] & 224 == 224)))
    if not valid:
        raise ValueError("audio header does not match its extension")
    return extension


class _GitHubRedirect(HTTPRedirectHandler):
    """HTTPS-only, same-host redirects with a hop budget (fail closed)."""

    def __init__(self, hosts):
        self.hosts = set(hosts)
        self.hops = 0

    def redirect_request(self, request, fp, code, msg, headers, newurl):
        parts = urlsplit(newurl)
        if (parts.scheme != "https" or parts.hostname not in self.hosts or
                parts.port is not None or "@" in parts.netloc):
            raise ValueError("refusing redirect outside raw.githubusercontent.com")
        self.hops += 1
        if self.hops > GITHUB_MAX_HOPS:
            raise ValueError("too many redirects")
        return super().redirect_request(request, fp, code, msg, headers, newurl)


def _http_get(url, maximum, hosts=(GITHUB_HOST,)):
    """Fetch a pinned-host URL with bounded redirects, time and size."""
    if urlsplit(url).scheme != "https":
        raise ValueError("manifest downloads require HTTPS")
    request = Request(url, headers={"User-Agent": "Tompouce-sound-import/1"})
    redirect = _GitHubRedirect(hosts)
    opener = build_opener(redirect)
    with opener.open(request, timeout=FETCH_TIMEOUT) as response:
        final = urlsplit(response.url)
        if final.scheme != "https" or final.hostname not in redirect.hosts:
            raise ValueError("refusing response outside raw.githubusercontent.com")
        declared = response.headers.get("Content-Length")
        if declared is not None and (not declared.isdigit() or int(declared) > maximum):
            raise ValueError(f"invalid download length: {url}")
        data = response.read(maximum + 1)
        if declared is not None and len(data) != int(declared):
            raise OSError("incomplete download")
    if len(data) > maximum:
        raise ValueError(f"download too large: {url}")
    return data


def parse_github_spec(spec):
    """Split github:user/repo[/branch][/path] like the web samples() form.

    Returns (manifest_url, base_url, bank_id). Defaults (repo samples, branch
    main) mirror upstream githubPath; every component is allowlisted.
    """
    if not isinstance(spec, str) or not spec.startswith("github:"):
        raise ValueError("expected a github:user/repo spec")
    parts = spec[len("github:"):].split("/")
    if (len(parts) < 1 or len(parts) > 6 or
            not all(part and len(part) <= 96 and re.fullmatch(r"[A-Za-z0-9_.-]+", part) for part in parts)):
        raise ValueError("invalid github bank spec")
    user = parts[0]
    repo = parts[1] if len(parts) >= 2 else "samples"
    branch = parts[2] if len(parts) >= 3 else "main"
    subpath = "/".join(quote(part, safe="") for part in parts[3:])
    base = f"https://{GITHUB_HOST}/{quote(user, safe='')}/{quote(repo, safe='')}/{quote(branch, safe='')}/"
    if subpath:
        base += subpath + "/"
    bank_id = "github:" + sound_name("/".join(parts))
    return base + "strudel.json", base, bank_id


def resolve_github_refs(base_url, groups, parent_pid=None):
    """Fetch and validate every referenced audio file (no side effects).

    Refs resolve under the manifest directory with the same confinement rules
    as local banks; every segment is validated, so joined URLs cannot escape
    the pinned user/repo/branch prefix. Returns the shared resolved mapping.
    """
    prefix = urlsplit(base_url).path
    if not prefix.endswith("/"):
        raise ValueError("invalid github base URL")
    ordered = [(name, item) for name, items in groups.items() for item in items]
    if len(ordered) > MANIFEST_MAX_REFS:
        raise ValueError("manifest references too many files")
    urls = []
    for name, item in ordered:
        segments = item["base"] + _check_rel(item["ref"])
        urls.append(base_url + "/".join(quote(part, safe="") for part in segments))
    for url in urls:
        if not urlsplit(url).path.startswith(prefix):
            raise ValueError("manifest reference escapes its directory")
    slots = [None] * len(ordered)
    total_bytes = 0
    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = {pool.submit(_http_get, url, MAX_FILE): index for index, url in enumerate(urls)}
        try:
            for future in as_completed(futures):
                if parent_pid is not None and os.getppid() != parent_pid:
                    raise ValueError("launching engine exited")
                index = futures[future]
                name, item = ordered[index][0], ordered[index][1]
                try:
                    data = future.result()
                except HTTPError as error:
                    if error.code == 404:
                        raise ValueError(f"unreadable audio: {item['ref']}")
                    raise
                total_bytes += len(data)
                if total_bytes > MAX_STORAGE:
                    raise ValueError("manifest audio exceeds 256 MiB")
                extension = _validate_audio(item["ref"], data)
                slots[index] = (_manifest_label(item["ref"]), item["midi"], item["region"], data, extension)
        finally:
            for future in futures:
                future.cancel()
    resolved = {}
    for (name, _), slot in zip(ordered, slots):
        resolved.setdefault(name, []).append(slot)
    return resolved


def import_github_bank(spec, parent_pid=None):
    """Import a github:user/repo[/branch][/path] manifest bank for offline use.

    Returns the imported slot count. Publication reuses the staged manifest
    bank path, so quotas, replacement, collection and failure behavior match
    local banks.
    """
    manifest_url, base_url, bank_id = parse_github_spec(spec)
    try:
        data = _http_get(manifest_url, MANIFEST_MAX_BYTES)
    except HTTPError as error:
        if error.code == 404:
            raise ValueError(f"github bank manifest not found: {spec}")
        raise
    groups, order = parse_manifest(data)
    if parent_pid is not None and os.getppid() != parent_pid:
        raise ValueError("launching engine exited")
    resolved = resolve_github_refs(base_url, groups, parent_pid)
    with SoundStore() as store:
        summary = store.import_manifest_bank(bank_id, order, resolved)
    return summary["slots"]


def _manifest_label(ref):
    """Stable slot label derived from the manifest-relative reference."""
    label = ref.replace("/", "_").replace("..", "__")
    if len(label) > 160:
        label = label[:150] + hashlib.sha256(ref.encode()).hexdigest()[:10]
    return label


def parse_manifest(data):
    """Normalize a strudel.json bank manifest without touching storage.

    Returns ({name: [{base, ref, midi|None, region}]}, [names in order]).
    Supported forms per name are single files, flat arrays and one-level
    pitch-keyed maps; deeper nesting, non-string entries, empty groups and
    invalid pitch keys are dropped (keys starting with _ are skipped like
    the web sounds tab). Raises ValueError when nothing importable remains.
    """
    if not data or len(data) > MANIFEST_MAX_BYTES:
        raise ValueError("invalid manifest size")

    def reject(value):
        raise ValueError(f"invalid manifest: {value}")

    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise ValueError("manifest is not UTF-8")
    try:
        doc = json.loads(text, parse_constant=reject)
    except (json.JSONDecodeError, RecursionError, ValueError) as error:
        raise ValueError(f"invalid manifest: {error}")
    if not isinstance(doc, dict):
        raise ValueError("manifest must be a JSON object")
    top_base = _check_base(doc.get("_base", ""))
    groups = {}
    order = []
    total_items = 0
    # Flat entries share region 0 so merged names wrap as one list; pitch-map
    # keys take globally unique regions so equal midis never merge runs.
    pitch_region = 1

    def add(name, base, ref, midi, region):
        nonlocal total_items
        total_items += 1
        if total_items > MANIFEST_MAX_REFS:
            raise ValueError("manifest references too many files")
        if name not in groups:
            groups[name] = []
            order.append(name)
        groups[name].append({"base": base, "ref": ref, "midi": midi, "region": region})

    def claim_pitch_region():
        nonlocal pitch_region
        region = pitch_region
        pitch_region += 1
        return region

    for key, value in doc.items():
        if key.startswith("_"):
            continue
        name = sound_name(key)
        if isinstance(value, str):
            for ref in [value]:
                add(name, top_base, ref, None, 0)
        elif isinstance(value, list):
            if not value or not all(isinstance(item, str) for item in value):
                continue
            for ref in value:
                add(name, top_base, ref, None, 0)
        elif isinstance(value, dict):
            try:
                entry_base = _check_base(value["_base"]) if "_base" in value else top_base
            except ValueError:
                continue
            keyed = False
            for note, samples in value.items():
                if note.startswith("_"):
                    continue
                midi = note_midi(note)
                if midi is None:
                    continue
                if isinstance(samples, str):
                    samples = [samples]
                if (not isinstance(samples, list) or not samples or
                        not all(isinstance(item, str) for item in samples)):
                    continue
                region = claim_pitch_region()
                for ref in samples:
                    add(name, entry_base, ref, midi, region)
                keyed = True
            if not keyed:
                continue
        else:
            continue
    # Drop names left empty by invalid entries only.
    groups = {name: items for name, items in groups.items() if items}
    if not groups:
        raise ValueError("manifest defines no importable sounds")
    return groups, [name for name in order if name in groups]


def resolve_manifest_refs(root, groups, parent_pid=None):
    """Read and validate every referenced audio file (no side effects).

    Returns {name: [(label, midi, region, data, extension)]}. Anything
    unreadable, oversized, non-audio or over quota raises before publication.
    """
    resolved = {}
    total_refs = 0
    total_bytes = 0
    for name, items in groups.items():
        slots = []
        for item in items:
            if parent_pid is not None and os.getppid() != parent_pid:
                raise ValueError("launching engine exited")
            parts = item["base"] + _check_rel(item["ref"])
            total_refs += 1
            if total_refs > MANIFEST_MAX_REFS:
                raise ValueError("manifest references too many files")
            try:
                fd, leaf = _walk_parts(root, parts)
            except OSError as error:
                raise ValueError(f"unreadable audio: {item['ref']}")
            try:
                leaf_fd = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
            except OSError as error:
                try:
                    os.close(fd)
                except OSError:
                    pass
                raise ValueError(f"unreadable audio: {item['ref']}")
            try:
                with os.fdopen(leaf_fd, "rb") as file:
                    metadata = os.fstat(file.fileno())
                    if not stat.S_ISREG(metadata.st_mode):
                        raise ValueError(f"not a regular file: {item['ref']}")
                    if metadata.st_size > MAX_FILE:
                        raise ValueError(f"file exceeds 8 MiB: {item['ref']}")
                    data = file.read(MAX_FILE + 1)
            finally:
                try:
                    os.close(fd)
                except OSError:
                    pass
            if len(data) > MAX_FILE:
                raise ValueError(f"file exceeds 8 MiB: {item['ref']}")
            total_bytes += len(data)
            if total_bytes > MAX_STORAGE:
                raise ValueError("manifest audio exceeds 256 MiB")
            extension = _validate_audio(item["ref"], data)
            slots.append((_manifest_label(item["ref"]), item["midi"], item["region"], data, extension))
        resolved[name] = slots
    return resolved


def _directory(path):
    """Create/open an absolute directory without following any symlinks."""
    if not path.is_absolute() or ".." in path.parts:
        raise ValueError("sound storage needs a safe absolute config directory")
    fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
    try:
        for part in path.parts[1:]:
            try:
                os.mkdir(part, 0o700, dir_fd=fd)
            except FileExistsError:
                pass
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


class SoundStore:
    def __init__(self):
        base = Path(os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config"))
        application = base / "tompouce"
        legacy = base / "strudelterm"
        if not os.path.lexists(application) and legacy.is_dir() and not legacy.is_symlink():
            rename = ctypes.CDLL(None, use_errno=True).renameat2
            rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
            rename.restype = ctypes.c_int
            if rename(-100, os.fsencode(legacy), -100, os.fsencode(application), 1) != 0:
                error = ctypes.get_errno()
                if error != errno.EEXIST:
                    raise OSError(error, os.strerror(error), str(legacy))
        self.root = _directory(application / "sounds")
        self.assets = self.lock = None
        try:
            try:
                os.mkdir("assets", 0o700, dir_fd=self.root)
            except FileExistsError:
                pass
            self.assets = os.open("assets", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=self.root)
            self.lock = os.open(".lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600, dir_fd=self.root)
        except BaseException:
            self.close()
            raise

    def close(self):
        for name in ["lock", "assets", "root"]:
            fd = getattr(self, name, None)
            if fd is not None:
                os.close(fd)
                setattr(self, name, None)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _index(self):
        try:
            fd = os.open("index.json", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=self.root)
        except FileNotFoundError:
            return {"version": 1, "sounds": {}}
        with os.fdopen(fd, "rb") as file:
            metadata = os.fstat(file.fileno())
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > 2 * 1024 * 1024:
                raise ValueError("invalid sound index")
            index = json.load(file)
        if (not isinstance(index, dict) or index.get("version") not in (1, 2)
                or not isinstance(index.get("sounds"), dict)):
            raise ValueError("invalid sound index")
        return index

    def import_file(self, name, label, data):
        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,62}", name):
            raise ValueError("sound name must start with a letter and use a-z, 0-9, _ or -")
        if (not isinstance(label, str) or not 0 < len(label) <= 160 or "/" in label or "\\" in label
                or ".." in label or any(ord(c) < 32 or ord(c) == 127 for c in label)):
            raise ValueError("invalid audio filename")
        extension = Path(label).suffix.lower()
        if extension not in EXTENSIONS or not 0 < len(data) <= MAX_FILE:
            raise ValueError("use WAV, MP3 or FLAC files up to 8 MiB")
        valid = ((extension == ".wav" and data[:4] == b"RIFF" and data[8:12] == b"WAVE") or
                 (extension == ".flac" and data[:4] == b"fLaC") or
                 (extension == ".mp3" and (data[:3] == b"ID3" or len(data) > 1 and data[0] == 255 and data[1] & 224 == 224)))
        if not valid:
            raise ValueError("audio header does not match its extension")
        filename = hashlib.sha256(data).hexdigest() + extension
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        temporary = asset_temporary = None
        try:
            index = self._index()
            groups = index["sounds"]
            if name not in groups and len(groups) >= 256:
                raise ValueError("256 imported sound names limit reached")
            entries = groups.setdefault(name, [])
            if not isinstance(entries, list) or any(not isinstance(item, dict) or not isinstance(item.get("label"), str) for item in entries):
                raise ValueError("invalid sound index entries")
            if any(item.get("bank") for item in entries):
                raise ValueError("sound is owned by an imported bank; reimport the bank instead")
            updated = [item for item in entries if item["label"] != label] + [{"label": label, "file": filename}]
            if len(updated) > 1024 or sum(len(v) for v in groups.values()) - len(entries) + len(updated) > 8192:
                raise ValueError("imported sound slot limit reached")
            groups[name] = sorted(updated, key=lambda item: (item["label"].casefold(), item["label"]))
            payload = json.dumps(index, ensure_ascii=True, sort_keys=True).encode()
            if len(payload) >= 2 * 1024 * 1024:
                raise ValueError("imported sound index limit reached")
            existing = os.listdir(self.assets)
            used = sum(os.stat(item, dir_fd=self.assets, follow_symlinks=False).st_size for item in existing)
            if filename not in existing:
                if len(existing) >= 2048 or used + len(data) > MAX_STORAGE:
                    raise ValueError("sound storage limit reached (2048 files / 256 MiB)")
                asset_temporary = ".asset-" + secrets.token_hex(12)
                fd = os.open(asset_temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.assets)
                with os.fdopen(fd, "wb") as file:
                    file.write(data)
                    file.flush()
                    os.fsync(file.fileno())
                os.replace(asset_temporary, filename, src_dir_fd=self.assets, dst_dir_fd=self.assets)
                asset_temporary = None
                os.fsync(self.assets)
            else:
                metadata = os.stat(filename, dir_fd=self.assets, follow_symlinks=False)
                if not stat.S_ISREG(metadata.st_mode):
                    raise ValueError("linked or invalid sound asset")
            temporary = ".index-" + secrets.token_hex(12)
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.root)
            with os.fdopen(fd, "wb") as file:
                file.write(payload)
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, "index.json", src_dir_fd=self.root, dst_dir_fd=self.root)
            temporary = None
            os.fsync(self.root)
            return {"name": name, "slots": len(updated)}
        finally:
            if temporary is not None:
                os.unlink(temporary, dir_fd=self.root)
            if asset_temporary is not None:
                os.unlink(asset_temporary, dir_fd=self.assets)
            fcntl.flock(self.lock, fcntl.LOCK_UN)

    def import_manifest_bank(self, bank_id, order, resolved):
        """Publish a validated manifest bank atomically.

        resolved maps names to [(label, midi|None, region, data, extension)]
        with all audio already read and header-checked. Groups named here are
        replaced wholesale (previous slots from any origin are dropped);
        reimports also drop this bank's slots from names it no longer defines.
        Assets unreferenced afterwards are collected. Any failure before the
        index swap preserves the previous bank and unlinks staged assets.
        """
        if not re.fullmatch(r"(manifest|github):[a-z0-9_-]{1,55}", bank_id):
            raise ValueError("invalid manifest bank identity")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        created = []
        temporary = None
        try:
            index = self._index()
            groups = index["sounds"]
            staged = {}
            for name in order:
                slots = []
                for label, midi, region, data, extension in resolved[name]:
                    filename = hashlib.sha256(data).hexdigest() + extension
                    slot = {"label": label, "file": filename}
                    if midi is not None:
                        slot["midi"] = midi
                    if region:
                        slot["region"] = region
                    slot["bank"] = bank_id
                    slots.append((slot, data))
                staged[name] = slots
            final = {name: [slot for slot, _ in slots] for name, slots in staged.items()}
            for name, entries in groups.items():
                if name in final or not isinstance(entries, list):
                    continue
                kept = [item for item in entries
                        if not isinstance(item, dict) or item.get("bank") != bank_id]
                if kept:
                    final[name] = kept
            if len(final) > 256:
                raise ValueError("256 imported sound names limit reached")
            total = 0
            for entries in final.values():
                if len(entries) > 1024:
                    raise ValueError("imported sound slot limit reached")
                total += len(entries)
            if total > 8192:
                raise ValueError("imported sound slot limit reached")
            payload = json.dumps({"version": 2, "sounds": final}, ensure_ascii=True, sort_keys=True).encode()
            if len(payload) >= 2 * 1024 * 1024:
                raise ValueError("imported sound index limit reached")
            existing = os.listdir(self.assets)
            used = sum(os.stat(item, dir_fd=self.assets, follow_symlinks=False).st_size for item in existing)
            staged_count = 0
            wanted = {}
            for slots in staged.values():
                for slot, data in slots:
                    wanted.setdefault(slot["file"], len(data))
            for filename, size in wanted.items():
                if filename in existing:
                    metadata = os.stat(filename, dir_fd=self.assets, follow_symlinks=False)
                    if not stat.S_ISREG(metadata.st_mode):
                        raise ValueError("linked or invalid sound asset")
                    continue
                if len(existing) + staged_count >= 2048 or used + size > MAX_STORAGE:
                    raise ValueError("sound storage limit reached (2048 files / 256 MiB)")
                asset_temporary = ".asset-" + secrets.token_hex(12)
                fd = os.open(asset_temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600,
                             dir_fd=self.assets)
                with os.fdopen(fd, "wb") as file:
                    file.write(data)
                    file.flush()
                    os.fsync(file.fileno())
                os.replace(asset_temporary, filename, src_dir_fd=self.assets, dst_dir_fd=self.assets)
                created.append(filename)
                staged_count += 1
                used += size
            os.fsync(self.assets)
            temporary = ".index-" + secrets.token_hex(12)
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=self.root)
            with os.fdopen(fd, "wb") as file:
                file.write(payload)
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, "index.json", src_dir_fd=self.root, dst_dir_fd=self.root)
            temporary = None
            os.fsync(self.root)
            referenced = set()
            for entries in final.values():
                for item in entries:
                    if isinstance(item, dict) and isinstance(item.get("file"), str):
                        referenced.add(item["file"])
            for filename in os.listdir(self.assets):
                if filename not in referenced:
                    try:
                        os.unlink(filename, dir_fd=self.assets)
                    except OSError:
                        pass
            os.fsync(self.assets)
            return {"bank": bank_id, "names": len(staged), "slots": sum(len(v) for v in staged.values())}
        except BaseException:
            for filename in created:
                try:
                    os.unlink(filename, dir_fd=self.assets)
                except OSError:
                    pass
            raise
        finally:
            if temporary is not None:
                try:
                    os.unlink(temporary, dir_fd=self.root)
                except OSError:
                    pass
            fcntl.flock(self.lock, fcntl.LOCK_UN)


def import_manifest_bank(folder, parent_pid=None):
    """Import a folder whose root holds strudel.json as a manifest bank.

    Returns the imported slot count. Bank identity derives from the folder
    name; names the manifest defines are replaced wholesale, and reimports
    drop this bank's slots from names it no longer defines.
    """
    root = Path(folder).absolute()
    if root.is_symlink() or not root.is_dir():
        raise ValueError("select a regular folder, not a symlink")
    bank_id = "manifest:" + sound_name(root.name)
    data = open_rooted(str(root), "strudel.json")
    groups, order = parse_manifest(data)
    if parent_pid is not None and os.getppid() != parent_pid:
        raise ValueError("launching engine exited")
    resolved = resolve_manifest_refs(str(root), groups, parent_pid)
    with SoundStore() as store:
        summary = store.import_manifest_bank(bank_id, order, resolved)
    return summary["slots"]


def import_folder(folder, parent_pid=None):
    if isinstance(folder, str) and folder.startswith("github:"):
        # F6 passes the typed string through --folder; github specs import
        # the pinned manifest bank instead of a local directory. (A local
        # folder literally named github:* is unreachable this way; rename it.)
        return import_github_bank(folder, parent_pid)
    root = Path(folder).absolute()
    if root.is_symlink() or not root.is_dir():
        raise ValueError("select a regular folder, not a symlink")
    try:
        has_manifest = stat.S_ISREG(os.lstat(root / "strudel.json").st_mode)
    except OSError:
        has_manifest = False
    if has_manifest:
        # A regular root manifest defines the bank; invalid content fails
        # here rather than silently falling back to a flat audio walk.
        return import_manifest_bank(folder, parent_pid)
    imported = 0
    with SoundStore() as store:
        for current, dirs, files, directory_fd in os.fwalk(root, follow_symlinks=False):
            dirs[:] = sorted(d for d in dirs if not (Path(current) / d).is_symlink())
            for label in sorted(files, key=lambda value: (value.casefold(), value)):
                if parent_pid is not None and os.getppid() != parent_pid:
                    raise ValueError("launching engine exited")
                if Path(label).suffix.lower() not in EXTENSIONS:
                    continue
                if imported >= 2048:
                    raise ValueError("folder import exceeds 2048 files")
                if (Path(current) / label).is_symlink():
                    continue
                fd = os.open(label, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory_fd)
                with os.fdopen(fd, "rb") as file:
                    metadata = os.fstat(file.fileno())
                    if not stat.S_ISREG(metadata.st_mode):
                        continue
                    if metadata.st_size > MAX_FILE:
                        raise ValueError(f"{label}: file exceeds 8 MiB")
                    data = file.read(MAX_FILE + 1)
                store.import_file(sound_name(Path(current).name), label, data)
                imported += 1
    return imported


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--folder", help="local audio folder (strudel.json at its root imports as a bank)")
    parser.add_argument("--github", help="github:user/repo[/branch][/path] manifest bank for offline use")
    parser.add_argument("--parent-pid", type=int)
    options = parser.parse_args()
    if bool(options.folder) == bool(options.github):
        parser.error("pass exactly one of --folder or --github")
    try:
        if options.github:
            number = import_github_bank(options.github, options.parent_pid)
        else:
            number = import_folder(options.folder, options.parent_pid)
        print(f"Imported {number} local audio files; available in User sounds.", flush=True)
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, f"Sound import failed: {error}\n")
