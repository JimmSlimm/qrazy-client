"""Isolated desktop worker. Fixed JSON operations only; never exposes paths or a shell.

QAS1 layout and limits match the Electron AssetStore. Stdlib Python is a declared
Linux prerequisite. Only the unsandboxed browser host can start this worker.
"""
import base64, hashlib, json, os, pathlib, re, stat, struct, sys, uuid
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))

HEADER, CHUNK = 4096, 1048576
LIMITS = {'map': 512*CHUNK, 'sound': 64*CHUNK, 'shader': 512*CHUNK, 'texture': 512*CHUNK}
TOTAL, RESERVE = 16*1024**3, 1024**3

def integer(n): return type(n) is int
def digest(s): return isinstance(s, str) and bool(re.fullmatch('[a-f0-9]{64}', s))
def encoded(obj): return json.dumps(obj, ensure_ascii=False, separators=(',', ':')).encode()
def safe_directory(path):
    path = pathlib.Path(path).absolute()
    for p in reversed([path, *path.parents]):
        if p.is_symlink(): raise ValueError('Profile directories cannot be symbolic links')
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir(): raise ValueError('Invalid profile directory')
    return path
def open_file(path, flags, mode=0o600):
    fd = os.open(path, flags | getattr(os, 'O_NOFOLLOW', 0), mode)
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        os.close(fd); raise ValueError('Expected a regular file')
    return os.fdopen(fd, 'r+b' if flags & os.O_RDWR else 'rb', buffering=0)
def write_all(f, data):
    view = memoryview(data)
    while view:
        n = f.write(view)
        if not n: raise OSError('Asset write made no progress')
        view = view[n:]
def read_all(f, size):
    data = bytearray()
    while len(data) < size:
        chunk = f.read(size-len(data))
        if not chunk: raise ValueError('Asset is truncated')
        data.extend(chunk)
    return bytes(data)
def sync_directory(directory):
    fd = os.open(directory, os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0))
    try: os.fsync(fd)
    finally: os.close(fd)

