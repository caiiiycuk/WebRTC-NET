#!/usr/bin/env python3
"""Rebuild the pinned SDK overlay offline; never modify the supplied SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def require_hash(path, expected):
    if not path.is_file() or sha(path) != expected:
        raise SystemExit('Pinned input mismatch: ' + str(path))

def include_hash(root):
    digest = hashlib.sha256()
    for path in sorted(x for x in root.rglob('*') if x.is_file()):
        digest.update((path.relative_to(root).as_posix() + '\0' + sha(path) + '\n').encode())
    return digest.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-root', required=True, type=Path)
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--cxx', required=True, type=Path)
    parser.add_argument('--ar', required=True, type=Path)
    args = parser.parse_args()
    sdk, source, output = (x.resolve() for x in (args.sdk_root, args.source_root, args.output_dir))
    cxx, ar = args.cxx.resolve(), args.ar.resolve()
    if output.exists() or any(output.is_relative_to(x) or x.is_relative_to(output) for x in (sdk, source, HERE)):
        raise SystemExit('Output must be fresh and separate from input/product roots')
    lock = json.loads((HERE / 'inputs.lock.json').read_text())
    require_hash(HERE / 'sdk-tx-ownership.patch', lock['patch_sha256'])
    require_hash(HERE / 'portal_tx_snapshot_v2.h', lock['public_header_sha256'])
    require_hash(cxx, lock['compiler_sha256'])
    require_hash(ar, lock['archiver_sha256'])
    if include_hash(sdk / 'include') != lock['base_include_sha256']:
        raise SystemExit('Pinned SDK include tree mismatch')
    for name, expected in lock['libraries'].items():
        require_hash(sdk / 'lib' / name, expected)
    for item in lock['files']:
        if item['base_sha256']:
            root = source if item['path'].endswith('.cc') else sdk / 'include'
            require_hash(root / item['path'], item['base_sha256'])

    output.mkdir(parents=True)
    overlay = output / 'source'
    objects = output / 'objects'
    objects.mkdir()
    shutil.copytree(sdk / 'include', output / 'include')
    (output / 'lib').mkdir()
    for name in lock['libraries']:
        shutil.copyfile(sdk / 'lib' / name, output / 'lib' / name)
    for item in lock['files']:
        if item['base_sha256']:
            path = overlay / item['path']
            path.parent.mkdir(parents=True, exist_ok=True)
            root = source if item['path'].endswith('.cc') else sdk / 'include'
            shutil.copyfile(root / item['path'], path)
    commands = []
    def run(argv, cwd=output):
        argv = list(map(str, argv))
        commands.append(dict(argv=argv, cwd=str(cwd)))
        (output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
        subprocess.run(argv, cwd=cwd, check=True)
    run(['patch', '-p1', '--batch', '--forward', '-i', HERE / 'sdk-tx-ownership.patch'], overlay)
    for item in lock['files']:
        path = overlay / item['path']
        require_hash(path, item['patched_sha256'])
        if not path.name.endswith('.cc'):
            target = output / 'include' / item['path']
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)

    rebuilt = []
    for item in lock['files']:
        if not item['path'].endswith('.cc'):
            continue
        path = overlay / item['path']
        obj = objects / (path.stem + '.o')
        run([cxx, '-std=c++17', '-DWEBRTC_POSIX', '-DWEBRTC_LINUX', '-DNDEBUG',
             '-O2', '-fPIC', '-frtti', '-fno-exceptions',
             '-ffile-prefix-map=' + str(output) + '=/portal-sdk',
             '-ffile-prefix-map=' + str(sdk) + '=/sdk',
             '-I', overlay, '-I', sdk / 'include', '-c', path, '-o', obj])
        rebuilt.append(obj)
    archive = output / 'lib/libwebrtc.a'
    members = subprocess.check_output([str(ar), 't', str(archive)], text=True).splitlines()
    if any(members.count(obj.name) != 1 for obj in rebuilt):
        raise SystemExit('Replacement archive members must be unique')
    run([ar, 'rD', archive, *rebuilt])
    require_hash(archive, lock['rebuilt_archive_sha256'])
    receipt = dict(schema=1, qualification=lock['qualification'],
                   source_revision=lock['source_revision'],
                   recipe_sha256=sha(HERE / 'build.py'), lock_sha256=sha(HERE / 'inputs.lock.json'),
                   libraries={name: sha(output / 'lib' / name) for name in lock['libraries']},
                   objects={obj.name: sha(obj) for obj in rebuilt},
                   note='Pinned original SDK members plus exact-source replacements; not an upstream release reproduction')
    (output / 'build-receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))

if __name__ == '__main__':
    main()
