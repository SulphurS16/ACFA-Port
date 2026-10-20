#!/usr/bin/env python3
"""Fetch the pinned official ARM64 Slang SDK into ../local/deps, never globally.
No game files or credentials are read. --verify-only performs no download/install.
"""
import argparse,hashlib,json,os,shutil,subprocess,tempfile,zipfile
from pathlib import Path,PurePosixPath
VERSION='2026.20'
URL='https://github.com/shader-slang/slang/releases/download/v2026.20/slang-2026.20-macos-aarch64.zip'
SHA='ede1865bf086442c8786c5b7c0470ce5f4fd4f032eefe881356726ff9c445f40'
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--verify-only',action='store_true');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];deps=root/'local/deps';archive=deps/f'slang-{VERSION}-macos-aarch64.zip';sdk=deps/f'slang-{VERSION}'
    if not archive.exists():
        if a.verify_only:raise SystemExit('SDK archive missing; run without --verify-only to fetch it')
        deps.mkdir(parents=True,exist_ok=True)
        part=archive.with_suffix('.zip.part')
        subprocess.run(['curl','--fail','--location','--proto','=https','--tlsv1.2','--retry','2','--connect-timeout','20','--max-time','300','--output',str(part),URL],check=True)
        if digest(part)!=SHA:raise SystemExit('Downloaded SDK SHA256 mismatch; not installed')
        part.replace(archive)
    if digest(archive)!=SHA:raise SystemExit('SDK archive SHA256 mismatch; not installed')
    with zipfile.ZipFile(archive) as z:
        bad=z.testzip()
        if bad:raise SystemExit('SDK ZIP CRC failed: '+bad)
        entries=z.infolist()
        # Validate paths and symlink targets before invoking the macOS ZIP extractor.
        for entry in entries:
            path=PurePosixPath(entry.filename)
            if path.is_absolute() or '..' in path.parts:raise SystemExit('Unsafe SDK archive path')
            if (entry.external_attr>>16)&0o170000 == 0o120000:
                target=PurePosixPath(z.read(entry).decode())
                if target.is_absolute():raise SystemExit('Absolute SDK symlink target')
                resolved=os.path.normpath(str(path.parent/target))
                if resolved=='..' or resolved.startswith('../'):raise SystemExit('Escaping SDK symlink target')
    if not (sdk/'bin/slangc').exists():
        if a.verify_only:raise SystemExit('Extracted SDK missing; archive SHA/CRC passed')
        if sdk.exists():raise SystemExit('Existing incomplete SDK directory; inspect it before replacement')
        stage=Path(tempfile.mkdtemp(prefix='.slang-extract-',dir=deps))
        try:
            subprocess.run(['ditto','-x','-k',str(archive),str(stage)],check=True)
            if not (stage/'bin/slangc').is_file():raise SystemExit('Unexpected SDK layout')
            stage.replace(sdk)
        finally:
            if stage.exists():shutil.rmtree(stage)
    actual=subprocess.check_output([str(sdk/'bin/slangc'),'-version'],text=True,stderr=subprocess.STDOUT).strip()
    if actual!=VERSION:raise SystemExit('SDK compiler version mismatch: '+actual)
    print(json.dumps({'version':actual,'archive_sha256':SHA,'zip_entries':len(entries),'sdk':str(sdk),'verified':True},ensure_ascii=False))
if __name__=='__main__':main()
