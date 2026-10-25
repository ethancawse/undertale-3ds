"""DELTARUNE inventory and private build inputs. Read-only player data; no content in the source tree."""
from pathlib import Path
import json
import os
import re
import shutil
import stat
import struct

import products
from products import verify_game as vg
from home_art import RuntimeArt

PARTS = vg.GAMES['deltarune'].parts[1:]
UNKNOWN_MESSAGE = "This version hasn't been checked yet. Update the builder."


def audio_inventory():
    with Path(__file__).with_name('audio-deltarune.json').open(encoding='utf-8') as source:
        result = json.load(source)
    if result.get('schema') != 1 or result.get('game') != 'deltarune':
        raise ValueError('Invalid audio inventory')
    return result


def inventory_audio_errors(folder, parts, checked=None):
    """Cover constructed stream names too. Every selected chapter needs the complete shared music set."""
    errors = []
    checked = {} if checked is None else checked
    try:
        manifest = audio_inventory()
        for part in parts:
            data = folder / ('data.win' if part == 'root' else part + '/data.win')
            digest = vg.data_digest(data)
            record = manifest['parts'].get(part, {}).get(digest)
            if record is None:
                # The owner override permits a new FORM, with the structural SOND/AUDO checks below.
                if vg.ALLOW_UNKNOWN_DELTARUNE:
                    continue
                errors.append(part + '/audio inventory')
                continue
            entries = record['files'] + (manifest['shared'] if record['shared'] else [])
            for entry in entries:
                name, size, sha = entry['name'], entry['size'], entry['sha256']
                path = folder / name
                if (not isinstance(name, str) or '\\' in name or not path.resolve().is_relative_to(folder)
                        or not name.startswith(('mus/' if record['shared'] else part + '/', part + '/'))
                        or not isinstance(size, int) or size <= 0 or not re.fullmatch(r'[0-9a-f]{64}', sha)):
                    raise ValueError('Invalid audio inventory entry')
                if name in checked:
                    if checked[name]:
                        errors.append(name)
                    continue
                try:
                    checked[name] = path.stat().st_size != size or vg.data_digest(path) != sha
                    if checked[name]:
                        errors.append(name)
                except OSError:
                    checked[name] = True
                    errors.append(name)
    except (OSError, ValueError, KeyError, TypeError):
        errors.append('audio inventory')
    return errors


def numbers(parts):
    return ', '.join(p[len('chapter')] for p in PARTS if p in parts) or 'none'


def inventory(folder, allow_list=None):
    return vg.check_versions(vg.GAMES['deltarune'], str(folder),
                             vg.ALLOW_LIST + vg.DELTARUNE_DEMO_ALLOW_LIST if allow_list is None else allow_list)


def selection(folder, parts=None, *, allow_list=None, audio=True):
    """Validate precisely the requested subset, including holes; never turn [] into 'all'."""
    states = inventory(folder, allow_list)
    chosen = list(parts) if parts is not None else [p for p in PARTS if states[p][0] == 'ok']
    chosen = [p for p in chosen if p != 'root']
    if not chosen or len(set(chosen)) != len(chosen) or any(p not in PARTS for p in chosen):
        raise ValueError('Choose at least one installed chapter.')
    releases = None
    rows = vg.ALLOW_LIST + vg.DELTARUNE_DEMO_ALLOW_LIST if allow_list is None else allow_list
    for part in ['root'] + chosen:
        state, release = states[part]
        if state == 'unknown' and vg.ALLOW_UNKNOWN_DELTARUNE:
            continue
        if state != 'ok':
            raise ValueError(UNKNOWN_MESSAGE if state == 'unknown' else
                             ('The launcher is missing.' if part == 'root' else
                              'Chapter %s is missing or unreadable.' % part[len('chapter')]))
        path = Path(folder) / ('data.win' if part == 'root' else part + '/data.win')
        digest = vg.data_digest(path)
        labels = {row[5] for row in rows if row[0] == 'deltarune' and row[1] == part
                  and row[2] == path.stat().st_size and row[3] == digest}
        releases = labels if releases is None else releases & labels
    if releases is not None and not releases:
        raise ValueError('These chapters are from different versions. Verify the game files in Steam.')
    if audio:
        errors = audio_errors(folder, ['root'] + chosen)
        if errors:
            raise ValueError('Missing or unreadable audio: %s. Verify the game files in Steam.' % errors[0])
    return [p for p in PARTS if p in chosen]


