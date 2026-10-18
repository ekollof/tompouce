"""Confined, persistent local sample imports shared by the UI and companion."""
import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat

MAX_FILE = 8 * 1024 * 1024
MAX_STORAGE = 256 * 1024 * 1024
EXTENSIONS = {".wav", ".mp3", ".flac"}


def sound_name(label):
    name = re.sub(r"[^a-z0-9_-]+", "_", label.lower()).strip("_-")[:55]
    return name if name and name[0].isalpha() else "sound_" + name


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
        if not isinstance(index, dict) or index.get("version") != 1 or not isinstance(index.get("sounds"), dict):
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


def import_folder(folder, parent_pid=None):
    root = Path(folder).absolute()
    if root.is_symlink() or not root.is_dir():
        raise ValueError("select a regular folder, not a symlink")
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
    parser.add_argument("--folder", required=True)
    parser.add_argument("--parent-pid", type=int)
    options = parser.parse_args()
    try:
        number = import_folder(options.folder, options.parent_pid)
        print(f"Imported {number} local audio files; available in User sounds.", flush=True)
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, f"Sound import failed: {error}\n")
