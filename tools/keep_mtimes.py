#!/usr/bin/env python3
"""Keeps the build incremental across a re-prepared tree (tools/container-build.sh).

A full build resets the Chromium checkout and applies every patch again, which writes each
patched or copied file anew: ninja then sees thousands of changed files though their content is
what it built last time. `save` records the content and time of each file that git sees as
changed or new in REPO...; `restore`, after the tree is prepared again, gives a file whose content
is the same its old time back. A file whose content changed keeps its new time and is rebuilt.

  keep_mtimes.py save STATE REPO...
  keep_mtimes.py restore STATE REPO...
"""
import hashlib, json, os, subprocess, sys
from pathlib import Path


def changed_files(repo):
    """The regular files of REPO that git sees as modified or untracked, ignored ones left out."""
    out = subprocess.run(['git', '-C', repo, 'status', '--porcelain', '-z', '--untracked-files=all'],
                         check=True, capture_output=True).stdout.decode()
    entries = out.split('\0')
    files = []
    i = 0
    while i < len(entries):
        entry = entries[i]
        i += 1
        if len(entry) < 4:
            continue
        # A rename's entry is followed by its old path, which isn't in the tree.
        if entry[0] in 'RC':
            i += 1
        path = Path(repo) / entry[3:]
        if path.is_file() and not path.is_symlink():
            files.append(path)
    return files


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(state, repos):
    files = {}
    for repo in repos:
        if Path(repo, '.git').exists():
            for path in changed_files(repo):
                files[str(path)] = [digest(path), path.stat().st_mtime_ns]
    Path(state).write_text(json.dumps(files))
    print(f'keep_mtimes: recorded {len(files)} files')


def restore(state, repos):
    try:
        saved = json.loads(Path(state).read_text())
    except (OSError, ValueError):
        print('keep_mtimes: nothing recorded')
        return
    kept = 0
    for repo in repos:
        if not Path(repo, '.git').exists():
            continue
        for path in changed_files(repo):
            old = saved.get(str(path))
            if old and digest(path) == old[0]:
                os.utime(path, ns=(old[1], old[1]))
                kept += 1
    print(f'keep_mtimes: {kept} unchanged files keep their time')


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[1] not in ('save', 'restore'):
        sys.exit(__doc__)
    (save if sys.argv[1] == 'save' else restore)(sys.argv[2], sys.argv[3:])