class Assets:
    def __init__(self, profile, max_bytes=TOTAL, reserve=RESERVE):
        self.directory = safe_directory(pathlib.Path(profile)/'desktop-assets')
        self.transfers, self.max_bytes, self.reserve = {}, max_bytes, reserve
    def filename(self, kind, key):
        if kind not in LIMITS or not isinstance(key, str) or not 1 <= len(key.encode()) <= 2048 or re.search('[\x00-\x1f\x7f]', key):
            raise ValueError('Invalid asset kind or key')
        return self.directory/(hashlib.sha256((kind+'\0'+key).encode()).hexdigest()+'.asset')
    def room(self):
        if len(self.transfers) >= 4: raise ValueError('At most four asset transfers may be open')
    def storage_room(self, size):
        # Count all regular files, including orphan temporaries, plus unwritten reservations.
        used = 0
        for p in self.directory.iterdir():
            info = p.lstat()
            if not stat.S_ISREG(info.st_mode): raise ValueError('Unsafe asset directory entry')
            used += info.st_size
        reserved = sum(t['size']-t['offset'] for t in self.transfers.values() if t['mode']=='write')
        required = HEADER+size
        if used+reserved+required > self.max_bytes: raise ValueError('Desktop asset storage limit reached (16 GiB). Existing downloads are preserved.')
        disk = os.statvfs(self.directory)
        if disk.f_bavail*disk.f_frsize-reserved-required < self.reserve:
            raise ValueError('Not enough free disk space to save desktop assets (1 GiB reserve).')
    def dispose(self, token):
        t = self.transfers.pop(token)
        try: t['file'].close()
        finally:
            if 'temporary' in t: t['temporary'].unlink(missing_ok=True)
    def reset(self):
        for token in list(self.transfers): self.dispose(token)
    def transfer(self, token, mode):
        if not isinstance(token, str) or token not in self.transfers or self.transfers[token]['mode'] != mode:
            raise ValueError('Unknown or expired transfer')
        return self.transfers[token]
    def metadata(self, kind, key):
        filename = self.filename(kind, key)
        try: f = open_file(filename, os.O_RDONLY)
        except FileNotFoundError: return None
        try:
            header = read_all(f, HEADER); length = struct.unpack('<I', header[4:8])[0]
            if header[:4] != b'QAS1' or not 1 <= length <= HEADER-8: raise ValueError('Invalid asset header')
            data = json.loads(header[8:8+length])
            if data['kind'] != kind or data['key'] != key or not integer(data['size']) or not 1 <= data['size'] <= LIMITS[kind] or not digest(data['sha256']) or os.fstat(f.fileno()).st_size != HEADER+data['size']:
                raise ValueError('Invalid or incomplete asset')
            return dict(data, file=f)
        except Exception: f.close(); raise
    def has(self, kind, key):
        t = self.metadata(kind,key)
        if t: t['file'].close()
        return bool(t)
    def openRead(self, kind, key):
        self.room(); t = self.metadata(kind,key)
        if not t: return None
        token = str(uuid.uuid4()); self.transfers[token] = dict(t, mode='read', offset=0, hash=hashlib.sha256())
        return dict(token=token,size=t['size'],sha256=t['sha256'])
    def readChunk(self, token):
        t = self.transfer(token,'read')
        try:
            data = read_all(t['file'], min(CHUNK,t['size']-t['offset']))
            t['offset'] += len(data); t['hash'].update(data); done = t['offset']==t['size']
            if done:
                if t['hash'].hexdigest() != t['sha256']: raise ValueError('Asset checksum mismatch; download it again')
                self.dispose(token)
            return dict(base64=base64.b64encode(data).decode(), done=done)
        except Exception:
            if token in self.transfers: self.dispose(token)
            raise
    def closeRead(self, token): self.transfer(token,'read'); self.dispose(token)
    def beginWrite(self, descriptor):
        kind,key,size = descriptor['kind'],descriptor['key'],descriptor['size']
        destination = self.filename(kind,key); sha = descriptor.get('sha256')
        if not integer(size) or not 1 <= size <= LIMITS[kind] or sha is not None and not digest(sha): raise ValueError('Invalid asset size or checksum')
        if len(encoded(dict(kind=kind,key=key,size=size,sha256='0'*64))) > HEADER-8: raise ValueError('Asset metadata too large')
        self.room(); self.storage_room(size)
        token = str(uuid.uuid4()); temporary = self.directory/(token+'.partial')
        f = open_file(temporary,os.O_RDWR|os.O_CREAT|os.O_EXCL)
        self.transfers[token] = dict(mode='write',file=f,temporary=temporary,destination=destination,kind=kind,key=key,size=size,sha256=sha,offset=0,hash=hashlib.sha256())
        try: write_all(f,b'\0'*HEADER)
        except Exception: self.dispose(token); raise
        return dict(token=token,maxChunkBytes=CHUNK)
    def writeChunk(self, token, text):
        t = self.transfer(token,'write')
        try:
            if not isinstance(text,str) or len(text)>1400000: raise ValueError('Invalid asset chunk')
            data=base64.b64decode(text,validate=True)
            if not 1 <= len(data) <= CHUNK or t['offset']+len(data)>t['size']: raise ValueError('Invalid asset chunk size')
            write_all(t['file'],data); t['offset']+=len(data); t['hash'].update(data)
        except Exception: self.dispose(token); raise
    def finishWrite(self, token):
        t = self.transfer(token,'write')
        try:
            sha=t['hash'].hexdigest()
            if t['offset']!=t['size'] or t['sha256'] is not None and sha!=t['sha256']: raise ValueError('Incomplete asset or checksum mismatch')
            metadata=dict(kind=t['kind'],key=t['key'],size=t['size'],sha256=sha)
            data=encoded(metadata); header=b'QAS1'+struct.pack('<I',len(data))+data
            t['file'].seek(0); write_all(t['file'],header.ljust(HEADER,b'\0')); os.fsync(t['file'].fileno()); t['file'].close()
            if t['destination'].is_symlink(): raise ValueError('Unsafe asset destination')
            os.replace(t['temporary'],t['destination']); sync_directory(self.directory)
            self.transfers.pop(token); return metadata
        except Exception:
            if token in self.transfers: self.dispose(token)
            raise
    def abortWrite(self,token): self.transfer(token,'write'); self.dispose(token)

