#!/usr/bin/env python3
"""Stage exact tested executables and checksums. Does not sign or publish."""
import pathlib,hashlib,shutil,json,sys
root=pathlib.Path(__file__).resolve().parents[1];dist=root/'dist';dist.mkdir(exist_ok=True)
files={'whitehttpd-fasm-x86_64':'whitehttpd_fasm/whitehttpd','whitehttpd-gas-x86_64':'whitehttpd_gas/whitehttpd','whitehttpd-mips32be':'whitehttpd_mips/whitehttpd','whitehttpd-nolibc-x86_64':'nolibc_httpd/build/httpd','whitehttpd-nolibc-mips32be':'nolibc_httpd/build/httpd.mipsbe','whitehttpd-nolibc-mips32el':'nolibc_httpd/build/httpd.mipsel'}
report=json.loads((root/'reports/tests.json').read_text())
if report.get('test_suite_sha256')!=hashlib.sha256((root/'tests/regression.py').read_bytes()).hexdigest():raise SystemExit('test suite changed after run')
if report['failed']:raise SystemExit('refusing to package a failed test run')
variants={r['variant']for r in report['results']}
native={'nolibc-x86_64','fasm-x86_64','gas-x86_64'}
mips={'nolibc-mips32be','nolibc-mips32el','asm-mips32be'}
native_only='--native-only' in sys.argv
if variants!=native|mips:
 if not(native_only and variants==native):
  raise SystemExit('all native and MIPS variants must be tested before packaging')
 print('WARNING: MIPS runtime tests were not executed (no qemu-user available).')
 print('WARNING: MIPS binaries are build-verified and ELF-checked only.')
# Refuse stale test reports: compare actual executable bytes, not timestamps.
for item in report.get('binaries',{}).values():
 if hashlib.sha256((root/item['path']).read_bytes()).hexdigest()!=item['sha256']:
  raise SystemExit('binary changed after testing: '+item['path'])
if len(report.get('binaries',{}))!=6:
 if not(native_only and len(report.get('binaries',{}))==3):
  raise SystemExit('missing tested binary hashes')
shutil.rmtree(dist);dist.mkdir()
for name,src in files.items():shutil.copy2(root/src,dist/name)
for name in ['LICENSE','NOTICE','SECURITY.md','README.md','README_RU.md']:shutil.copy2(root/name,dist/name)
shutil.copytree(root/'nolibc_httpd/third_party/nolibc',dist/'licenses/nolibc-headers')
(dist/'licenses/darkhttpd-COPYING').write_bytes((root/'darkhttpd_c/COPYING').read_bytes())
(dist/'licenses/FASM-license.txt').write_bytes((root/'whitehttpd_fasm/fasm/fasm/license.txt').read_bytes())
shutil.copytree(root/'LICENSES',dist/'licenses/texts')
(dist/'reports').mkdir()
for name in ['tests.json','elf.json']:
 shutil.copy2(root/'reports'/name,dist/'reports'/name)
(dist/'README.md').rename(dist/'SOURCE_README.md')
(dist/'README.md').write_text("""# whitehttpd binary release candidate

Six exact tested server executables are in this directory. See reports/tests.json
for executable hashes and named test results; reports/elf.json for ELF checks.
SHA256SUMS checks integrity, not a cryptographic signature by the author.

Run as an unprivileged user, with a public-only document root, not a repository:

    ./whitehttpd-nolibc-x86_64 /path/to/public --port 8080 --addr 127.0.0.1 --workers 1
    qemu-mips ./whitehttpd-nolibc-mips32be /path/to/public --port 8080 --addr 127.0.0.1 --workers 1

QEMU is required to run MIPS binaries on x86, but not on a compatible MIPS device.
MIPS user-mode testing is not a real-router compatibility certification.
Assembly directory listing is disabled (403); Range ignored. FASM/MIPS ASM bind
IPv4 wildcard and reject --addr; isolate them with OS network policy.
Read SECURITY.md before use. This is not a production-ready/security guarantee.

Source instructions in SOURCE_README.md and README_RU.md refer to the matching
source release, not to build directories within this binary-only payload.
Obtain corresponding source from the accompanying source ZIP or the matching tag
of https://github.com/Svarkovsky/whitehttpd (do not substitute an unrelated tag).
Project-owned code is AGPL-3.0-or-later. LICENSE is complete. NOTICE lists third
parties. In this binary payload the nolibc headers/notices are in
licenses/nolibc-headers, full MIT/LGPL texts in licenses/texts, and other component
licenses alongside them. Do not discard these notices when redistributing.
""")
checks=[];manifest={}
for p in sorted(dist.rglob('*')):
 if p.is_file()and p.name not in ('SHA256SUMS','manifest.json'):
  digest=hashlib.sha256(p.read_bytes()).hexdigest();checks.append(digest+'  '+str(p.relative_to(dist))+'\n');manifest[str(p.relative_to(dist))]={'bytes':p.stat().st_size,'sha256':digest}
(dist/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
checks.append(hashlib.sha256((dist/'manifest.json').read_bytes()).hexdigest()+'  manifest.json\n')
(dist/'SHA256SUMS').write_text(''.join(checks));print('Staged',len(files),'binaries; checksums are not digital signatures.')
