"""Stage public runtime inputs for external signing; never install or launch.

Use ignored staging storage. Signed metadata and full distribution installation
remain explicit steps. This script does not invent a version or release identity.
"""
import argparse,pathlib,shutil
p=argparse.ArgumentParser()
for name in ('cef-root','sdl-root','build','controls','out'):p.add_argument('--'+name,required=True,type=pathlib.Path)
a=p.parse_args();source=pathlib.Path(__file__).resolve().parent
if a.out.exists():raise SystemExit('Output exists; preserve and review it before preparing again.')
runtime=a.out/'runtime';runtime.mkdir(parents=True)
shutil.copyfile(a.build/'Qrazy.exe',a.out/'Qrazy.exe')
for name in ('chrome_elf.dll','d3dcompiler_47.dll','dxcompiler.dll','dxil.dll','libcef.dll','v8_context_snapshot.bin','vk_swiftshader.dll','vulkan-1.dll','vk_swiftshader_icd.json'):
    shutil.copyfile(a.cef_root/'Release'/name,runtime/name)
shutil.copyfile(a.cef_root/'Release/bootstrap.exe',runtime/'Qrazy.exe')
shutil.copyfile(a.build/'Qrazy.dll',runtime/'Qrazy.dll')
shutil.copyfile(a.sdl_root/'lib/x64/SDL3.dll',runtime/'SDL3.dll')
for item in (a.cef_root/'Resources').iterdir():
    if item.is_dir():shutil.copytree(item,runtime/item.name)
    else:shutil.copyfile(item,runtime/item.name)
shutil.copyfile(source/'bridge-production.js',runtime/'bridge.js')
shutil.copyfile(source.parent/'update-notice.cjs',runtime/'update-notice.cjs')
for name in ('SYSTEM-REQUIREMENTS.txt','CHANGES.txt'):
    shutil.copyfile(source/name,runtime/name)
licenses=runtime/'licenses';licenses.mkdir()
for item,name in ((a.cef_root/'LICENSE.txt','cef-license.txt'),(a.cef_root/'CREDITS.html','cef-credits.html'),(a.sdl_root/'LICENSE.txt','sdl3-license.txt'),(source.parents[2]/'LICENSE','project-license.txt')):
    shutil.copyfile(item,licenses/name)
controls=a.out/'updater';controls.mkdir()
for name in ('node.exe','node-license.txt','production-runtime.cjs','production-release.cjs','release-public.pem','distribution.cjs','transition-install.cjs','transition-peer.cjs','legacy-release.cjs','legacy-config.cjs'):
    shutil.copyfile(a.controls/name,controls/name)
print('Public runtime prepared for inventory/signing; no installation or client launch.')
