"""Package the existing Linux runtime as a type-2 AppImage on Linux or Windows.

Build tools/runtime are explicit local inputs, never downloaded at build time.
Unix permissions come from the original Electron ZIP when cross-packaging.
"""
import argparse
import hashlib
import os
import pathlib
import shutil
import stat
import subprocess
import tempfile
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--root', required=True)
parser.add_argument('--runtime', required=True)
parser.add_argument('--runtime-sha256', required=True)
parser.add_argument('--mksquashfs', required=True)
parser.add_argument('--electron-zip')
parser.add_argument('--out', required=True)
args = parser.parse_args()
root = pathlib.Path(args.root).resolve()
target = pathlib.Path(args.out).resolve()
runtime = pathlib.Path(args.runtime).read_bytes()
if hashlib.sha256(runtime).hexdigest() != args.runtime_sha256:
    raise ValueError('AppImage runtime does not match the pinned SHA-256')
if runtime[:6] != b'\x7fELF\x02\x01' or runtime[8:11] != b'AI\x02' or int.from_bytes(runtime[18:20], 'little') != 62:
    raise ValueError('Expected a type-2 Linux x86-64 AppImage runtime')
if target == root or root in target.parents:
    raise ValueError('AppImage output must be outside its source folder')
tool = str(pathlib.Path(args.mksquashfs).resolve())
modes = {}
if args.electron_zip:
    with zipfile.ZipFile(args.electron_zip) as archive:
        for entry in archive.infolist():
            mode = entry.external_attr >> 16
            if stat.S_ISLNK(mode):
                raise ValueError('Runtime links require a native Linux build')
            name = {'electron': 'Qrazy', 'LICENSE': 'LICENSE.electron'}.get(entry.filename, entry.filename)
            modes[name.rstrip('/')] = (mode & 0o777) or (0o755 if entry.is_dir() else 0o644)

target.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='.qrazy-appimage-', dir=target.parent) as temporary:
    work = pathlib.Path(temporary)
    appdir = work / 'Qrazy.AppDir'
    for file in root.rglob('*'):
        if file.is_symlink():
            raise ValueError('AppImage source must not contain symlinks')
    shutil.copytree(root, appdir)
    # Ozone must be selected before Electron starts, not after Wayland setup.
    (appdir / 'AppRun').write_text('#!/bin/sh\nset -eu\nplatform=x11\nif [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${WAYLAND_SOCKET:-}" ] || [ "${XDG_SESSION_TYPE:-}" = wayland ]; then\n    platform=wayland\nfi\nexec "${APPDIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}/Qrazy" "$@" --ozone-platform="$platform"\n', encoding='utf-8', newline='\n')
    (appdir / 'qrazy.desktop').write_text('[Desktop Entry]\nType=Application\nName=Qrazy\nComment=Qrazy desktop game client\nExec=Qrazy\nIcon=qrazy\nCategories=Game;\nTerminal=false\n', encoding='utf-8', newline='\n')
    shutil.copyfile(pathlib.Path(__file__).parent / 'assets/qrazy.png', appdir / 'qrazy.png')
    shutil.copyfile(appdir / 'qrazy.png', appdir / '.DirIcon')
    shutil.copytree(pathlib.Path(__file__).parent / 'appimage-licenses', appdir / 'appimage-licenses')
    overrides = []
    for file in sorted(appdir.rglob('*')):
        name = file.relative_to(appdir).as_posix()
        if any(c in name for c in '\n\r\\"') or ' ' in name:
            raise ValueError('Unsupported SquashFS permission path: ' + name)
        mode = modes.get(name, (file.stat().st_mode & 0o777) if not args.electron_zip else (0o755 if file.is_dir() else 0o644))
        if name in ('AppRun', 'Qrazy', 'chrome-sandbox', 'chrome_crashpad_handler', 'resources/qrazy-input-x11'):
            mode = 0o755
        overrides.append(f'{name} m {mode:o} 0 0')
    (work / 'permissions.txt').write_text('\n'.join(overrides) + '\n', encoding='utf-8', newline='\n')
    subprocess.run([tool, 'Qrazy.AppDir', 'payload.squashfs', '-noappend', '-comp', 'gzip', '-all-root', '-no-xattrs', '-no-progress', '-processors', '2', '-pf', 'permissions.txt'], cwd=work, check=True, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    payload = work / 'payload.squashfs'
    with payload.open('rb') as source:
        if source.read(4) != b'hsqs':
            raise ValueError('Invalid SquashFS payload')
    result = work / 'Qrazy.AppImage'
    with result.open('wb') as output, payload.open('rb') as source:
        output.write(runtime)
        shutil.copyfileobj(source, output)
    result.chmod(0o755)
    result.replace(target)
print(target)
