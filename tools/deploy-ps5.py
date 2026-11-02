"""Deploy PSteroid5 through one FTP connection with staging and rollback.

Default: read-only preflight. --publish authorizes backup, staging and replacement.
This deliberately supports only the known PSteroid5 title and USB location.
"""
from pathlib import Path, PurePosixPath
import argparse, datetime, ftplib, hashlib, importlib.util, io, json, struct, time, subprocess, tempfile

WORKSPACE=Path(__file__).resolve().parents[2]
SOURCE=WORKSPACE/'PSteroids5/dist/PPSA77305'
TARGET='/mnt/usb0/homebrew/PPSA77305'

def digest(data): return hashlib.sha256(data).hexdigest()

class Connection:
    def __init__(self,host,port): self.host=host;self.port=port;self.ftp=None
    def connect(self):
        if self.ftp:
            try:self.ftp.close()
            except OSError:pass
        self.ftp=ftplib.FTP()
        self.ftp.connect(self.host,self.port,timeout=30)
        self.ftp.login('anonymous','')
    def call(self,operation):
        for attempt in range(3):
            try:
                if self.ftp is None:self.connect()
                return operation(self.ftp)
            except (OSError,EOFError,ftplib.error_temp) as error:
                if attempt==2:raise
                print(f'FTP reconnect/retry {attempt+1}: {error}',flush=True)
                self.ftp.close();self.ftp=None;time.sleep(1)
    def listing(self,path):
        def request(ftp):
            lines=[];ftp.retrlines('LIST '+path,lines.append);result=[]
            for line in lines:
                parts=line.split(None,8)
                if len(parts)!=9:raise ValueError('Unexpected FTP listing: '+line)
                kind,name=parts[0][0],parts[8]
                if name in ('.','..'):continue
                if kind not in ('d','-'):raise ValueError('Refusing symlink/special file: '+line)
                if '/' in name or '\\' in name or ':' in name:raise ValueError('Unsafe filename: '+name)
                result.append((name,kind,int(parts[4])))
            return result
        return self.call(request)
    def tree(self,path):
        result=[]
        def walk(directory,prefix=''):
            for name,kind,size in self.listing(directory):
                rel=prefix+name
                if kind=='d':walk(directory+'/'+name,rel+'/')
                else:result.append((rel,size))
        walk(path);return result
    def read(self,path):
        def request(ftp):
            data=io.BytesIO();ftp.retrbinary('RETR '+path,data.write,blocksize=256*1024);return data.getvalue()
        return self.call(request)
    def put(self,source,target):
        def request(ftp):
            with source.open('rb') as data:ftp.storbinary('STOR '+target,data,blocksize=256*1024)
        self.call(request)
    def mkdir(self,path):
        current=''
        for part in PurePosixPath(path).parts[1:]:
            current+='/'+part
            def request(ftp,p=current):
                try:ftp.mkd(p)
                except ftplib.error_perm as error:
                    if not str(error).startswith('550'):raise
                    ftp.cwd(p);ftp.cwd('/') # Verify it exists and is a directory.
            self.call(request)
    def rename(self,source,dest):
        # Never blindly retry an uncertain rename. The caller journals/rolls back.
        self.ftp.rename(source,dest)

