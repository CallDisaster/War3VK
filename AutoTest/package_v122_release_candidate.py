"""Create a NEW player RC package after offline gates; never deploy or launch."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

from release_v122_package_audit import audit_package, REQUIRED_FLAGS, BUILD_CONFIG_EXPECTED


def identity(path: Path) -> dict:
    data = path.read_bytes()
    return {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest().upper()}


def create_text(path: Path, text: str) -> None:
    with path.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(text)


def pe_tables(path: Path, objdump: Path) -> dict:
    text = subprocess.check_output([str(objdump), '-p', str(path)], text=True)
    if 'file format pei-i386' not in text:
        raise ValueError('Not an i386 PE image')
    exports = text.split('[Ordinal/Name Pointer] Table', 1)[1].split('PE File Base Relocations', 1)[0]
    names = re.findall(r'^\s*\[\s*\d+\]\s+\+base\[\s*\d+\]\s+[0-9a-f]+\s+(\S+)\s*$', exports, re.M)
    imports = re.findall(r'DLL Name:\s*(\S+)', text)
    if not names or not imports:
        raise ValueError('Missing import/export proof')
    full_export_table = text.split('The Export Tables', 1)[1].split('PE File Base Relocations', 1)[0]
    return {'imports': sorted(imports), 'exports': sorted(names),
            'exportTableSha256': hashlib.sha256(full_export_table.encode()).hexdigest().upper()}


def package(build_root: Path, destination: Path, tools_root: Path) -> dict:
    build_root, destination = build_root.resolve(), destination.resolve()
    archive = destination.parent / (destination.name + '.zip')
    if destination.exists() or archive.exists():
        raise FileExistsError('RC package or archive already exists')
    source = build_root / 'source'
    source_manifest = build_root / 'source-final-2.json'
    source_data = json.loads(source_manifest.read_text(encoding='utf-8'))
    for relative, expected in source_data['files'].items():
        if identity(source / relative) != expected:
            raise ValueError('Frozen source changed: ' + relative)
    config = json.loads((build_root / 'configuration-audit-final.json').read_text(encoding='utf-8-sig'))
    if config != {'ok': True, 'errors': []}:
        raise ValueError('Product configuration did not pass')
    static_results = json.loads((build_root / 'static-accepted/results.json').read_text(encoding='utf-8-sig'))
    if len(static_results) != len(list((source / 'AutoTest').glob('test_*_static.py'))):
        raise ValueError('Static test membership/count mismatch')
    if any(x['exitCode'] != 0 for x in static_results):
        raise ValueError('Static test failed')
    test_log = build_root / 'build32-tests/meson-logs/testlog.json'
    tests = [json.loads(x) for x in test_log.read_text(encoding='utf-8').splitlines() if x.strip()]
    declared_tests = json.loads((build_root / 'build32-tests/meson-info/intro-tests.json').read_text(encoding='utf-8'))
    if len(tests) != len(declared_tests) or any(x['result'] != 'OK' for x in tests):
        raise ValueError('CPU tests missing, skipped or failed')
    original = build_root / 'build32-product/src/d3d9/d3d9.dll'
    before = identity(original)
    before_tables = pe_tables(original, tools_root / 'objdump.exe')
    destination.mkdir(parents=True, exist_ok=False)
    dll = destination / 'd3d9.dll'
    # --strip-unneeded changes only the package copy; preserve full local symbols.
    subprocess.run([str(tools_root / 'strip.exe'), '--strip-unneeded', '-o', str(dll), str(original)], check=True)
    if before_tables != pe_tables(dll, tools_root / 'objdump.exe'):
        raise ValueError('Stripping changed imported/exported API')
    if identity(original) != before:
        raise ValueError('Build DLL changed during packaging')
    target = identity(dll)
    readme = f'''# WarVK 1.22.00 RC1 — 正式配置测试候选

这是发布前的最后一轮测试包，尚未发布或认证为稳定版。只支持 Warcraft III 1.27a / 32位。

## 安装与回退

1. 完全退出魔兽和编辑器。先把游戏目录现有 d3d9.dll 复制到独立备份目录，不要覆盖旧备份。
2. 将本包 d3d9.dll 复制到你实际测试的 E:\\Work\\Warcraft III，和 War3.exe 放在一起。
3. 使用普通游戏启动方式；不要使用之前强制开启重型取证或实验变量的诊断启动器。
4. 回退时完全退出游戏后，将第1步保存的原 DLL 复制回来。无需修改 War3.exe、地图或注册表。

本次没有自动部署。交付时磁盘上的旧玩家候选仍是 FAC75C10...，不是本RC。

## 本包默认行为

- 保留快照position/UV/index按保留寿命分页、缓存释放修正与384MiB容量边界。
- 保留蒙皮矩阵来源/组数/坐标空间保护；修复候选仍需高压移动视角视觉确认。
- 保留原生空同步点优化和异步截图；不需要开始性能日志才能启用同步优化。
- Ctrl+F1 的 Render Stats 展示快照页、Arena及相关资源统计。
- 默认关闭重型帧取证、图像历史与开发实验。显式环境变量仍可覆盖诊断默认值。
- 未加入未完成Water、原版模型自动点光接管或64位渲染器；已有作者显式点光API保留。

## 建议这轮测试

使用2560×1440与之前相同地图/画质；每张图先用新游戏进程。

1. 大地图反复去不同区域，压低视角，再移开并返回，观察阴影是否持续存在和恢复。
2. 高压多单位时移动和反复俯仰，打开体积雾，检查短时拉长阴影/裂缝及合法对象投影。
3. 打开Ctrl+F1查看Render Stats是否有数据；分别在性能日志关闭/开启时观察帧率。
4. 测试原生截图、正常退出；有条件时检查太阳开关、局部雾和Froxel画面。

若异常，请保留性能HTML、截图、地图名和发生顺序。无需为测试强制开启重型取证。
同进程跨地图、点阴影摩尔纹以及未覆盖API仍不能据此宣称已完全修复。

## 身份与证据边界

DLL：{target['size']:,} bytes
SHA-256：{target['sha256']}
PE32/i386，Windows版本1.22.00，明确标记PRERELEASE / RC1。
新目录构建；{len(tests)}项断言开启的Meson CPU测试、{len(static_results)}个静态脚本通过。
配置、PE、导入/导出与ZIP校验不等于GPU/实机/视觉验收；本次组合等待你判定。
详见pack.json及CHANGELOG.md（候选更新草稿，不是已发布日志）。
'''
    create_text(destination / 'README.md', readme)
    create_text(destination / 'VERSION', '1.22.00-RC1\n')
    create_text(destination / 'CHANGELOG.md',
                '# RC1构建状态补充\n\n本包已经按正式配置构建1.22.00 RC1；'
                '以下保留发布准备草稿与历史验收边界，其中待生成版本资源/构建的描述已由本包pack.json更新。'
                '尚未发布，最终组合仍待玩家测试。\n\n' +
                (source / 'docs/RELEASE_NOTES_1.22.00_DRAFT.md').read_text(encoding='utf-8'))
    license_paths = ['COPYING', 'LICENSE', 'src/minhook/LICENSE.txt', 'subprojects/imgui/LICENSE.txt',
                     'subprojects/dxbc-spirv/LICENSE', 'subprojects/libdisplay-info/LICENSE',
                     'smaa/LICENSE.txt', 'include/vulkan/LICENSE.md', 'include/spirv/LICENSE',
                     'subprojects/StormBreaker/LICENSE.txt']
    notices = '\n\n'.join('===== ' + p + ' =====\n' + (source / p).read_text(encoding='utf-8') for p in license_paths)
    create_text(destination / 'COPYING.txt', notices)
    receipt = {'candidate': '1.22.00-RC1', 'sourceHead': source_data['sourceHead'],
               'sourceManifest': identity(source_manifest), 'buildOptions': identity(build_root / 'build32-product/meson-info/intro-buildoptions.json'),
               'unstrippedDll': before, 'playerDll': target, 'apiTables': before_tables,
               'mesonCpuTests': len(tests), 'cpuAssertionsEnabled': True,
               'staticScripts': len(static_results), 'runtimeValidated': False,
               'published': False, 'sourceWorkspaceHasUncommittedReleasePreparation': True}
    create_text(destination / 'pack.json', json.dumps(receipt, ensure_ascii=False, indent=2))
    files = {p.name: identity(p) for p in destination.iterdir() if p.is_file()}
    manifest = {'schema': 1, 'kind': 'warvk-offline-candidate', 'scope': 'player',
                'target': {'path': 'd3d9.dll', **target}, 'files': files,
                'flags': dict.fromkeys(REQUIRED_FLAGS, False), 'buildConfig': BUILD_CONFIG_EXPECTED,
                'limits': ['Offline RC only; awaiting player visual/GPU/runtime acceptance.',
                           'Excluded: unfinished Water, automatic model lights, x64 product.',
                           'Published changelog and remote release remain unchanged.']}
    create_text(destination / 'manifest.json', json.dumps(manifest, ensure_ascii=False, indent=2))
    preaudit = audit_package(destination, expect_sha256=target['sha256'], expect_size=target['size'])
    if not preaudit['ok']:
        raise ValueError(preaudit['errors'])
    with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(destination.iterdir()):
            z.write(p, p.name)
    result = audit_package(destination, archive_path=archive, expect_sha256=target['sha256'], expect_size=target['size'])
    if not result['ok']:
        raise ValueError(result['errors'])
    result['archive'] = {'path': str(archive), **identity(archive)}
    create_text(build_root / 'package-audit.json', json.dumps(result, ensure_ascii=False, indent=2))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-root', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--tools-root', type=Path, default=Path('E:/Dev/MinGW/bin'))
    args = parser.parse_args()
    result = package(args.build_root, args.destination, args.tools_root)
    print(json.dumps({'ok': result['ok'], 'dll': result['target'], 'archive': result['archive']}, ensure_ascii=False, indent=2))
