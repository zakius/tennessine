#!/usr/bin/env python3
"""tennessine's commands. Stdlib only, Python 3.10.

apply [--stop]       prepares the Chromium tree in the current directory:
                     1. dubna/src/ (Firefox Sync, no saved history: shared with desktop),
                        then src/, copied over the tree.
                     2. Vanadium's patches, straight from the vanadium submodule, renamed to
                        tennessine, minus patches/vanadium-drops.txt.
                     3. patches/series: paths from the repo root, so dubna/ patches come
                        from that submodule. A series entry from a GPL-3.0 source fails:
                        Vanadium's patches are GPL-2.0-only (see LICENSING below).
                     4. dubna's Polish translations of its strings.
                     --stop stops at the first failure, for refresh.
check [WORKDIR] [--stop]
                     apply on a Chromium checkout at Vanadium's version, zero fuzz. The
                     checkout (default work/chromium/src) is shallow, one tag per version,
                     and reused; fix a failed patch in place there and refresh it.
refresh PATCH [FILE...]
                     in the tree: applies a patch that no longer applies cleanly with fuzz,
                     then rewrites it from the result, keeping its header comment. The tree
                     must hold everything before the patch and nothing after (check --stop).
                     Rejected hunks are left as *.rej. Fuzz can land a hunk in the wrong
                     place: read every hunk patch reports with fuzz, fix the tree, then take
                     the fix with FILE: rewrites FILE's part of PATCH from the tree's git
                     diff (only when no other patch edits FILE).
upstream             lists newer upstream tags (vanadium, titanium), a newer uBlock Origin
                     release than the installer's pin, and dubna commits; exits 1 when
                     there is one.
build [x64] [--release]
                     arm64 APK in a rootless Ubuntu container on a scratch copy in
                     work/build (tools/container-build.sh), signed with the key in
                     ~/.local/share/tennessine-signing. The tree is prepared again only when
                     what preparing reads changed; a change to src/ alone is copied in.
                     Output: work/build/tennessine/chromium/src/out/release/*.apk.
                     x64: the x86_64 APK for the emulator, whose ARM translation crashes
                     arm64 Chromium, from the same tree (out/x64).
                     --release: the APK compressed with Zopfli, for publishing (about
                     40 seconds more).
publish [APK]        puts the APK (default: the local build's arm64 one) in the F-Droid
                     repository, https://packages.zakius.xyz/fdroid/repo.

LICENSING: Vanadium's patches are GPL-2.0-only and allow no GPL-3.0 code in the same
program, so nothing GPL-3.0 is built in: not Helium's or Cromite's patches. dubna is used
under its GPL-2.0 option. uBlock Origin (GPL-3.0, Helium's build) is a separate program
that the browser downloads and installs on first run (src/chrome/browser/ublock_installer).
"""
import fnmatch, hashlib, json, os, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VERSION_RE = r'\d+(?:\.\d+){3}'


def run(*args, **kw):
    return subprocess.run(args, check=True, **kw)


def vanadium_version():
    return re.search(VERSION_RE, (ROOT / 'vanadium/args.gn').read_text())[0]


# Series sources whose code is GPL-3.0, which Vanadium's GPL-2.0-only license excludes.
GPL3_SOURCES = ('helium/', 'patches/cromite/')
# Vanadium's files whose text users read: grit's messages and their translations, and Android's
# string resources (the launcher label is one). In them the name is lowercase, as everywhere
# else; code keeps Tennessine, for identifiers.
STRING_FILES = re.compile(r'diff --git a/\S+(\.grdp?|\.xtb|/values[^/]*/[^/\s]+\.xml) ')
# The uBlock Origin build the installer downloads: its pin is in this file.
UBLOCK_INSTALLER = ROOT / 'src/chrome/browser/ublock_installer/ublock_installer.cc'


def copy_if_changed(src, dst):
    """Copies a file whose content differs, with the time of the copy: a source's own older
    time would leave outputs that another checkout's build made from it looking up to date."""
    if not (os.path.isfile(dst) and Path(src).read_bytes() == Path(dst).read_bytes()):
        shutil.copyfile(src, dst)
    shutil.copymode(src, dst)
    return dst


