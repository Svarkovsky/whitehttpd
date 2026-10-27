#!/usr/bin/env python3
"""Unit tests of build utilities, not substitutes for real HTTP regressions."""
import unittest,tempfile,pathlib,sys,struct,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from check_elf import inspect
from sstrip import sstrip

def fixture(wide=True,big=False,flags=5,stack=6):
    e='>'if big else'<';size=64 if wide else 52;phsize=56 if wide else 32
    data=bytearray(320);data[:7]=b'\x7fELF'+bytes([2 if wide else 1,2 if big else 1,1])
    struct.pack_into(e+'HHI',data,16,2,62 if wide else 8,1)
    struct.pack_into(e+('Q'if wide else'I'),data,24,0x400000)
    struct.pack_into(e+('Q'if wide else'I'),data,32 if wide else 28,size)
    struct.pack_into(e+'HHH',data,52 if wide else 40,size,phsize,2)
    if wide:
        struct.pack_into(e+'IIQQQQQQ',data,size,1,flags,0,0x400000,0,300,300,4096)
        struct.pack_into(e+'IIQQQQQQ',data,size+phsize,0x6474e551,stack,0,0,0,0,0,16)
    else:
        struct.pack_into(e+'IIIIIIII',data,size,1,0,0x400000,0,300,300,flags,4096)
        struct.pack_into(e+'IIIIIIII',data,size+phsize,0x6474e551,0,0,0,0,0,stack,16)
    return data
class ELFTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.p=pathlib.Path(self.tmp.name)/'elf'
    def tearDown(self):self.tmp.cleanup()
    def write(self,*a,**kw):self.p.write_bytes(fixture(*a,**kw));return self.p
    def test_64_le(self):self.assertEqual(inspect(self.write())['bits'],64)
    def test_64_be(self):self.assertEqual(inspect(self.write(big=True))['byte_order'],'big')
    def test_32_le(self):self.assertEqual(inspect(self.write(wide=False))['bits'],32)
    def test_32_be(self):self.assertEqual(inspect(self.write(wide=False,big=True))['byte_order'],'big')
    def test_rwx_refused(self):
        with self.assertRaises(ValueError):inspect(self.write(flags=7))
    def test_exec_stack_refused(self):
        with self.assertRaises(ValueError):inspect(self.write(stack=7))
    def test_bad_magic(self):
        self.p.write_bytes(b'not ELF')
        with self.assertRaises(ValueError):inspect(self.p)
    def test_truncated(self):
        self.write();self.p.write_bytes(self.p.read_bytes()[:290])
        with self.assertRaises(ValueError):inspect(self.p)
    def test_sstrip_preserves_load_64(self):
        self.write();sstrip(self.p);self.assertEqual(len(self.p.read_bytes()),300);inspect(self.p)
    def test_sstrip_preserves_load_32be(self):
        self.write(wide=False,big=True);sstrip(self.p);self.assertEqual(len(self.p.read_bytes()),300);inspect(self.p)
    def test_sstrip_bad_layout(self):
        self.write();b=bytearray(self.p.read_bytes());struct.pack_into('<H',b,54,55);self.p.write_bytes(b)
        with self.assertRaises(ValueError):sstrip(self.p)
    def test_fasm_requires_reserved_zero_slot(self):
        self.write();b=bytearray(self.p.read_bytes());b[176]=1;self.p.write_bytes(b)
        result=subprocess.run([sys.executable,str(ROOT/'tools/fasm_nx_stack.py'),str(self.p)],capture_output=True)
        self.assertNotEqual(result.returncode,0)

class PackageRefusalTests(unittest.TestCase):
    """Refuse incomplete/stale results without touching the real executables."""
    def check_refusal(self,mutation):
        import shutil,json,hashlib
        with tempfile.TemporaryDirectory()as d:
            root=pathlib.Path(d)
            for name in ['tools','tests','reports']:(root/name).mkdir()
            for name in ['tools/package_release.py','tests/regression.py']:
                shutil.copy2(ROOT/name,root/name)
            paths={'nolibc-x86_64':'nolibc_httpd/build/httpd','fasm-x86_64':'whitehttpd_fasm/whitehttpd','gas-x86_64':'whitehttpd_gas/whitehttpd','nolibc-mips32be':'nolibc_httpd/build/httpd.mipsbe','nolibc-mips32el':'nolibc_httpd/build/httpd.mipsel','asm-mips32be':'whitehttpd_mips/whitehttpd'}
            report={'passed':6,'failed':0,'test_suite_sha256':hashlib.sha256((root/'tests/regression.py').read_bytes()).hexdigest(),'binaries':{},'results':[]}
            for name,path in paths.items():
                dest=root/path;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(b'unit test fixture')
                report['binaries'][name]={'path':path,'sha256':hashlib.sha256(dest.read_bytes()).hexdigest()}
                report['results'].append({'variant':name,'result':'PASS'})
            mutation(report,root)
            (root/'reports/tests.json').write_text(json.dumps(report))
            result=subprocess.run([sys.executable,str(root/'tools/package_release.py')],capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0,result.stdout)
            return result.stderr
    def test_failed_results_refused(self):
        self.assertIn('failed test run',self.check_refusal(lambda report,root:report.update(failed=1)))
    def test_missing_variant_refused(self):
        self.assertIn('all native and MIPS',self.check_refusal(lambda report,root:report.update(results=[x for x in report['results']if x['variant']!='asm-mips32be'])))
    def test_stale_binary_refused(self):
        self.assertIn('binary changed',self.check_refusal(lambda report,root:(root/next(iter(report['binaries'].values()))['path']).write_bytes(b'changed')))
    def test_stale_suite_refused(self):
        self.assertIn('test suite changed',self.check_refusal(lambda report,root:report.update(test_suite_sha256='0'*64)))

if __name__=='__main__':unittest.main()
