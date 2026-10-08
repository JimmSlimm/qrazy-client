import pathlib,shutil,hashlib,json,urllib.request,tarfile,zipfile,os,re,subprocess
import argparse
parser=argparse.ArgumentParser(description='Prepare an unsigned Linux candidate using retained dependencies; no system installation or publication')
parser.add_argument('--workspace',required=True);parser.add_argument('--dependencies',required=True)
parser.add_argument('--version',required=True);parser.add_argument('--sequence',required=True,type=int)
args=parser.parse_args()
assert re.fullmatch(r'(0|[1-9][0-9]{0,5})[.](0|[1-9][0-9]{0,5})[.](0|[1-9][0-9]{0,5})',args.version) and args.sequence>0
root=pathlib.Path(args.workspace).resolve();old=pathlib.Path(args.dependencies).resolve()
linux=pathlib.Path(__file__).resolve().parent;source=linux.parent;out=root/'out';out.mkdir(exist_ok=True)
target=out/('Qrazy-SDLCEF-linux-x64-'+args.version);target.mkdir() # Refuse rebuilding a used/prepared candidate.
runtime=target/'runtime';runtime.mkdir();control=target/'updater';control.mkdir()
cef=pathlib.Path((old/'cef-path.txt').read_text().strip())
for p in (cef/'Release').iterdir():
 if p.is_file() and p.name!='chrome-sandbox':shutil.copyfile(p,runtime/p.name)
for p in (cef/'Resources').iterdir():
 if p.is_dir():shutil.copytree(p,runtime/p.name)
 else:shutil.copyfile(p,runtime/p.name)
shutil.copyfile(old/'deps/baseline/sdl/lib/libSDL3.so.0.4.18',runtime/'libSDL3.so.0')
for name in ('bridge.js','desktop_backend.py','SYSTEM-REQUIREMENTS.txt','CHANGES.txt'):shutil.copyfile(linux/name,runtime/name)
shutil.copyfile(root/'build/qrazy-sdl-cef',runtime/'qrazy-sdl-cef')
licenses=runtime/'licenses';licenses.mkdir()
for src,name in [(cef/'LICENSE.txt','CEF-LICENSE.txt'),(cef/'CREDITS.html','CEF-CREDITS.html'),(old/'deps/baseline/sdl/share/licenses/SDL3/LICENSE.txt','SDL3-LICENSE.txt'),(old/'PROJECT-LICENSE.txt','PROJECT-LICENSE.txt')]:shutil.copyfile(src,licenses/name)
for p in runtime.rglob('*'):
 if p.is_file():p.chmod(0o755 if p.name=='qrazy-sdl-cef' or '.so' in p.name else 0o644)
lock=json.loads((linux/'node-lock.json').read_text());archive=root/lock['name']
if not archive.exists():
 with urllib.request.urlopen(lock['url'],timeout=45) as response,archive.open('xb') as dest:shutil.copyfileobj(response,dest)
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert sha(archive)==lock['sha256']
with tarfile.open(archive,'r:xz') as tar:
 for suffix,dest in [('bin/node',control/'node'),('LICENSE',control/'NODE-LICENSE.txt')]:
  member=tar.getmember('node-v22.23.3-linux-x64/'+suffix)
  assert member.isfile()
  with tar.extractfile(member) as f,dest.open('xb') as stream:shutil.copyfileobj(f,stream)
(control/'node').chmod(0o755);(control/'NODE-LICENSE.txt').chmod(0o644)
for name in ('production-runtime.cjs','production-release.cjs','release-public.pem'):shutil.copyfile(source/name,control/name);(control/name).chmod(0o644)
subprocess.run([str(control/'node'),'--version'],check=True)
inventory=[]
for p in sorted(runtime.rglob('*')):
 if p.is_file():inventory.append(dict(path=p.relative_to(runtime).as_posix(),size=p.stat().st_size,sha256=sha(p),mode=493 if p.stat().st_mode&0o111 else 420))
asset=out/('Qrazy-SDLCEF-linux-x64-'+args.version+'.zip')
with zipfile.ZipFile(asset,'x',zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=False) as z:
 for f in inventory:
  p=runtime/f['path'];info=zipfile.ZipInfo(f['path'],(2026,10,8,0,0,0));info.external_attr=(0o100000|f['mode'])<<16;info.compress_type=zipfile.ZIP_DEFLATED
  with p.open('rb') as stream,z.open(info,'w') as dest:shutil.copyfileobj(stream,dest)
manifest=dict(schema='qrazy-sdlcef-release-v1',product='qrazy-sdlcef',platform='linux-x64',channel='stable',version=args.version,sequence=args.sequence,notes=(linux/'CHANGES.txt').read_text(),files=inventory,asset=dict(name=asset.name,size=asset.stat().st_size,sha256=sha(asset)))
(out/'runtime-manifest.json').write_text(json.dumps(manifest,separators=(',',':')))
pins={p.relative_to(target).as_posix():sha(p) for p in control.iterdir()}
launch=(linux/'launch.py').read_text().replace('CONTROL_PINS = {}', 'CONTROL_PINS = '+repr(pins))
(target/'launch.py').write_text(launch);(target/'launch.py').chmod(0o644)
shutil.copyfile(linux/'launch.sh',target/'launch.sh');(target/'launch.sh').chmod(0o755)
shutil.copyfile(linux/'SYSTEM-REQUIREMENTS.txt',target/'SYSTEM-REQUIREMENTS.txt');(target/'SYSTEM-REQUIREMENTS.txt').chmod(0o644)
provenance={'baselineLockSHA256':sha(old/'deps/baseline/lock.json'),'cef':json.loads((old/'cef-distribution.json').read_text()),'node':lock,'nativeSHA256':sha(runtime/'qrazy-sdl-cef'),'sourceFiles':{p.relative_to(source).as_posix():sha(p) for p in source.rglob('*') if p.is_file() and '__pycache__' not in p.parts},'scope':'Retained VM/dependencies; no independent clean build or GUI/clean-system/hardware acceptance'}
(out/'BUILD-PROVENANCE.json').write_text(json.dumps(provenance,indent=2)+'\n')
print(json.dumps(manifest['asset']))
