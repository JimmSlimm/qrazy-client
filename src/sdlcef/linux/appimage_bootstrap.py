"""AppImage entry point: one writable installation, signed runtime updates."""
import fcntl, hashlib, os, pathlib, shutil, stat, sys, tempfile

INVENTORY = []  # Filled by package-appimage.py; no profile contents.

def no_links(path):
    for item in (path, *path.parents):
        if item.is_symlink():
            raise ValueError('Linked installation path refused')

def verify(root, files):
    for entry in files:
        file = root / entry['path']
        no_links(file)
        info = file.stat()
        if not stat.S_ISREG(info.st_mode) or info.st_size != entry['size']:
            raise ValueError('AppImage installation inventory differs')
        with file.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != entry['sha256']:
                raise ValueError('AppImage installation checksum differs')

def main():
    if sys.argv[1:] or not INVENTORY:
        raise ValueError('Unprepared AppImage or unsupported arguments')
    os.umask(0o077)
    source = pathlib.Path(__file__).absolute().parent / 'payload'
    data = pathlib.Path(os.environ.get('XDG_DATA_HOME', str(pathlib.Path.home() / '.local/share')))
    if not data.is_absolute():
        raise ValueError('XDG_DATA_HOME must be absolute')
    no_links(data)
    data.mkdir(parents=True, exist_ok=True)
    root = data / 'qrazy-sdlcef'
    no_links(root)
    fd = os.open(data / '.qrazy-sdlcef-bootstrap.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if not root.exists():
            verify(source, INVENTORY)
            if shutil.disk_usage(data).free < sum(f['size'] for f in INVENTORY) + 1024**3:
                raise ValueError('Keep at least 1 GiB free after installation')
            temporary = pathlib.Path(tempfile.mkdtemp(prefix='.qrazy-sdlcef-install-', dir=data))
            try:
                for entry in INVENTORY:
                    target = temporary / entry['path']
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(source / entry['path'], target)
                    target.chmod(entry['mode'])
                verify(temporary, INVENTORY)
                os.rename(temporary, root)
            finally:
                if temporary.exists():
                    shutil.rmtree(temporary)
        # Never overwrite a newer signed runtime or any existing profile.
        # Control changes require a separately reviewed upgrade mechanism.
        verify(root, [f for f in INVENTORY if not f['path'].startswith('runtime/') and f['path'] != 'updater/current.json'])
    finally:
        os.close(fd)
    os.execv('/usr/bin/python3', ['/usr/bin/python3', str(root / 'launch.py')])

if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print('Qrazy: ' + str(error) + '. Keep the installation and profile intact.', file=sys.stderr)
        raise SystemExit(1)
