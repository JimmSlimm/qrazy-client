"""Pinned updater selection, exclusion and orderly single-runtime installation.

The packaging tool replaces CONTROL_PINS with hashes of the frozen public helpers.
No profile migration, alternate signing identity, automatic opening or rollback.
"""
import fcntl, hashlib, json, os, pathlib, stat, subprocess, sys

CONTROL_PINS = {}  # Filled only in the packaged launcher.
ROOT = pathlib.Path(__file__).absolute().parent

def regular(path):
    for p in (path,*path.parents):
        if p.is_symlink():raise ValueError('Linked installation path refused')
    info=path.stat()
    if not stat.S_ISREG(info.st_mode):raise ValueError('Nonregular installation file')
    return info

def lock(path):
    regular(path) if path.exists() else None
    fd=os.open(path,os.O_RDWR|os.O_CREAT|os.O_NOFOLLOW|os.O_CLOEXEC,0o600)
    fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
    return fd

def controls():
    if not CONTROL_PINS:raise ValueError('Unprepared launcher')
    for name,expected in CONTROL_PINS.items():
        file=ROOT/name;regular(file)
        with file.open('rb') as stream:
            if hashlib.file_digest(stream,'sha256').hexdigest()!=expected:raise ValueError('Updater helper checksum mismatch')

def helper(operation):
    environment=dict(os.environ)
    for key in ('NODE_OPTIONS','NODE_PATH','LD_PRELOAD','LD_LIBRARY_PATH'):environment.pop(key,None)
    result=subprocess.run([str(ROOT/'updater/node'),'--no-addons',str(ROOT/'updater/production-runtime.cjs'),operation,str(ROOT)],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,timeout=900,env=environment)
    if result.returncode or len(result.stdout)>3*1024**2:raise ValueError('Signed runtime verification or recovery failed')
    reply=json.loads(result.stdout)
    if not reply.get('ok'):raise ValueError('Signed updater refused the operation')
    return reply['data']

def run_logged(command, profile, **kwargs):
    """Bounded private diagnostics; subprocess crashes are never a clean exit."""
    log=profile/'launch.log';previous=profile/'launch.previous.log'
    if log.exists():
        regular(log)
        if previous.exists():regular(previous)
        if previous.is_symlink():raise ValueError('Linked diagnostic log refused')
        os.replace(log,previous)
    fd=os.open(log,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600)
    fatal=False;tail=b'';size=0
    with os.fdopen(fd,'wb',buffering=0) as stream:
        with subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,**kwargs) as process:
            while True:
                block=process.stdout.read1(65536)
                if not block:break
                scan=tail+block
                if any(marker in scan for marker in (b'stack smashing detected',b':FATAL:',b'QRAZY renderer terminated')):fatal=True
                tail=scan[-128:]
                if size+len(block)>8*1024**2:
                    stream.seek(0);stream.truncate();size=0
                stream.write(block);size+=len(block)
            code=process.wait()
    if fatal:
        raise ValueError('A browser subprocess failed. Keep this folder and report '+str(log))
    return code

def main():
    if sys.argv[1:]:raise ValueError('Launch arguments refused')
    os.umask(0o077)
    controls()
    profile=ROOT/'profile-sdlcef-linux'
    if profile.is_symlink():raise ValueError('Linked profile refused')
    profile.mkdir(mode=0o700,exist_ok=True)
    install_lock=lock(ROOT/'updater/install.lock')
    host_lock=lock(profile/'host.lock')
    # Recovery/verification may touch runtimes only with all live users excluded.
    worker_lock=lock(profile/'desktop-worker.lock')
    selected=helper('verify')
    os.close(worker_lock)
    version=selected['manifest']['version']
    if os.environ.get('XDG_SESSION_TYPE')!='wayland' or not os.environ.get('WAYLAND_DISPLAY'):
        raise ValueError('A native Wayland desktop session is required; Xorg is not supported')
    environment=dict(os.environ,QRAZY_ROOT=str(ROOT),QRAZY_LOCK_FD=str(host_lock),QRAZY_INSTALLED_VERSION=version,SDL_VIDEO_DRIVER='wayland',LD_LIBRARY_PATH=str(ROOT/'runtime'))
    for key in ('LD_PRELOAD','NODE_OPTIONS','NODE_PATH','QRAZY_PROTOTYPE_ROOT','QRAZY_PROTOTYPE_LOCK_FD'):environment.pop(key,None)
    # Parent retains locks through child CEF shutdown and installation. No relaunch.
    code=run_logged([str(ROOT/'runtime/qrazy-sdl-cef')],profile,cwd=ROOT/'runtime',env=environment,pass_fds=(host_lock,))
    if code==42:
        worker_lock=lock(profile/'desktop-worker.lock')
        helper('install')
        helper('verify')
        os.close(worker_lock)
        print('Qrazy update installed and verified. Open Qrazy manually.')
    elif code:
        raise ValueError('Qrazy exited with code '+str(code)+'. Keep this folder and report '+str(profile/'launch.log'))
    os.close(host_lock);os.close(install_lock)

if __name__=='__main__':
    try:main()
    except Exception as error:
        print('Qrazy: '+(str(error) if isinstance(error,ValueError) else 'Installation/profile unavailable or in use. Close the existing client; preserve this folder.'),file=sys.stderr)
        raise SystemExit(1)
