"""Package a completed signed SDL/CEF distribution; no signing or publication."""
import argparse, base64, hashlib, json, os, pathlib, shutil, subprocess, tempfile

def sha(file):
    with file.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def main():
    parser = argparse.ArgumentParser()
    for name in ('root', 'runtime', 'runtime-sha256', 'mksquashfs', 'out'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()
    target = pathlib.Path(args.out).resolve()
    runtime = pathlib.Path(args.runtime).read_bytes()
    if hashlib.sha256(runtime).hexdigest() != args.runtime_sha256:
        raise ValueError('AppImage runtime hash differs')
    if runtime[:6] != b'\x7fELF\x02\x01' or runtime[8:11] != b'AI\x02' or int.from_bytes(runtime[18:20], 'little') != 62:
        raise ValueError('Expected type-2 Linux x86-64 AppImage runtime')
    if target.exists() or root == target or root in target.parents:
        raise ValueError('Refuse overwriting a package or writing inside its payload')
    current = root / 'updater/current.json'
    # Run only the explicitly supplied completed production verifier.
    subprocess.run([str(root/'updater/node'), '--no-addons', str(root/'updater/production-runtime.cjs'), 'verify', str(root)], check=True)
    payload = base64.b64decode(json.loads(current.read_bytes())['payload'])
    manifest = json.loads(payload)
    if manifest['platform'] != 'linux-x64':
        raise ValueError('Expected Linux signed runtime')
    if target.name != 'Qrazy-SDLCEF-linux-x64-distribution-' + manifest['version'] + '.AppImage':
        raise ValueError('Unexpected AppImage asset name')
    inventory = []
    for file in sorted(root.rglob('*')):
        if file.is_symlink():
            raise ValueError('Links forbidden in AppImage source')
        if file.is_file():
            name = file.relative_to(root).as_posix()
            if not (name in ('launch.py', 'launch.sh', 'SYSTEM-REQUIREMENTS.txt') or name.startswith('runtime/') or name in ('updater/node','updater/NODE-LICENSE.txt','updater/current.json','updater/production-runtime.cjs','updater/production-release.cjs','updater/release-public.pem')):
                raise ValueError('Non-application file in package: ' + name)
            if any(part.startswith('.') for part in file.relative_to(root).parts):
                raise ValueError('Private/transaction file in package')
            inventory.append(dict(path=name, size=file.stat().st_size, sha256=sha(file), mode=493 if file.stat().st_mode & 0o111 else 420))
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.qrazy-appimage-', dir=target.parent) as temporary:
        work = pathlib.Path(temporary)
        appdir = work/'Qrazy.AppDir'
        shutil.copytree(root, appdir/'payload')
        bootstrap = pathlib.Path(__file__).with_name('appimage_bootstrap.py').read_text().replace('INVENTORY = []', 'INVENTORY = '+repr(inventory))
        (appdir/'bootstrap.py').write_text(bootstrap)
        (appdir/'AppRun').write_text('#!/bin/sh\nset -eu\nexec /usr/bin/python3 "${APPDIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}/bootstrap.py" "$@"\n')
        (appdir/'AppRun').chmod(0o755)
        (appdir/'qrazy.desktop').write_text('[Desktop Entry]\nType=Application\nName=Qrazy\nExec=AppRun\nIcon=qrazy\nCategories=Game;\nTerminal=false\n')
        icon=pathlib.Path(__file__).parents[2]/'assets/qrazy.png'
        shutil.copyfile(icon, appdir/'qrazy.png')
        shutil.copyfile(icon, appdir/'.DirIcon')
        shutil.copytree(pathlib.Path(__file__).parents[2]/'appimage-licenses', appdir/'appimage-licenses')
        subprocess.run([args.mksquashfs, str(appdir), str(work/'payload.squashfs'), '-noappend', '-comp', 'gzip', '-all-root', '-no-xattrs', '-no-progress', '-processors', '2'], check=True)
        with (work/'payload.squashfs').open('rb') as stream:
            if stream.read(4) != b'hsqs':
                raise ValueError('Invalid SquashFS')
        with target.open('xb') as output, (work/'payload.squashfs').open('rb') as stream:
            output.write(runtime)
            shutil.copyfileobj(stream, output)
        target.chmod(0o755)
    descriptor=dict(schema='qrazy-sdlcef-linux-distribution-v1', product='qrazy-sdlcef', platform='linux-x64', channel='stable', version=manifest['version'], sequence=manifest['sequence'], runtimeIdentity=hashlib.sha256(payload).hexdigest(), files=inventory, asset=dict(name=target.name,size=target.stat().st_size,sha256=sha(target)))
    target.with_suffix('.manifest.json').write_text(json.dumps(descriptor,separators=(',',':')))
    print('AppImage prepared. Sign its distribution manifest with the existing key before release planning.')

if __name__ == '__main__':
    main()
