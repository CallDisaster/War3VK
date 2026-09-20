"""Create audited v1.22 release assets locally. Never push, deploy or launch."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

from package_v122_release_candidate import identity, pe_tables, create_text
from release_v122_package_audit import _pe_identity

PLAYER = ('d3d9.dll', 'README.md', 'README_CN.md', 'CHANGELOG.md', 'LICENSE',
          'COPYING', 'THIRD_PARTY_NOTICES.md', 'DEPENDENCY_LICENSES.txt')
AUTHOR = ('WarVK/action.txt', 'WarVK/call.txt', 'WarVK/define.txt',
          'WarVK/README.md', 'WarVK/MATH_CURVE_API.md',
          'WarVK/icons/atom.blp', 'WarVK/icons/atom.png',
          'WarVK/jass/warvk_api.j', 'WarVK/jass/warvk_bridge.j',
          'WarVK/jass/warvk_constant.j', 'WarVK/jass/warvk_init.j',
          'WarVK/jass/warvk_smoke_test.j', 'LICENSE', 'COPYING', 'THIRD_PARTY_NOTICES.md')
RC_SHA = '62BF9F402C90DE8C284F5C9C194517EC165F208F0A03F33A75D2F1FDB56381C3'
DEPENDENCY_LICENSES = ('COPYING', 'LICENSE', 'src/minhook/LICENSE.txt',
    'subprojects/imgui/LICENSE.txt', 'subprojects/dxbc-spirv/LICENSE',
    'subprojects/libdisplay-info/LICENSE', 'smaa/LICENSE.txt',
    'include/vulkan/LICENSE.md', 'include/spirv/LICENSE',
    'subprojects/StormBreaker/LICENSE.txt')


def verify_win32(path):
    pe = _pe_identity(path)
    if not pe['i386'] or pe['x64']:
        raise ValueError('Architecture mismatch')
    return pe


def members(directory):
    result = {}
    for p in directory.rglob('*'):
        if p.is_symlink():
            raise ValueError('Symlink in package')
        if p.is_file():
            result[p.relative_to(directory).as_posix()] = identity(p)
    return result


def verify_archive(directory, archive, payload):
    expected = set(payload) | {'manifest.json'}
    actual = members(directory)
    if set(actual) != expected:
        raise ValueError('Package membership mismatch')
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
    if set(manifest['files']) != set(payload):
        raise ValueError('Manifest membership mismatch')
    for p in payload:
        if manifest['files'][p] != actual[p]:
            raise ValueError('Manifest identity mismatch: ' + p)
    with zipfile.ZipFile(archive) as z:
        if len(z.namelist()) != len(expected) or set(z.namelist()) != expected:
            raise ValueError('ZIP membership mismatch')
        if z.testzip() is not None:
            raise ValueError('ZIP CRC failed')
        for p in expected:
            data = z.read(p)
            if {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest().upper()} != actual[p]:
                raise ValueError('ZIP bytes mismatch: ' + p)


def make_archive(directory, archive, payload, evidence):
    if set(members(directory)) != set(payload):
        raise ValueError('Unexpected payload')
    manifest = dict(schema=1, kind='warvk-release', version='1.22.00',
                    files=members(directory), evidence=evidence)
    create_text(directory / 'manifest.json', json.dumps(manifest, ensure_ascii=False, indent=2))
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(members(directory)):
            z.write(directory / p, p)
    verify_archive(directory, archive, payload)


def package(root, build, out, manifest_name):
    root, build, out = root.resolve(), build.resolve(), out.resolve()
    if out.exists() or out == root or root in out.parents:
        raise ValueError('Require a new external output directory')
    source = build / 'source'
    frozen = json.loads((build / manifest_name).read_text(encoding='utf-8'))
    for name, expected in frozen['files'].items():
        if identity(source / name) != expected:
            raise ValueError('Changed frozen input: ' + name)
        if name.startswith(('src/d3d9/', 'src/dxvk/', 'src/util/', 'subprojects/war3fx/')):
            if identity(root / name) != expected:
                raise ValueError('Runtime source not represented by build: ' + name)
    commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    # Only preserved dirty external worktrees may remain; no uncommitted product input.
    status = subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain', '--untracked-files=all'], text=True)
    if any(line[3:] != 'subprojects/StormBreaker' for line in status.splitlines()):
        raise ValueError('Uncommitted release files')
    config = json.loads((build / 'release-configuration-audit.json').read_text(encoding='utf-8-sig'))
    if config != {'ok': True, 'errors': []}:
        raise ValueError('Release configuration failed')
    results = json.loads((build / 'static-release/results.json').read_text(encoding='utf-8-sig'))
    expected_static = {p.name for p in (source / 'AutoTest').glob('test_*_static.py')}
    if len(results) != len(expected_static) or {x['name'] for x in results} != expected_static or any(x['exitCode'] != 0 for x in results):
        raise ValueError('Static membership/result failure')
    tests = [json.loads(x) for x in (build / 'build32-tests/meson-logs/testlog.json').read_text(encoding='utf-8').splitlines() if x.strip()]
    declared = json.loads((build / 'build32-tests/meson-info/intro-tests.json').read_text(encoding='utf-8'))
    if len(tests) != len(declared) or any(x['result'] != 'OK' for x in tests):
        raise ValueError('CPU test failure')
    for build_name, required in (('build32-tests', 'false'), ('build32-product', 'if-release')):
        opts = {x['name']: x['value'] for x in json.loads((build / build_name / 'meson-info/intro-buildoptions.json').read_text(encoding='utf-8'))}
        if opts['b_ndebug'] != required:
            raise ValueError('Assertion configuration mismatch')
    if 'ninja: no work to do.' not in (build / 'release-no-work.log').read_text(encoding='utf-8-sig'):
        raise ValueError('Missing no-work receipt')
    original = build / 'build32-product/src/d3d9/d3d9.dll'
    original_id = identity(original)
    objdump = Path('E:/Dev/MinGW/bin/objdump.exe')
    out.mkdir(parents=True, exist_ok=False)
    dll = out / 'd3d9.dll'
    # PE binutils otherwise stamps the current time and changes the digest on
    # every invocation, despite identical machine code. Preserve source dates.
    subprocess.run(['E:/Dev/MinGW/bin/strip.exe', '--strip-unneeded', '--preserve-dates',
                    '-o', str(dll), str(original)], check=True)
    if pe_tables(dll, objdump) != pe_tables(original, objdump) or identity(original) != original_id:
        raise ValueError('Strip altered API or original')
    verify_win32(dll)
    # VersionInfo reads PE resources, not LoadLibrary/DllMain.
    version = json.loads(subprocess.check_output(['powershell', '-NoProfile', '-Command',
        f"$v=(Get-Item -LiteralPath '{dll}').VersionInfo; @{{v=$v.FileVersion;pre=$v.IsPreRelease;description=$v.FileDescription}}|ConvertTo-Json -Compress"], text=True))
    if version != {'v': '1.22.00', 'pre': False, 'description': 'WarVK 1.22.00 Direct3D 9 Runtime'}:
        raise ValueError('Not a final version resource')
    evidence = dict(sourceCommit=commit, buildInputManifest=identity(build / manifest_name),
        configurationAudit=identity(build / 'release-configuration-audit.json'),
        playerDll=identity(dll), unstrippedDll=original_id,
        mesonCpuTests=len(tests), assertionsEnabledInTests=True, staticScripts=len(results),
        userReleaseAuthorized=True, playerAcceptedRcSha256=RC_SHA,
        deltaAfterPlayerTest='Render Stats telemetry fix and final version metadata; no budget/caster/fence policy changes',
        finalDllRuntimeRetested=False, universalGpuVisualOrApiAcceptance=False)
    player, author = out / 'player', out / 'author'
    player.mkdir(); author.mkdir()
    for name in PLAYER:
        src = dll if name == 'd3d9.dll' else root / name
        if name == 'DEPENDENCY_LICENSES.txt':
            notices = '\n\n'.join('===== ' + p + ' =====\n' +
                (source / p).read_text(encoding='utf-8') for p in DEPENDENCY_LICENSES)
            create_text(player / name, notices)
        else:
            shutil.copyfile(src, player / name)
    for name in AUTHOR:
        dest = author / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, dest)
    player_zip = out / 'WarVK-1.22.00-win32.zip'
    author_zip = out / 'WarVK-1.22.00-author-kit.zip'
    make_archive(player, player_zip, PLAYER, evidence)
    make_archive(author, author_zip, AUTHOR, evidence)
    assets = {p.name: identity(p) for p in (dll, player_zip, author_zip)}
    create_text(out / 'SHA256SUMS.txt', ''.join(f"{v['sha256']}  {k}\n" for k, v in assets.items()))
    create_text(out / 'release-audit.json', json.dumps(dict(ok=True, assets=assets, evidence=evidence), indent=2))
    return assets


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--build-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--manifest', default='source-release-1.json')
    args = parser.parse_args()
    print(json.dumps(package(args.root, args.build_root, args.out, args.manifest), indent=2))