def runtime_selection(folder, parts, target='o3ds'):
    """Both compact runtime guards support verified subsets, including gaps."""
    return selection(folder, parts, audio=False)


def audio_errors(folder, parts, checked=None):
    """Follow each selected FORM's SOND references, including shared ../mus streams and AUDO indices."""
    folder = Path(folder).resolve()
    errors = inventory_audio_errors(folder, parts, checked)
    for part in parts:
        data = folder / ('data.win' if part == 'root' else part + '/data.win')
        try:
            with RuntimeArt(data) as form:
                groups = {}
                for ptr in form.pointers(b'SOND'):
                    flags = form.u32(ptr + 4)
                    group, index = struct.unpack_from('<ii', form.data, ptr + 28)
                    embedded = flags & 0x64 != 0x64 or flags & 3
                    if not embedded:
                        address = form.u32(ptr + 12)
                        end = form.data.find(b'\0', address)
                        if end < address:
                            raise ValueError('Invalid audio name')
                        name = form.data[address:end].decode('utf-8')
                        if '.' not in name:
                            name += '.ogg'
                        path = (data.parent / name.replace('\\', '/')).resolve()
                        if not path.is_relative_to(folder) or not path.is_file() or path.stat().st_size == 0:
                            errors.append(part + '/' + name)
                    elif index >= 0:
                        if group not in groups:
                            if group == 0:
                                groups[group] = (len(form.pointers(b'AUDO')), None)
                            elif group > 0:
                                path = data.with_name('audiogroup%d.dat' % group)
                                with RuntimeArt(path) as audio:
                                    entries = audio.pointers(b'AUDO')
                                    for entry in entries:
                                        size = audio.u32(entry)
                                        if not size or entry + 4 + size > len(audio.data):
                                            raise ValueError('Invalid audio group')
                                    groups[group] = (len(entries), path.name)
                            else:
                                raise ValueError('Invalid audio group')
                        if index >= groups[group][0]:
                            raise ValueError('Invalid audio index')
        except (OSError, ValueError, KeyError, IndexError, struct.error) as exc:
            # Report a relative dependency, never a personal path or an extracted asset name.
            missing = getattr(exc, 'filename', None) or getattr(exc.__cause__, 'filename', None)
            errors.append(part + '/' + (Path(missing).name if missing else 'audio data'))
    return sorted(set(errors))


def stage_files(folder, parts):
    return [(src, relative, size) for src, relative, size in
            products.sd_files(products.PRODUCTS['deltarune'], str(folder), parts)
            if not any(p in ('cache', 'saves') for p in Path(relative).parts)
            and (not Path(relative).parts[0].startswith('chapter') or Path(relative).parts[0] in parts)]


def stage_bytes(folder, parts):
    return sum(size for _, _, size in stage_files(folder, parts if parts is not None else PARTS))


def _remove_staged(function, path, exc):
    # Only the private staging tree is passed here. Never change a player's file attributes.
    if not isinstance(exc[1], PermissionError):
        raise exc[1]
    if os.stat(path, follow_symlinks=False).st_nlink > 1:
        raise ValueError('The staged files are shared and read-only. Choose another work folder.')
    os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
    function(path)


def stage(folder, work, parts):
    """Mirror only approved files. Each new stage replaces the previous one, so deselection cannot leave AOT inputs."""
    chosen = selection(folder, parts)
    dest = Path(work).resolve() / 'data' / 'deltarune'
    dest.parent.mkdir(parents=True, exist_ok=True)
    # This is our dedicated private staging directory, never the player's source folder.
    if (dest == Path(folder).resolve() or dest in Path(folder).resolve().parents
            or Path(folder).resolve() in dest.parents):
        raise ValueError('Choose a work folder outside the game folder.')
    if dest.exists():
        shutil.rmtree(dest, onerror=_remove_staged)
    dest.mkdir()
    need = stage_bytes(folder, chosen)
    if shutil.disk_usage(dest).free < need + 30 * 1024**2:
        raise ValueError('Not enough space for the selected chapters. Free up space and try again.')
    for src, relative, _size in stage_files(folder, chosen):
        target = dest / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, target)  # New writable files; Steam permissions and hard links stay at the source.
        stamp = os.stat(src)
        os.utime(target, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))  # Stable AOT/font fingerprints, writable copies.
    selection(dest, chosen)
    return str(dest), chosen
