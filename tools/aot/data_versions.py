"""Shared size/SHA-256 allowlist check. No game bytes are stored or uploaded."""
import hashlib
import json
from pathlib import Path
import re

TABLE = Path(__file__).resolve().parents[2] / "platform/3ds/data_versions.inc"


def load_table():
    text = TABLE.read_text(encoding="utf-8")
    revision = int(re.search(r"^#define DATA_VERSIONS_REVISION (\d+)$", text, re.M).group(1))
    rows = re.findall(r'DATA_VERSION\("([^"]+)", "([^"]+)", (\d+)ULL, "([0-9a-f]{64})"\)', text)
    return revision, {(game, part, int(size), digest) for game, part, size, digest in rows}


def check_file(path, game, part, revision, known, cache):
    path = Path(path)
    stat = path.stat()
    key = str(path.resolve())
    identity = [revision, stat.st_size, stat.st_mtime_ns]
    previous = cache.get(key, {})
    digest = previous.get("sha256", "") if isinstance(previous, dict) else ""
    cached = isinstance(previous, dict) and previous.get("identity") == identity and re.fullmatch(r"[0-9a-f]{64}", digest) is not None
    if not cached:
        sha = hashlib.sha256()
        with path.open("rb") as data:
            for block in iter(lambda: data.read(1024 * 1024), b""):
                sha.update(block)
        after = path.stat()
        if (stat.st_size, stat.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise OSError("file changed while checking")
        digest = sha.hexdigest()
        cache[key] = {"identity": identity, "sha256": digest}
    return (game, part, stat.st_size, digest) in known, cached, digest


def check_folder(game, folder, cache_folder):
    revision, known = load_table()
    cache_file = Path(cache_folder) / "data_versions.json"
    try:
        cache = json.loads(cache_file.read_text(encoding="utf-8"))
        if not isinstance(cache, dict): cache = {}
    except (OSError, ValueError):
        cache = {}
    files = [("root", Path(folder) / "data.win")]
    if game == "deltarune":
        files += [(f"chapter{i}_windows", Path(folder) / f"chapter{i}_windows/data.win") for i in range(1, 6)]
    for part, path in files:
        # A build may intentionally use a subset of the chapters.
        if not path.exists(): continue
        try:
            good, cached, digest = check_file(path, game, part, revision, known, cache)
            if good:
                print(f"Data version: {game}/{part} known (table {revision}, {'cached' if cached else 'hashed'})")
            else:
                print(f"WARNING: Unknown data.win: {path} (SHA-256 {digest}). This file version has not been checked. "
                      "It may work; continuing is allowed. If it fails, verify your PC game files and copy data.win again.")
        except OSError as error:
            print(f"WARNING: Could not verify data.win: {path}: {error}. Continuing is allowed.")
    try:
        cache_file.parent.mkdir(parents=True, exist_ok=True)
        cache_file.write_text(json.dumps(cache, indent=2) + "\n", encoding="utf-8")
    except OSError:
        pass  # Optional cache; the next build hashes again if it could not be saved.
