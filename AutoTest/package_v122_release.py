"""Create audited release assets locally (v1.22 pipeline, 1.30 adds the x64 renderer). Never push, deploy or launch."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

from package_v122_release_candidate import identity, pe_tables, create_text
from release_v122_package_audit import _pe_identity

VERSION = '1.30.00'
# Since 1.30 the player package carries the 64-bit renderer: with WarVK/x64 beside
# d3d9.dll the product starts it by itself (src/d3d9/war3/platform/war3_proxy_auto.cpp).
X64 = 'WarVK/x64'
FRONT_MEMBER = f'{X64}/warvk-d3d9-front.dll'
HOST_MEMBER = f'{X64}/warvk-d3d9-host.exe'
PLAYER = ('d3d9.dll', FRONT_MEMBER, HOST_MEMBER, f'{X64}/dxvk.conf',
          'README.md', 'README_CN.md', 'CHANGELOG.md', 'LICENSE',
          'COPYING', 'THIRD_PARTY_NOTICES.md', 'DEPENDENCY_LICENSES.txt',
          'WarVK_GPU_REQUIREMENTS.md', 'WarVK_LockD3D9.cmd', 'WarVK_UnlockD3D9.cmd',
          'WarVK_EvidenceStart.cmd')
# Player package members that live elsewhere in the repository.
PLAYER_SOURCES = {
    f'{X64}/dxvk.conf': 'tools/player/x64/dxvk.conf',
    'WarVK_GPU_REQUIREMENTS.md': 'docs/WarVK_GPU_REQUIREMENTS.md',
    'WarVK_LockD3D9.cmd': 'tools/player/WarVK_LockD3D9.cmd',
    'WarVK_UnlockD3D9.cmd': 'tools/player/WarVK_UnlockD3D9.cmd',
    'WarVK_EvidenceStart.cmd': 'tools/player/WarVK_EvidenceStart.cmd',
}
# The x86 front end of the renderer is the f8 build frozen on 2026-11-18
# (tools/render_host/d3d9_proxy.cpp and its wire headers have not changed since);
# every x64 gate since then ran this file. Digest of the unstripped file.
FRONT_SHA256 = '14E995B67B7435ABB5535F0019E855077D5F50EEBD602885CA0D0C0600609AB4'
# The host is built from the frozen source by AutoTest/run_render_host_candidate_build.py.
HOST_TARGET = 'tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe'
LLVM_TOOLS = Path('E:/Dev/Toolchains/warvk-rh0-llvm-mingw-20260616/portable/'
                  'llvm-mingw-20260616-ucrt-x86_64/bin')
# Only DLLs every supported Windows has: a toolchain runtime among the imports
# would mean a player's machine cannot start the file.
SYSTEM_IMPORTS = re.compile(
    r'^(api-ms-win-[a-z0-9-]+|kernel32|user32|gdi32|advapi32|shell32|ole32|oleaut32|'
    r'dbghelp|dwmapi|bcrypt|setupapi|ws2_32|winmm|imm32|version|psapi|ntdll|shlwapi|'
    r'crypt32|secur32|uxtheme|comdlg32)\.dll$', re.I)
AUTHOR = ('WarVK/action.txt', 'WarVK/call.txt', 'WarVK/define.txt',
          'WarVK/README.md', 'WarVK/MATH_CURVE_API.md', 'WarVK/WEATHER_API.md',
          'WarVK/icons/atom.blp', 'WarVK/icons/atom.png',
          'WarVK/jass/warvk_api.j', 'WarVK/jass/warvk_bridge.j',
          'WarVK/jass/warvk_constant.j', 'WarVK/jass/warvk_init.j',
          'WarVK/jass/warvk_smoke_test.j', 'WarVK/jass/warvk_weather_demo.j',
          'LICENSE', 'COPYING', 'THIRD_PARTY_NOTICES.md')
# The 2026-09-25 candidate the player kept running until this release. The
# release DLL is newer: it was re-tested in isolated AutoTest runs only.
PLAYER_RUNNING_PREDECESSOR_SHA = '1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF'
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


def verify_win64(path):
    pe = _pe_identity(path)
    if pe['machine'] != 0x8664 or pe['magic'] != 0x20B:
        raise ValueError('Architecture mismatch')
    return pe


def foreign_imports(names):
    """Imported DLLs that are not part of Windows."""
    return [name for name in names if not SYSTEM_IMPORTS.match(name)]


def strip_llvm(source, target, verify):
    """Stripped copy of an llvm-mingw build (front end, host), checked for architecture and imports."""
    if target.exists():
        raise ValueError('Strip target exists: ' + target.name)
    target.parent.mkdir(parents=True, exist_ok=True)
    original = identity(source)
    subprocess.run([str(LLVM_TOOLS / 'llvm-strip.exe'), '--strip-unneeded', '-o', str(target), str(source)], check=True)
    if identity(source) != original:
        raise ValueError('Strip altered the original: ' + source.name)
    verify(target)
    text = subprocess.run([str(LLVM_TOOLS / 'llvm-objdump.exe'), '-p', str(target)], check=True,
                          capture_output=True, text=True, errors='replace').stdout
    names = sorted(set(re.findall(r'DLL Name:\s*(\S+)', text)))
    if not names or foreign_imports(names):
        raise ValueError(f'{target.name} imports non-system DLLs: {foreign_imports(names)}')
    return dict(unstripped=original, packaged=identity(target), imports=names)


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
    manifest = dict(schema=1, kind='warvk-release', version=VERSION,
                    files=members(directory), evidence=evidence)
    create_text(directory / 'manifest.json', json.dumps(manifest, ensure_ascii=False, indent=2))
    with zipfile.ZipFile(archive, 'x', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(members(directory)):
            z.write(directory / p, p)
    verify_archive(directory, archive, payload)


def package(root, build, out, manifest_name, front, host_receipt):
    root, build, out = root.resolve(), build.resolve(), out.resolve()
    if out.exists() or out == root or root in out.parents:
        raise ValueError('Require a new external output directory')
    source = build / 'source'
    frozen = json.loads((build / manifest_name).read_text(encoding='utf-8'))
    for name, expected in frozen['files'].items():
        if identity(source / name) != expected:
            raise ValueError('Changed frozen input: ' + name)
        if name.startswith(('src/d3d9/', 'src/dxvk/', 'src/util/', 'subprojects/war3fx/', 'tools/render_host/')):
            if identity(root / name) != expected:
                raise ValueError('Runtime source not represented by build: ' + name)
    if identity(Path(front))['sha256'] != FRONT_SHA256:
        raise ValueError('Front end is not the frozen f8 file')
    host_build = json.loads(Path(host_receipt).read_text(encoding='utf-8'))
    host_dir = Path(host_build.get('build', '')).resolve()
    if not (host_build.get('ok') is True and host_build.get('sourcesPreserved') is True
            and host_build.get('protectedUnchanged') is True and host_dir.parent == source.resolve()):
        raise ValueError('Host was not built cleanly from the frozen source')
    host = host_dir / HOST_TARGET
    if identity(host) != {'size': host_build['host']['bytes'], 'sha256': host_build['host']['sha256'].upper()}:
        raise ValueError('Host differs from its build receipt')
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
    x64 = out / 'x64-stripped'
    stripped = {FRONT_MEMBER: strip_llvm(Path(front), x64 / 'warvk-d3d9-front.dll', verify_win32),
                HOST_MEMBER: strip_llvm(host, x64 / 'warvk-d3d9-host.exe', verify_win64)}
    # VersionInfo reads PE resources, not LoadLibrary/DllMain.
    version = json.loads(subprocess.check_output(['powershell', '-NoProfile', '-Command',
        f"$v=(Get-Item -LiteralPath '{dll}').VersionInfo; @{{v=$v.FileVersion;pre=$v.IsPreRelease;description=$v.FileDescription}}|ConvertTo-Json -Compress"], text=True))
    if version != {'v': VERSION, 'pre': False, 'description': f'WarVK {VERSION} Direct3D 9 Runtime'}:
        raise ValueError('Not a final version resource')
    evidence = dict(sourceCommit=commit, buildInputManifest=identity(build / manifest_name),
        configurationAudit=identity(build / 'release-configuration-audit.json'),
        playerDll=identity(dll), unstrippedDll=original_id,
        x64Renderer=dict(frontEnd=stripped[FRONT_MEMBER], frontEndFrozenSha256=FRONT_SHA256,
                         host=stripped[HOST_MEMBER], hostBuildReceipt=identity(Path(host_receipt))),
        mesonCpuTests=len(tests), assertionsEnabledInTests=True, staticScripts=len(results),
        userReleaseAuthorized=True,
        playerRunningPredecessorSha256=PLAYER_RUNNING_PREDECESSOR_SHA,
        playerEvidence='The player ran the 2026-09-25 predecessor; this exact DLL has no player acceptance yet',
        finalDllRuntimeRetested=True,
        finalDllRuntimeScope='Isolated AutoTest smoke runs on one NVIDIA GPU (see release notes)',
        universalGpuVisualOrApiAcceptance=False)
    player, author = out / 'player', out / 'author'
    player.mkdir(); author.mkdir()
    binaries = {'d3d9.dll': dll, FRONT_MEMBER: x64 / 'warvk-d3d9-front.dll', HOST_MEMBER: x64 / 'warvk-d3d9-host.exe'}
    for name in PLAYER:
        src = binaries.get(name) or root / PLAYER_SOURCES.get(name, name)
        (player / name).parent.mkdir(parents=True, exist_ok=True)
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
    player_zip = out / f'WarVK-{VERSION}-win32.zip'
    author_zip = out / f'WarVK-{VERSION}-author-kit.zip'
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
    parser.add_argument('--front', type=Path, required=True, help='the frozen f8 warvk-d3d9-front.dll (unstripped)')
    parser.add_argument('--host-receipt', type=Path, required=True,
                        help='receipt.json of run_render_host_candidate_build.py run in the frozen source')
    args = parser.parse_args()
    print(json.dumps(package(args.root, args.build_root, args.out, args.manifest, args.front, args.host_receipt),
                     indent=2))
