#!/usr/bin/env python3
"""Black-box regression tests against the actual release executables.
No copied model implementations, external network calls, root, or killall.
Usage: python3 tests/regression.py [--mips] [--output reports/tests.json]
"""
import argparse, hashlib, json, os, pathlib, shutil, signal, socket, random, errno
import subprocess, tempfile, time, urllib.parse, concurrent.futures
ROOT=pathlib.Path(__file__).resolve().parents[1]
RESULTS=[]
def require(ok, message):
    if not ok: raise AssertionError(message)
def record(variant, name, function):
    t=time.monotonic()
    try:
        detail=function()
        RESULTS.append(dict(variant=variant,test=name,result='PASS',seconds=round(time.monotonic()-t,3),detail=detail))
    except Exception as e:
        RESULTS.append(dict(variant=variant,test=name,result='FAIL',seconds=round(time.monotonic()-t,3),detail=f'{type(e).__name__}: {e}'))
    print(variant,name,RESULTS[-1]['result'],flush=True)
class Server:
    def __init__(self,command,root,extra=()):
        s=socket.socket();s.bind(('127.0.0.1',0));self.port=s.getsockname()[1];s.close()
        self.log=tempfile.TemporaryFile()
        self.p=subprocess.Popen(command+[str(root),'--port',str(self.port)]+list(extra),stdout=self.log,stderr=self.log,start_new_session=True)
        until=time.monotonic()+4
        while time.monotonic()<until:
            if self.p.poll() is not None:
                rc=self.p.returncode;self.close();raise RuntimeError('server exited: '+str(rc))
            try:
                s=socket.create_connection(('127.0.0.1',self.port),.1);s.close();return
            except OSError:time.sleep(.025)
        self.close();raise RuntimeError('startup timed out')
    def close(self):
        try:os.killpg(self.p.pid,signal.SIGKILL)
        except ProcessLookupError:pass
        self.p.wait(timeout=5);self.log.close()
    def connect(self):
        s=socket.create_connection(('127.0.0.1',self.port),timeout=5);s.settimeout(8);return s
    def raw(self,data,fragment=False,slow=False,halfclose=False):
        s=self.connect()
        if slow:s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,65536)
        try:
            if fragment:s.sendall(data[:2]);time.sleep(.06);s.sendall(data[2:])
            else:s.sendall(data)
            if halfclose:
                try:s.shutdown(socket.SHUT_WR)
                except OSError as e:
                    # A malformed request may already have been rejected/closed.
                    if e.errno not in (errno.ENOTCONN,errno.ECONNRESET):raise
            if slow:time.sleep(.15)
            out=b''
            while True:
                try:part=s.recv(65536)
                except ConnectionResetError:break
                if not part:break
                out+=part
            return out
        finally:s.close()
    def request(self,path,method='GET',extra=''):
        return self.raw(request_bytes(path,method,extra))
def request_bytes(path,method='GET',extra='',close=True):
    return f'{method} {path} HTTP/1.1\r\nHost: test\r\nConnection: {"close" if close else "keep-alive"}\r\n{extra}\r\n'.encode()
def response(raw,head=False):
    header,sep,body=raw.partition(b'\r\n\r\n');require(bool(sep),'incomplete HTTP header')
    lines=header.split(b'\r\n');code=int(lines[0].split()[1]);h={}
    for l in lines[1:]:
        k,v=l.split(b':',1);h[k.strip().lower()]=v.strip()
    length=int(h[b'content-length']);expected=0 if head else length
    require(len(body)==expected,f'body {len(body)} != advertised {expected}')
    return code,h,body