def apply(stop=False):
    tree = Path.cwd()
    for top in (ROOT / 'dubna/src', ROOT / 'src'):
        shutil.copytree(top, tree, dirs_exist_ok=True, copy_function=copy_if_changed)

    drops = [line.split('\t')[0] for line in (ROOT / 'patches/vanadium-drops.txt').read_text()
             .splitlines() if line and not line.startswith('#')]
    vanadium = sorted((ROOT / 'vanadium/patches').glob('*.patch'))
    for glob in drops:
        if not any(fnmatch.fnmatch(p.name, glob) for p in vanadium):
            print(f'WARN  no Vanadium patch matches drop {glob}')
    rename = (('VANADIUM', 'TENNESSINE'), ('Vanadium', 'Tennessine'), ('vanadium', 'tennessine'))
    patches = []
    for p in vanadium:
        if not any(fnmatch.fnmatch(p.name, glob) for glob in drops):
            text = ''.join(s.replace('Vanadium', 'tennessine') if STRING_FILES.match(s) else s
                           for s in re.split(r'(?m)^(?=diff --git )', p.read_text()))
            for old, new in rename:
                text = text.replace(old, new)
            patches.append((f'vanadium/{p.name}', text))
    for line in (ROOT / 'patches/series').read_text().splitlines():
        if line and not line.startswith('#'):
            if line.startswith(GPL3_SOURCES):
                sys.exit(f'tennessine: {line} is GPL-3.0 code, which Vanadium\'s GPL-2.0-only '
                         'license excludes (LICENSING in tennessine.py)')
            patches.append((line, (ROOT / line).read_text()))

    failed = False
    for name, text in patches:
        result = subprocess.run(['git', 'apply', '--whitespace=nowarn'], input=text, text=True,
                                capture_output=True)
        if result.returncode == 0:
            print(f'ok    {name}')
            continue
        print(f'FAIL  {name}')
        print(''.join(f'      {line}\n' for line in (result.stdout + result.stderr).splitlines()),
              end='')
        failed = True
        if stop:
            return 1
    # dubna's translations of its strings, after the patches add the grdp.
    sys.stdout.flush()
    if subprocess.run([sys.executable, str(ROOT / 'dubna/tools/translate.py'), str(tree),
                       str(ROOT / 'dubna')]).returncode:
        failed = True
    return 1 if failed else 0


def check(workdir, stop):
    version = vanadium_version()
    src = Path(workdir or ROOT / 'work/chromium') / 'src'
    src.mkdir(parents=True, exist_ok=True)
    git = lambda *args, **kw: subprocess.run(['git', '-C', str(src), *args], **kw)
    if not (src / '.git').is_dir():
        git('init', '-q', check=True)
        git('remote', 'add', 'origin', 'https://chromium.googlesource.com/chromium/src.git',
            check=True)
    if git('rev-parse', '-q', '--verify', f'refs/tags/{version}', capture_output=True).returncode:
        git('fetch', '-q', '--depth=1', 'origin', f'refs/tags/{version}:refs/tags/{version}',
            check=True)
    git('checkout', '-q', '-f', f'refs/tags/{version}', check=True)
    git('clean', '-qfd', check=True)
    os.chdir(src)
    code = apply(stop)
    milestone = (ROOT / 'dubna/milestone.txt').read_text().strip()
    if version.split('.')[0] != milestone:
        print(f'WARN  Chromium {version} is not on milestone {milestone} of dubna/milestone.txt; '
              'only an exploit in the wild allows that')
    return code


def section_span(text, name):
    """(start, end) of name's section in a patch."""
    f = re.escape(name)
    start = re.compile(rf'^(diff --git a/{f} b/{f}\n(index .*\n)?)?--- (a/{f}|/dev/null)\n',
                       re.M).search(text)
    if not start:
        sys.exit(f'tennessine: the patch has no section for {name}')
    end = re.compile(r'^(diff --git |--- (a/|/dev/null))', re.M).search(text, start.end() + 1)
    return start.start(), end.start() if end else len(text)


