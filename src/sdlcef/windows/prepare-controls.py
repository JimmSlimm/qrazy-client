"""Prepare public controls/pins in ignored build storage; no signing or launch."""
import argparse, hashlib, pathlib, shutil
p=argparse.ArgumentParser()
p.add_argument('--node-root',required=True,type=pathlib.Path)
p.add_argument('--out',required=True,type=pathlib.Path)
a=p.parse_args();source=pathlib.Path(__file__).resolve().parent.parent
a.out.mkdir(parents=True,exist_ok=True)
scripts=('production-runtime.cjs','production-release.cjs','release-public.pem','distribution.cjs','transition-install.cjs','transition-peer.cjs','legacy-release.cjs')
for name in scripts:
    shutil.copyfile(source/name,a.out/name)
shutil.copyfile(source.parent/'update-config.cjs',a.out/'legacy-config.cjs')
shutil.copyfile(a.node_root/'node.exe',a.out/'node.exe')
shutil.copyfile(a.node_root/'LICENSE',a.out/'node-license.txt')
pins=[]
for name in ('node.exe',*scripts,'legacy-config.cjs'):
    with (a.out/name).open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
    pins.append('  {L"'+name+'","'+digest+'"},')
(a.out/'updater_pins.h').write_text('#pragma once\nstruct ControlPin {const wchar_t* name;const char* sha256;};\ninline constexpr ControlPin ControlPins[]={\n'+'\n'.join(pins)+'\n};\n')