def serve(profile):
    assets=Assets(profile)
    # Exclude a second worker, without touching or deleting any stored data.
    import fcntl
    lock=open_file(safe_directory(profile)/'desktop-worker.lock',os.O_RDWR|os.O_CREAT)
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    operations={'has':2,'openRead':2,'readChunk':1,'closeRead':1,'beginWrite':1,'writeChunk':2,'finishWrite':1,'abortWrite':1}
    try:
        for line in sys.stdin.buffer:
            try:
                if len(line)>1500000: raise ValueError('Request too large')
                request=json.loads(line); op,args=request['op'],request['args']
                if op=='reset': assets.reset(); value=None
                elif op=='changelog':
                    with open_file(pathlib.Path(__file__).resolve().parent/'CHANGES.txt',os.O_RDONLY) as f: value=f.read(16000).decode()
                elif op.startswith('update-'):
                    if args or op not in ('update-state','update-check','update-install'): raise ValueError('Invalid production update action')
                    import subprocess
                    root=pathlib.Path(profile).parent
                    operation={'update-state':'state','update-check':'update','update-install':'prepare'}[op]
                    env=dict(os.environ);env.pop('NODE_OPTIONS',None);env.pop('NODE_PATH',None);env.pop('LD_PRELOAD',None);env.pop('LD_LIBRARY_PATH',None)
                    # Keep exclusion through Node's lifetime, even if this worker
                    # is interrupted during a download. A new launch/install must
                    # wait for that operation rather than racing its staged files.
                    result=subprocess.run([str(root/'updater/node'),'--no-addons',str(root/'updater/production-runtime.cjs'),operation,str(root)],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=880,env=env,pass_fds=(lock.fileno(),),check=False)
                    if result.returncode or len(result.stdout)>1500000: raise ValueError('Signed update failed; preserve the folder and reopen manually')
                    reply=json.loads(result.stdout)
                    if not reply.get('ok'): raise ValueError('Signed update verification failed')
                    value=reply['data']
                    if op=='update-install':value=dict(phase='close-required',message='Closing to install. Open Qrazy manually afterward.')
                elif op in ('config-read','config-write'):
                    # Paths come only from SDL's native picker, never renderer args.
                    path=pathlib.Path(args[0])
                    if not path.is_absolute() or path.suffix.lower()!='.cfg' or path.is_symlink(): raise ValueError('Expected a regular cfg file')
                    if op=='config-read':
                        with open_file(path,os.O_RDONLY) as f:
                            if os.fstat(f.fileno()).st_size>CHUNK: raise ValueError('Config exceeds 1 MiB')
                            data=f.read(CHUNK+1)
                            if len(data)>CHUNK: raise ValueError('Config exceeds 1 MiB')
                        text=data.decode('utf-8-sig')
                        if '\0' in text: raise ValueError('Invalid config text')
                        value=dict(cancelled=False,name=path.name,text=text)
                    else:
                        data=args[1].encode()
                        if len(data)>CHUNK or b'\0' in data: raise ValueError('Invalid config text')
                        temp=path.with_name('.'+path.name+'.'+str(uuid.uuid4())+'.partial')
                        try:
                            with open_file(temp,os.O_RDWR|os.O_CREAT|os.O_EXCL) as f: write_all(f,data);os.fsync(f.fileno())
                            os.replace(temp,path);sync_directory(path.parent)
                        finally: temp.unlink(missing_ok=True)
                        value=dict(cancelled=False)
                elif op in operations and isinstance(args,list) and len(args)==operations[op]: value=getattr(assets,op)(*args)
                else: raise ValueError('Unknown desktop operation')
                result=dict(ok=True,data=value)
            except Exception as e:
                # Fixed, bounded error messages; never echo input, paths or credentials.
                result=dict(ok=False,error=str(e)[:160] if isinstance(e,ValueError) else 'Desktop storage unavailable; normal loading can continue')
            sys.stdout.buffer.write(encoded(result)+b'\n'); sys.stdout.buffer.flush()
    finally: assets.reset(); lock.close()

if __name__=='__main__':
    if len(sys.argv)!=2: raise SystemExit('Expected the isolated profile directory')
    os.umask(0o077)
    try: serve(sys.argv[1])
    except Exception: raise SystemExit('Isolated desktop worker could not acquire its profile or storage')