def run_variant(name,cmd,w,c):
    server=Server(cmd,w)
    def status(path,expected,method='GET',extra=''):
        code,h,body=response(server.request(path,method,extra),head=method=='HEAD')
        require(code in expected,f'status {code}, expected {expected}');return dict(status=code,body_bytes=len(body))
    try:
        record(name,'GET',lambda:status('/a.txt',{200}))
        record(name,'empty file',lambda:status('/empty',{200}))
        record(name,'FIFO cannot block server',lambda:status('/fifo',{403,404}))
        record(name,'HEAD directory policy',lambda:status('/listing/',{200}if c else{403},'HEAD'))
        def encoded_slash():
            raw=server.request('/%2fspace%20dir')
            if raw:
                code,h,_=response(raw)
                require(code in (301,400,403),'unexpected encoded slash status')
                require(not h.get(b'location',b'').startswith(b'//'),'scheme-relative redirect')
        record(name,'encoded slash cannot redirect off-origin',encoded_slash)
        def http10():
            for path in ('/a.txt','/space%20dir','/missing'):
                raw=server.raw(('GET '+path+' HTTP/1.0\r\n\r\n').encode())
                require(raw.startswith(b'HTTP/1.0 '),'response version mismatch')
                require(response(raw)[1][b'connection']==b'close','HTTP/1.0 default not close')
        record(name,'HTTP/1.0 version and default close',http10)
        def bad_headers():
            cases=[b'Host: x\r\nHost: y',b'Host: ',b'Host: x\r\nContent-Length : 5',b'Host: x\r\nTransfer-Encoding: chunked',b'Host: x\r\nContent-Length: 0\r\nContent-Length: 0',b'Host: x\r\n folded: x',b'Host: x\r\nX: a\nb']
            for headers in cases:
                raw=server.raw(b'GET /a.txt HTTP/1.1\r\nConnection: close\r\n'+headers+b'\r\n\r\n')
                if raw:require(response(raw)[0]==400,'invalid headers accepted: '+repr(headers))
        record(name,'strict header syntax and framing',bad_headers)
        def all_host_check():
            raw=server.raw(b'GET /a.txt HTTP/1.1\r\nConnection: close\r\n\r\n')
            if raw:require(response(raw)[0]==400,'missing Host accepted')
        record(name,'Host mandatory in every HTTP/1.1 profile',all_host_check)
        def long_target():
            raw=server.request('/'+'a'*1600)
            if raw:require(response(raw)[0] in (400,414,431),'overlong target accepted')
        record(name,'overlong request target bounded',long_target)
        def fuzz():
            rng=random.Random(20261023)
            for i in range(96):
                raw=bytearray(request_bytes('/a.txt'))
                for _ in range(1+i%4):raw[rng.randrange(len(raw)-4)]=rng.randrange(256)
                # Every trial has a terminator, so tests never rely on waiting for EOF.
                raw=server.raw(bytes(raw),halfclose=True)
                require(b'OUTSIDE_SECRET'not in raw,'root escape')
                require(server.p.poll() is None,'server process crashed')
            require(response(server.request('/a.txt'))[2]==b'hello\n','server unusable after fuzz')
            server.log.seek(0);log=server.log.read().lower()
            require(b'segmentation fault'not in log and b'uncaught target signal'not in log,'crash in worker')
            return {'seed':20261023,'mutations':96}
        record(name,'deterministic malformed-request smoke fuzz',fuzz)

        record(name,'HEAD',lambda:status('/a.txt',{200},'HEAD'))
        record(name,'HEAD missing has no body',lambda:status('/missing',{404},'HEAD'))
        record(name,'GET missing',lambda:status('/missing',{404}))
        record(name,'plain traversal blocked',lambda:status('/../outside/secret',{400,403}))
        record(name,'encoded traversal blocked',lambda:status('/%2e%2e/outside/secret',{400,403}))
        record(name,'file symlink blocked',lambda:status('/file-link',{403,404}))
        record(name,'directory symlink blocked',lambda:status('/dir-link/secret',{403,404}))
        record(name,'directory symlink index blocked',lambda:status('/dir-link/',{403,404}))
        record(name,'index.html',lambda:status('/subdir/',{200}))
        def redirect():
            code,h,_=response(server.request('/space%20dir'));require(code==301,'not 301')
            require(h[b'location']==b'/space%20dir/',repr(h));return {'status':code,'location':h[b'location'].decode()}
        record(name,'encoded redirect',redirect)
        def injection():
            raw=server.request('/'+urllib.parse.quote('redir\r\nX-Review: injected',safe=''))
            require(b'X-Review:' not in raw and b'x-review:' not in raw,'injected header')
            if raw:require(response(raw)[0] in (400,403),'controls accepted')
        record(name,'CRLF injection blocked',injection)
        def invalid_uri():
            for target in ['/a.txt%00x','/%0a','/%ZZ','/%']:
                raw=server.request(target)
                if raw:require(response(raw)[0] in (400,403),'accepted invalid URI '+target)
        record(name,'invalid encodings rejected',invalid_uri)
        def listing():
            code,h,body=response(server.request('/listing/'))
            if c:
                require(code==200,'listing disabled in C')
                require(b'<img src=x onerror=alert(1)>' not in body,'unescaped filename')
                require(b'&lt;img' in body and b'%3Cimg' in body,'missing text or URL escaping')
            else:require(code==403,'assembly directory listing must fail closed')
            return {'status':code}
        record(name,'safe directory listing policy',listing)
        record(name,'oversized directory bounded',lambda:status('/large-list/',{507}if c else{403}))
        def fragment():
            raw=server.raw(request_bytes('/a.txt'),fragment=True);require(response(raw)[2]==b'hello\n','fragmented request failed')
        record(name,'fragmented TCP request',fragment)
        def pipeline():
            raw=server.raw(request_bytes('/a.txt',close=False)+request_bytes('/a.txt'))
            for i in range(2):
                h,sep,tail=raw.partition(b'\r\n\r\n');require(bool(sep),'missing pipelined response')
                require(b' 200 ' in h.split(b'\r\n')[0],'bad pipeline status')
                length=next(int(l.split(b':',1)[1])for l in h.split(b'\r\n')if l.lower().startswith(b'content-length:'))
                require(tail[:length]==b'hello\n','pipeline body mismatch');raw=tail[length:]
            require(not raw,'extra pipeline data')
        record(name,'two pipelined responses',pipeline)
        def large():
            code,h,body=response(server.raw(request_bytes('/big.bin'),slow=True));require(code==200,'large status')
            require(hashlib.sha256(body).digest()==hashlib.sha256((w/'big.bin').read_bytes()).digest(),'large file content mismatch')
            return {'body_bytes':len(body)}
        record(name,'16 MiB limited-reader transfer',large)
        def large_header():
            raw=server.raw(request_bytes('/a.txt',extra='X-Large: '+'a'*8192+'\r\n'))
            if raw:require(response(raw)[0] in (400,414,431),'oversized header accepted')
        record(name,'oversized header bounded',large_header)
        def request_body():
            raw=server.raw(request_bytes('/a.txt',extra='Content-Length: 5\r\n')+b'hello'+request_bytes('/a.txt'))
            if raw:require(response(raw)[0]==400,'request body accepted')
        record(name,'request body framing rejected',request_body)
        def partial_timeout():
            s=server.connect();s.sendall(b'GE');s.settimeout(19)
            try:require(s.recv(1)==b'','partial request not closed')
            finally:s.close()
        record(name,'partial request timeout',partial_timeout)
        def idle():
            s=server.connect();s.settimeout(9)
            try:require(s.recv(1)==b'','idle connection not closed')
            finally:s.close()
        # C has an absolute request deadline of 15 s, assembly idle deadline 6 s.
        if not c:record(name,'idle accepted connection timeout',idle)
        def parallel():
            def one(_):return response(server.request('/a.txt'))[2]==b'hello\n'
            with concurrent.futures.ThreadPoolExecutor(max_workers=8)as pool:require(all(pool.map(one,range(20))),'parallel request failed')
        record(name,'20 parallel requests',parallel)
        if c:
            def ranges():
                cases=[('bytes=0-2',206,b'hel'),('bytes=3-',206,b'lo\n'),('bytes=-2',206,b'o\n'),('bytes=99-',416,None),('bytes=999999999999999999999999-',416,None)]
                for v,expected,body in cases:
                    code,h,out=response(server.request('/a.txt',extra='Range: '+v+'\r\n'));require(code==expected,f'{v}: {code}')
                    require(b'content-range'in h,'missing Content-Range')
                    if body is not None:require(out==body,'bad range body')
            record(name,'bounded Range forms and 416',ranges)
            def split_pipeline():
                raw=server.raw(request_bytes('/subdir/',close=False)+request_bytes('/listing/',close=False)+request_bytes('/a.txt'))
                require(raw.count(b'HTTP/1.1 200 OK')==3,'mixed memory/file pipeline')
            record(name,'file/listing/file pipeline',split_pipeline)
            def host_check():
                raw=server.raw(b'GET /a.txt HTTP/1.1\r\nConnection: close\r\n\r\n');require(response(raw)[0]==400,'missing Host accepted')
            record(name,'HTTP/1.1 Host required',host_check)
    finally:server.close()
    # Binding must actually be honored or the option rejected with an error.
    def bind_test():
        if not c and name!='gas-x86_64':
            p=subprocess.run(cmd+[str(w),'--addr','127.0.0.1'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=3)
            require(p.returncode!=0,'unsupported --addr silently accepted');return {'policy':'rejected'}
        srv=Server(cmd,w,('--addr','127.0.0.1'))
        try:
            require(response(srv.request('/a.txt'))[0]==200,'IPv4 bind failed')
            s=socket.socket(socket.AF_INET6);s.settimeout(.2)
            try:s.connect(('::1',srv.port));allowed=True
            except OSError:allowed=False
            finally:s.close()
            require(not allowed,'IPv6 allowed for IPv4-only bind')
        finally:srv.close()
    record(name,'--addr policy',bind_test)
    for args in [('--port','0'),('--port','65536'),('--port','8080junk'),('--port',),('--unknown',),('--port','9999999999999999999999999999'),('--addr','999.1.1.1'),('--addr','127.0.0.1junk'),('--workers','17'),('--workers','1x')]:
        def invalid(args=args):
            p=subprocess.run(cmd+list(args),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=3)
            require(p.returncode!=0,'invalid CLI accepted')
        record(name,'invalid CLI '+' '.join(args),invalid)
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--mips',action='store_true');ap.add_argument('--variant',action='append');ap.add_argument('--output',default='reports/tests.json');args=ap.parse_args()
    variants=[('nolibc-x86_64',[str(ROOT/'whitehttpd_c/build/httpd')],True),('fasm-x86_64',[str(ROOT/'whitehttpd_fasm/whitehttpd')],False),('gas-x86_64',[str(ROOT/'whitehttpd_gas/whitehttpd')],False)]
    if args.mips:
        qbe=shutil.which('qemu-mips-static')or shutil.which('qemu-mips');qle=shutil.which('qemu-mipsel-static')or shutil.which('qemu-mipsel')
        if not qbe or not qle:ap.error('MIPS emulators required; refusing to silently skip')
        variants += [('nolibc-mips32be',[qbe,str(ROOT/'whitehttpd_c/build/httpd.mipsbe')],True),('nolibc-mips32el',[qle,str(ROOT/'whitehttpd_c/build/httpd.mipsel')],True),('asm-mips32be',[qbe,str(ROOT/'final-4092/whitehttpd-mips32be')],False)]
    absent=[v for v in variants if not pathlib.Path(v[1][-1]).exists()]
    for v in absent:print('SKIP variant %s: binary not present (%s)'%(v[0],v[1][-1]))
    variants=[v for v in variants if pathlib.Path(v[1][-1]).exists()]
    if not variants:ap.error('no runnable variants')
    if args.variant:
        unknown=set(args.variant)-{v[0]for v in variants}
        if unknown:ap.error('unknown or unavailable variant: '+','.join(unknown))
        variants=[v for v in variants if v[0]in args.variant]
    binaries={name:{'path':str(pathlib.Path(cmd[-1]).relative_to(ROOT)), 'sha256':hashlib.sha256(pathlib.Path(cmd[-1]).read_bytes()).hexdigest(),'bytes':pathlib.Path(cmd[-1]).stat().st_size}for name,cmd,c in variants}
    with tempfile.TemporaryDirectory(prefix='whitehttpd-tests-')as temp:
        base=pathlib.Path(temp);w=base/'www';w.mkdir();outside=base/'outside';outside.mkdir()
        (w/'empty').touch();os.mkfifo(w/'fifo');(w/'a.txt').write_bytes(b'hello\n');(w/'index.html').write_bytes(b'INDEX\n');(w/'big.bin').write_bytes(b'B'*(16*1024*1024))
        (outside/'secret').write_bytes(b'OUTSIDE_SECRET');(outside/'index.html').write_bytes(b'OUTSIDE_INDEX')
        (w/'file-link').symlink_to(outside/'secret');(w/'dir-link').symlink_to(outside,target_is_directory=True)
        for d in ['listing','large-list','subdir','space dir','redir\r\nX-Review: injected']:(w/d).mkdir()
        (w/'subdir'/'index.html').write_bytes(b'SUBDIR\n');(w/'listing'/'<img src=x onerror=alert(1)>').touch()
        for i in range(450):(w/'large-list'/('%04d_'%i+'a'*170)).touch()
        for name,cmd,c in variants:
            try:run_variant(name,cmd,w,c)
            except Exception as e:RESULTS.append(dict(variant=name,test='startup/harness',result='FAIL',detail=str(e)))
    output=ROOT/args.output;output.parent.mkdir(exist_ok=True,parents=True)
    for name,cmd,c in variants:
        if hashlib.sha256(pathlib.Path(cmd[-1]).read_bytes()).hexdigest()!=binaries[name]['sha256']:
            RESULTS.append(dict(variant=name,test='binary unchanged during run',result='FAIL',detail='executable changed'))
    report={'scope':'Black-box tests of exact built binaries; QEMU user-mode, not router hardware','test_suite_sha256':hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),'binaries':binaries,'results':RESULTS,'passed':sum(x['result']=='PASS'for x in RESULTS),'failed':sum(x['result']=='FAIL'for x in RESULTS)}
    output.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n');print('SUMMARY',report['passed'],'PASS',report['failed'],'FAIL')
    return bool(report['failed'])
if __name__=='__main__':raise SystemExit(main())
