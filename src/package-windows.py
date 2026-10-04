import argparse
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import os

parser = argparse.ArgumentParser()
parser.add_argument('--root', required=True)
parser.add_argument('--out', required=True)
args = parser.parse_args()
root = Path(args.root).resolve()
target = Path(args.out).resolve()
temporary = target.with_suffix('.zip.tmp')
with ZipFile(temporary, 'w', compression=ZIP_DEFLATED, compresslevel=9) as archive:
    for file in sorted(root.rglob('*')):
        if file.is_symlink():
            raise RuntimeError('Package must not contain symlinks')
        if file.is_file():
            archive.write(file, file.relative_to(root.parent).as_posix())
with ZipFile(temporary) as archive:
    if archive.testzip() is not None or f'{root.name}/Qrazy.exe' not in archive.namelist():
        raise RuntimeError('Windows ZIP verification failed')
os.replace(temporary, target)
print(f'Windows ZIP created and verified: {target.stat().st_size} bytes')