def validate_local():
    spec=importlib.util.spec_from_file_location('package_check',WORKSPACE/'PSPGameCPP/ps5/validate_package.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);module.validate(SOURCE,title_id='PPSA77305',game_assets=())
    report=json.loads((SOURCE/'validation.json').read_text())
    for name,expected in report['file_sha256'].items():
        if digest((SOURCE/name).read_bytes())!=expected:raise ValueError('Local package changed: '+name)
    return json.loads((SOURCE/'sce_sys/param.json').read_text())

def verify_bytes(name,expected,actual,elf):
    if actual==expected:return 'sha256'
    if name in {'eboot.bin','sce_module/libc.prx'} and actual[:4]==b'\x7fELF':
        if elf is None:raise ValueError('FTP returned ELF; local linked ELF is required for verification')
        if actual[:64]!=elf[:64]:raise ValueError('FTP ELF header mismatch')
        offset=struct.unpack_from('<Q',elf,32)[0]
        entry,count=struct.unpack_from('<HH',elf,54)
        if actual[offset:offset+entry*count]!=elf[offset:offset+entry*count]:raise ValueError('FTP ELF program-header mismatch')
        checked=0
        for i in range(count):
            kind,flags,at,va,pa,size,memory,alignment=struct.unpack_from('<IIQQQQQQ',elf,offset+i*entry)
            if kind==1:
                if actual[at:at+size]!=elf[at:at+size]:raise ValueError('FTP executable loadable segment mismatch')
                checked+=1
        if not checked:raise ValueError('No executable loadable segments found')
        return f'FTP reconstructed ELF: header and {checked} loadable segments match'
    raise ValueError('Remote content mismatch: '+name)

def extract_runtime():
    # The console FTP reconstructs both the app SELF and companion PRX as ELF.
    # Extract our locally verified plaintext PRX with the existing native tool.
    with tempfile.TemporaryDirectory(prefix='psteroids5-runtime-') as scratch:
        output=Path(scratch)/'libc.elf'
        def linux(path):
            return subprocess.check_output(['wsl.exe','-d','Ubuntu','--','wslpath','-a',str(path).replace('\\','/')],text=True).strip()
        subprocess.run(['wsl.exe','-d','Ubuntu','--',
                        '/home/futurepr0n/work/PS5_Vulkan/build/host/ps5-native-tool',
                        'self','--extract','--file',linux(SOURCE/'sce_module/libc.prx'),
                        '--out',linux(output)],check=True,capture_output=True)
        return output.read_bytes()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',default='192.168.1.212');parser.add_argument('--port',type=int,default=2121)
    parser.add_argument('--publish',action='store_true');args=parser.parse_args()
    param=validate_local()
    elf_path=Path(r'\\wsl.localhost\Ubuntu\home\futurepr0n\work\psteroids5-app\build\ps5\eboot.elf')
    if not elf_path.is_file():
        raise RuntimeError('Linked ELF required for FTP verification: '+str(elf_path))
    elf=elf_path.read_bytes()
    runtime_elf=extract_runtime()
    connection=Connection(args.host,args.port)
    if any(name.startswith('PPSA77305_') for name,kind,size in connection.listing('/mnt/sandbox')):
        raise RuntimeError('PSteroid5 has a sandbox; close the game before deployment')
    for parent in ['/data/homebrew','/mnt/usb1/homebrew']:
        try:
            entries=connection.listing(parent)
        except ftplib.error_perm as error:
            if str(error).startswith('550'):continue
            raise
        if any(name=='PPSA77305' for name,kind,size in entries):
            raise RuntimeError('Duplicate PSteroid5 source at '+parent+'; resolve it before deploying')
    previous=connection.tree(TARGET)
    print(f'Existing PSteroid5: {len(previous)} files, {sum(size for name,size in previous)} FTP-visible bytes',flush=True)
    old_param=connection.read(TARGET+'/sce_sys/param.json')
    if json.loads(old_param.decode('utf-8-sig'))['titleId']!='PPSA77305':raise ValueError('Wrong remote title')
    if not args.publish:
        print('Preflight only. Use --publish to stage, verify and replace.');return
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    root=WORKSPACE/'PSteroids5/deploy-logs'/stamp;root.mkdir(parents=True,exist_ok=False)
    previous_dir=root/'previous-ftp-readback';previous_dir.mkdir()
    transfer='/mnt/usb0/.psteroids5-transfer-'+stamp
    stage=transfer+'/incoming/PPSA77305';rollback=transfer+'/rollback/PPSA77305'
    pending='sce_sys/param.json.pending'
    state={'title_id':'PPSA77305','version':param['contentVersion'],'target':TARGET,'stage':stage,'rollback':rollback,
           'rollback_metadata':'sce_sys/param.json.rollback','local_backup':str(previous_dir),'console_tested':False,
           'old_moved':False,'old_metadata_withheld':False,'new_moved':False,'published':False,'verification':{},'runtime_canonical_elf_sha256':digest(runtime_elf)}
    def journal(): (root/'deployment.json').write_text(json.dumps(state,indent=2)+'\n',encoding='utf-8')
    journal()
    # Local reads may expose a reconstructed ELF. The remote rename backup retains
    # the underlying title exactly, including user files and shader caches.
    for index,(name,size) in enumerate(previous,1):
        data=connection.read(TARGET+'/'+name);destination=previous_dir/name
        destination.parent.mkdir(parents=True,exist_ok=True);destination.write_bytes(data)
        if index%10==0 or index==len(previous):print(f'Backed up {index}/{len(previous)} existing files',flush=True)
    expected={p.relative_to(SOURCE).as_posix():p for p in SOURCE.rglob('*') if p.is_file()}
    preserved=[]
    for name,size in previous:
        if name not in expected and name!='smoke-test.txt':expected[name]=previous_dir/name;preserved.append(name)
    state['preserved_user_or_cache_files']=preserved
    state['withheld_smoke_test']=any(name=='smoke-test.txt' for name,size in previous)
    journal();connection.mkdir(stage)
    for index,(name,source) in enumerate(sorted(expected.items()),1):
        remote=stage+'/'+(pending if name=='sce_sys/param.json' else name)
        connection.mkdir(str(PurePosixPath(remote).parent));connection.put(source,remote)
        if index%10==0 or index==len(expected):print(f'Staged {index}/{len(expected)} files',flush=True)
    for index,(name,source) in enumerate(sorted(expected.items()),1):
        remote=stage+'/'+(pending if name=='sce_sys/param.json' else name)
        state['verification'][name]=verify_bytes(name,source.read_bytes(),connection.read(remote),runtime_elf if name=='sce_module/libc.prx' else elf)
        if index%10==0 or index==len(expected):print(f'Verified {index}/{len(expected)} staged files',flush=True)
    state['status']='staged_and_verified';journal()
    if any(name.startswith('PPSA77305_') for name,kind,size in connection.listing('/mnt/sandbox')):
        raise RuntimeError('PSteroid5 started during transfer; verified stage retained, existing title unchanged')
    connection.mkdir(str(PurePosixPath(rollback).parent))
    try:
        connection.rename(TARGET,rollback);state['old_moved']=True;journal()
        connection.rename(rollback+'/sce_sys/param.json',rollback+'/sce_sys/param.json.rollback');state['old_metadata_withheld']=True;journal()
        connection.rename(stage,TARGET);state['new_moved']=True;journal()
        # Publish the metadata last, after all game data is in its final location.
        connection.rename(TARGET+'/'+pending,TARGET+'/sce_sys/param.json');state['published']=True;journal()
    except Exception:
        connection.connect()
        if state['new_moved']:connection.rename(TARGET,stage)
        if state['old_metadata_withheld']:connection.rename(rollback+'/sce_sys/param.json.rollback',rollback+'/sce_sys/param.json')
        if state['old_moved']:connection.rename(rollback,TARGET)
        state['status']='commit_failed_old_title_restored';journal();raise
    for name in ['sce_sys/param.json','sce_sys/icon0.png','eboot.bin']:
        verify_bytes(name,(SOURCE/name).read_bytes(),connection.read(TARGET+'/'+name),elf)
    state['status']='published_and_verified';state['completed_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();journal()
    (root/'RESTORE.txt').write_text('Close PSteroid5 before rollback. Remote original folder: '+rollback+
        '\nIts original sce_sys/param.json is retained as param.json.rollback to prevent duplicate registration.\n'+
        'Retain/move the new title aside, move the rollback folder to '+TARGET+', then rename its param.json.rollback to param.json.\n'+
        'Do not copy an FTP-reconstructed ELF over a packaged SELF; prefer the preserved remote original folder.\n',encoding='utf-8')
    print('Deployment complete: '+TARGET+' version '+param['contentVersion'],flush=True)
    print('Receipt and local backup: '+str(root),flush=True)
    print('Remote rollback retained: '+rollback,flush=True)
    connection.ftp.quit()

if __name__=='__main__':main()