def refresh(patch, files):
    patch = Path(patch).resolve()
    if files:
        for name in files:
            diff = run('git', 'diff', '--src-prefix=a/', '--dst-prefix=b/', '--', name,
                       capture_output=True, text=True).stdout
            part = diff[diff.index('\n--- ') + 1:] if '\n--- ' in diff else ''
            text = patch.read_text()
            start, end = section_span(text, name)
            patch.write_text(text[:start] + part + text[end:])
            print(f'refreshed {name} in {patch}')
        return 0
    text = patch.read_text()
    names = re.findall(r'^\+\+\+ b/(\S+)', text, re.M)
    with tempfile.TemporaryDirectory() as before:
        for name in names:
            if Path(name).is_file():
                (Path(before) / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(name, Path(before) / name)
        subprocess.run(['patch', '-p1', '-F3', '--no-backup-if-mismatch'], input=text, text=True)
        header = re.split(r'(?m)^(?:---|diff) ', text, maxsplit=1)[0]
        parts = []
        for name in names:
            old = Path(before) / name
            old, label = (old, f'a/{name}') if old.is_file() else ('/dev/null', '/dev/null')
            result = subprocess.run(['diff', '-u', '--label', label, '--label', f'b/{name}',
                                     str(old), name], capture_output=True, text=True)
            # 2: trouble, e.g. no such file outside the tree; writing would empty the patch.
            if result.returncode > 1:
                sys.exit(f'tennessine: {result.stderr.strip()}')
            parts.append(result.stdout)
    patch.write_text(header + ''.join(parts))
    print(f'refreshed {patch}')
    return 0


def upstream():
    def remote(*args):
        return run('git', 'ls-remote', *args, capture_output=True, text=True).stdout
    url = lambda m: run('git', '-C', str(ROOT), 'config', '-f', '.gitmodules', f'submodule.{m}.url',
                        capture_output=True, text=True).stdout.strip()
    key = lambda tag: [int(n) if n.isdigit() else n for n in re.split(r'(\d+)', tag)]
    new = 0
    for m in ('vanadium', 'titanium'):
        tags = re.findall(r'refs/tags/(\S+)', remote('--tags', '--refs', url(m)))
        want = max(tags, key=key)
        have = subprocess.run(['git', '-C', str(ROOT / m), 'describe', '--tags', '--exact-match'],
                              capture_output=True, text=True).stdout.strip() or '?'
        if have != want:
            print(f'{m}: {have} -> {want}')
            new = 1
    pin = re.search(r'kVersion\[\] = "([^"]+)"', UBLOCK_INSTALLER.read_text())[1]
    tags = re.findall(r'refs/tags/(\S+)', remote('--tags', '--refs',
                                                  'https://github.com/imputnet/uBlock.git'))
    latest = max(tags, key=key)
    if key(latest) > key(pin):
        print(f'uBlock Origin: {pin} -> {latest} (pin in {UBLOCK_INSTALLER.relative_to(ROOT)})')
        new = 1
    have = run('git', '-C', str(ROOT / 'dubna'), 'rev-parse', 'HEAD', capture_output=True,
               text=True).stdout.strip()
    want = remote(url('dubna'), 'refs/heads/main').split()[0]
    if have != want:
        print(f'dubna: {have[:7]} -> {want[:7]}')
        new = 1
    return new


IMAGE = 'localhost/tennessine-build'


def build_image(build_dir):
    """The build container's image (tools/Containerfile), with Chromium's build dependencies as
    the tree in `build_dir` lists them. podman's layer cache makes this nearly free when nothing
    changed."""
    context = ROOT / 'work/build-image'
    shutil.rmtree(context, ignore_errors=True)
    context.mkdir(parents=True)
    shutil.copy(ROOT / 'tools/Containerfile', context)
    # COPY needs a file to match when the tree isn't there yet.
    (context / 'install-build-deps.none').write_text('')
    for name in ('install-build-deps.sh', 'install-build-deps.py'):
        script = build_dir / 'tennessine/chromium/src/build' / name
        if script.is_file():
            shutil.copy(script, context)
    run('podman', 'build', '-q', '-t', IMAGE, str(context))


# What preparing the tree reads besides src/ and dubna/src/: the Chromium version, the patches,
# their translations, the gclient config and hooks, and the scripts that run them. Taking in too
# much costs only a needless prepare.
PREP_INPUTS = ('vanadium', 'patches', 'dubna/patches', 'dubna/l10n', 'dubna/tools/translate.py',
               'res', 'post-sync.sh', 'common.sh', '.gclient', 'tennessine.py', 'tools')
SOURCES = ('dubna/src', 'src')


def files_under(top):
    path = ROOT / top
    files = [path] if path.is_file() else path.rglob('*')
    # A submodule's .git file names its checkout's git folder, which differs by worktree.
    return sorted(f for f in files if f.is_file() and not {'__pycache__', '.git'} & set(f.parts))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patched_paths():
    """The tree paths that Vanadium's patches and ours change."""
    series = [ROOT / line for line in (ROOT / 'patches/series').read_text().splitlines()
              if line and not line.startswith('#')]
    return {m for p in [*(ROOT / 'vanadium/patches').glob('*.patch'), *series]
            for m in re.findall(r'^\+\+\+ b/(\S+)', p.read_text(), re.M)}


def build_inputs():
    """(prep, src): the hash of what preparing the tree reads, and {tree path: (source, hash)}
    of our new files, dubna's then ours, as apply copies them. A new file that a patch also
    changes counts as a prep input: copying it alone would drop the patch's change."""
    prep = hashlib.sha256()
    for top in PREP_INPUTS:
        for path in files_under(top):
            prep.update(str(path.relative_to(ROOT)).encode() + b'\0' + path.read_bytes())
    src = {}
    for top in SOURCES:
        for path in files_under(top):
            src[str(path.relative_to(ROOT / top))] = (str(path.relative_to(ROOT)), digest(path))
    for name in sorted(patched_paths() & set(src)):
        prep.update(name.encode() + src[name][1].encode())
    return prep.hexdigest(), src


def version_code():
    """Android's versionCode: 10^9 plus the minutes from 2026-01-01 UTC to the commit's time, so
    each commit's build updates the last one, and stays above Chromium's own codes (805903974
    for 155.0.8059.39 arm64). From the commit, not the clock: a release then has the code of the
    development build of its commit, and builds nothing again for it. Android's ceiling,
    2100000000, is about 2000 years away."""
    time = int(subprocess.run(['git', '-C', str(ROOT), 'log', '-1', '--format=%ct'],
                              capture_output=True, text=True, check=True).stdout)
    return 1000000000 + (time - 1767225600) // 60


def build(mode, release):
    # One tree for both APKs, moved forward to each new Chromium: out/Default for arm64,
    # out/x64 for the emulator's x86_64.
    build_dir = ROOT / 'work/build'
    (build_dir / 'tennessine').mkdir(parents=True, exist_ok=True)
    build_image(build_dir)
    if not all((KEYS / f).is_file() for f in ('test.jks', 'local.properties')):
        sys.exit(f'tennessine: no APK signing key (test.jks, local.properties) in {KEYS}')
    # No .git: in a git worktree it is a file naming a gitdir outside the container, and with
    # it every git command in the copy fails. H, not exclude, so --delete drops an old one.
    run('rsync', '-a', '--delete', '--filter=H .git', '--exclude=/work', '--exclude=/dubna/rust/oganesson_sync/target',
        '--exclude=/chromium', '--exclude=/keys', '--exclude=/depot_tools', f'{ROOT}/',
        f'{build_dir}/tennessine/')
    # The tree needs preparing again only when a prep input changed or a new file went away. The
    # stamp is in the tree, so any other prepare's git clean removes it; container-build.sh
    # writes it once a prepare is done. A changed or added new file is copied in here.
    prep, src = build_inputs()
    tree = build_dir / 'tennessine/chromium/src'
    stamp = tree / '.tennessine-prepared.json'
    last = json.loads(stamp.read_text()) if stamp.is_file() else {}
    prepare = last.get('prep') != prep or not set(last.get('src', [])) <= set(src)
    record = json.dumps({'prep': prep, 'src': sorted(src)})
    if prepare:
        (build_dir / 'prepared.json.new').write_text(record)
    else:
        for name, (path, _) in src.items():
            (tree / name).parent.mkdir(parents=True, exist_ok=True)
            copy_if_changed(ROOT / path, tree / name)
        stamp.write_text(record)
    # --name: one build at a time in the shared tree.
    os.execvp('podman', ['podman', 'run', '--rm', '-i', '--pids-limit=-1',
                         '--name', 'tennessine-build',
                         '-v', f'{build_dir}:/build', '-v', f'{KEYS}:/keys',
                         '-w', '/build/tennessine', '-e', f'MODE={mode}',
                         '-e', f'PREPARE={"1" if prepare else ""}',
                         '-e', f'RELEASE={"1" if release else ""}',
                         '-e', f'VERSION_CODE={version_code()}',
                         # A job per CPU of the machine: the system limits the containers'
                         # CPUs by the time of day, and a build that starts under the
                         # daytime limit then still fills every CPU at night.
                         '-e', f'JOBS={os.environ.get("JOBS", os.cpu_count())}',
                         # The container's first process ignores SIGTERM unless it traps it:
                         # this one exits on it, which ends the whole build, so stopping podman
                         # stops the build too.
                         IMAGE, 'sh', '-c',
                         'trap "exit 143" TERM; bash tools/container-build.sh & wait $!'])


# The APK signing key (test.jks, local.properties): every update must be signed with it, or
# Android won't install it over the last one. It exists only there: back the folder up.
KEYS = Path.home() / '.local/share/tennessine-signing'
FDROID = Path.home() / '.local/share/tennessine-fdroid'
FDROID_IMAGE = 'registry.gitlab.com/fdroid/docker-executable-fdroidserver:master'


def env(name):
    """NAME from the environment, else from tennessine's .env, which Git ignores: where things
    are deployed stays out of the repository."""
    if name in os.environ:
        return os.environ[name]
    path = ROOT / '.env'
    if not path.exists():
        # A git worktree has no .env of its own: the main checkout's is the one.
        common = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--path-format=absolute',
                                 '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        if common:
            path = Path(common).parent / '.env'
    for line in path.read_text().splitlines() if path.exists() else []:
        key, _, value = line.partition('=')
        if key.strip() == name:
            return value.strip()
    sys.exit(f'tennessine: set {name} in the environment or in {path}, e.g. {name}=myserver:fdroid')



def publish(apk):
    """The repository lives in ~/.local/share/tennessine-fdroid: config.yml, the index signing
    key (keystore.p12, which exists only there: back the folder up), metadata/ and repo/.
    fdroid update, from F-Droid's fdroidserver image, renames the APK to
    <package>_<versionCode>.apk, keeps the newest 3 in repo/ and moves older ones to archive/
    (emptied of its APKs first, so it holds only the ones just moved),
    and signs the index. repo/ and archive/ then go to FDROID_TARGET/www (HOST:DIR, from .env), served by nginx
    (tools/fdroid-compose.yaml) behind the host's Traefik. An APK the repository already has is
    not added again. The repository's own icon is dubna's, as icon.png in that folder:
    fdroid update empties repo/icons and copies it in."""
    out = ROOT / 'work/build/tennessine/chromium/src/out/release'
    apks = [Path(apk)] if apk else sorted(out.glob('*-arm64-v8a.apk'))
    if len(apks) != 1 or not apks[0].is_file():
        sys.exit(f'tennessine: expected one APK, found {[str(a) for a in apks]}')
    if not (FDROID / 'keystore.p12').is_file():
        sys.exit(f'tennessine: no signing key in {FDROID}')
    digest = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
    have = {digest(f) for d in ('repo', 'archive') for f in (FDROID / d).glob('*.apk')}
    if digest(apks[0]) not in have:
        # The versionCode comes from the commit, so a rebuild of a published commit has its
        # code: it takes the old APK's place, as fdroid wants one APK per code.
        if not apk:
            for same in (FDROID / 'repo').glob(f'*_{version_code()}.apk'):
                same.unlink()
        shutil.copy2(apks[0], FDROID / 'repo' / apks[0].name)
    # The archive holds only what this update moves out of repo/: older APKs go.
    for old in (FDROID / 'archive').glob('*.apk'):
        old.unlink()
    run('docker', 'run', '--rm', '-u', f'{os.getuid()}:{os.getgid()}', '-e', 'HOME=/tmp',
        # The image checks its own fdroidserver checkout, owned by another user.
        '-e', 'GIT_CONFIG_COUNT=1', '-e', 'GIT_CONFIG_KEY_0=safe.directory',
        '-e', 'GIT_CONFIG_VALUE_0=*', '-v', f'{FDROID}:/repo', '-w', '/repo', FDROID_IMAGE,
        'update', '--rename-apks')
    target = env('FDROID_TARGET')
    host, path = target.split(':', 1)
    run('ssh', host, f'mkdir -p {path}/www')
    run('rsync', '-a', '--delete', f'{FDROID}/repo', f'{FDROID}/archive', f'{target}/www/')
    run('rsync', '-a', str(ROOT / 'tools/fdroid-compose.yaml'), f'{target}/compose.yaml')
    run('ssh', host, f'cd {path} && docker compose up -d --quiet-pull')
    return 0


def main():
    args = sys.argv[1:]
    command = args.pop(0) if args else ''
    stop = '--stop' in args
    args = [a for a in args if a != '--stop']
    if command == 'apply' and not args:
        return apply(stop)
    if command == 'check' and len(args) <= 1:
        return check(args[0] if args else None, stop)
    if command == 'refresh' and args:
        return refresh(args[0], args[1:])
    if command == 'upstream' and not args:
        return upstream()
    release = '--release' in args
    args = [a for a in args if a != '--release']
    if command == 'build' and args in ([], ['x64']):
        return build(args[0] if args else '', release)
    if command == 'publish' and len(args) <= 1:
        return publish(args[0] if args else None)
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main())
