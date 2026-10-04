"""Cross-host tar packaging: recover Unix runtime modes from Electron's ZIP.

Only used by explicit Windows cross-packaging. Native Linux uses system tar.
No system installation, symlink permissions or setuid sandbox installation.
"""
import pathlib
import stat
import sys
import tarfile
import zipfile

folder = pathlib.Path(sys.argv[1]).resolve()
archive = pathlib.Path(sys.argv[3]).resolve()
if archive == folder or folder in archive.parents:
    raise ValueError('Archive must be outside the packaged folder')
with zipfile.ZipFile(sys.argv[2]) as runtime:
    modes = {}
    for entry in runtime.infolist():
        name = 'Qrazy' if entry.filename == 'electron' else entry.filename
        mode = entry.external_attr >> 16
        if stat.S_ISLNK(mode):
            raise ValueError('Electron runtime contains a symlink; use a native Linux build')
        modes[name.rstrip('/')] = (mode & 0o777) or (0o755 if entry.is_dir() else 0o644)

def permissions(info):
    relative = pathlib.PurePosixPath(info.name).relative_to(folder.name).as_posix()
    info.uid = info.gid = 0
    info.uname = info.gname = ''
    info.mode = 0o755 if info.isdir() else modes.get(relative, 0o644)
    if relative in ('Qrazy', 'chrome-sandbox', 'chrome_crashpad_handler', 'resources/qrazy-input-x11'):
        info.mode = 0o755
    return info

with tarfile.open(archive, 'w:gz', format=tarfile.PAX_FORMAT) as output:
    output.add(folder, arcname=folder.name, recursive=True, filter=permissions)
print('Archived Linux runtime with original Unix permissions; no setuid installation.')
