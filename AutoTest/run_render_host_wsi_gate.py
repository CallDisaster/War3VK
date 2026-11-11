"""RH3 actual x86 HWND/message pump -> x64 Vulkan swapchain laboratory.

Hidden non-activating owned window only; no game, deployment or global input.
Requires VK_EXT_swapchain_maintenance1 present completion fences, rather than
pretending a render fence means presentation has released its resources.
This is a WSI function gate, NOT a visible/FPS, complete D3D9 or memory-saving gate.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke
from run_render_host_transport_gate import strict_json, child_settled, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', required=True, type=Path)
    parser.add_argument('--compiler64', required=True, type=Path)
    parser.add_argument('--vulkan-sdk', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows-only lab')
    for path in (args.compiler32, args.compiler64, args.vulkan_sdk):
        require(path.is_absolute(), 'explicit absolute compiler/SDK path required')
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'RH3_HIDDEN_CROSS_BITNESS_WSI_LAB', 'ok': False,
               'gameTouched': False, 'shippingDllBuilt': False, 'rendererMigrated': False,
               'visualAcceptance': False, 'performanceAcceptance': False,
               'memorySavingsProven': False, 'globalInputUsed': False,
               'watchdogTerminationIsFailure': True, 'sources': [], 'builds': []}
    try:
        sdk = args.vulkan_sdk.resolve(strict=True)
        include = sdk / 'Include'
        require((include / 'vulkan/vulkan.h').is_file(), 'SDK headers missing')
        receipt['vulkanHeader'] = identity(include / 'vulkan/vulkan_core.h')
        names = ('protocol.h', 'protocol.cpp', 'win32_transport.h', 'win32_transport.cpp',
                 'win32_security.h', 'win32_security.cpp', 'owned_child.h', 'owned_child.cpp',
                 'wsi_lab_protocol.h', 'wsi_host_main.cpp')
        sources = [ROOT / 'tools/render_host' / n for n in names]
        sources += [Path(__file__).resolve(), ROOT / 'AutoTest/test_render_host_wsi_parent.cpp',
                    ROOT / 'AutoTest/run_render_host_protocol_gate.py',
                    ROOT / 'AutoTest/run_render_host_transport_gate.py']
        receipt['sources'] = [identity(path) for path in sources]
        common = ['tools/render_host/protocol.cpp', 'tools/render_host/win32_transport.cpp',
                  'tools/render_host/win32_security.cpp']
        executables = {}
        # Serial, BelowNormal; never shares or mutates a shipping DLL build tree.
        for bits, compiler in ((64, args.compiler64), (32, args.compiler32)):
            compiler = compiler.resolve(strict=True)
            exe = output / f'rh3-wsi-{bits}.exe'
            source = ['tools/render_host/wsi_host_main.cpp'] if bits == 64 else [
                'tools/render_host/owned_child.cpp', 'AutoTest/test_render_host_wsi_parent.cpp']
            command = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                       '-Wno-missing-field-initializers', '-Wno-cast-function-type',
                       '-D_WIN32_WINNT=0x0a00', '-DNOMINMAX', '-static', '-municode',
                       '-I', include, *common, *source, '-lbcrypt', '-ladvapi32', '-luser32', '-o', exe]
            stdout, stderr = invoke(command, 180)
            require(not stdout and not stderr, 'compiler diagnostics present')
            require(pe_identity(exe) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)),
                    'wrong PE architecture')
            receipt['builds'].append({'bits': bits, 'compiler': identity(compiler),
                                      'exe': identity(exe), 'argv': list(map(str, command))})
            executables[bits] = exe
        host_log = output / 'host.json'
        start = time.monotonic()
        process = subprocess.Popen([str(executables[32]), str(executables[64]), str(host_log)],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
        try:
            stdout, stderr = process.communicate(timeout=40)
        except subprocess.TimeoutExpired:
            process.kill()  # Exact owned Popen handle; native Job contains its child.
            stdout, stderr = process.communicate(timeout=10)
            (output / 'parent.stdout').write_bytes(stdout)
            (output / 'parent.stderr').write_bytes(stderr)
            raise RuntimeError('outer watchdog terminated exact parent: failed WSI gate')
        receipt['seconds'] = time.monotonic() - start
        (output / 'parent.stdout').write_bytes(stdout)
        (output / 'parent.stderr').write_bytes(stderr)
        # Preserve native failure diagnostics before refusing acceptance.
        receipt['exitCode'] = process.returncode
        parent = strict_json(stdout.decode('utf-8'))
        receipt['parent'] = parent
        if host_log.is_file():
            receipt['host'] = strict_json(host_log.read_text(encoding='utf-8'))
            receipt['hostLog'] = identity(host_log)
        if parent.get('childPid', 0) and parent.get('childCreation', 0):
            receipt['settlement'] = child_settled(parent['childPid'], parent['childCreation'])
        require(process.returncode == 0 and not stderr, 'native parent failed; inspect parent/host receipt')
        host = receipt['host']
        require(parent['ok'] is True and parent['bits'] == 32 and host['bits'] == 64 and host['ok'] is True,
                'native width/success')
        require(parent['peerVerified'] is True and host['peerVerified'] is True and
                host['windowOwnerVerified'] is True, 'peer/window authority missing')
        require(parent['childExit'] == 0 and parent['exchanges'] == host['accepted'] == 6,
                'not the expected complete protocol')
        require(parent['presented'] == parent['gpuCompleted'] == host['presented'] == host['gpuCompleted'] == 6,
                'render/presentation completion counts')
        require(parent['resizes'] == host['resizes'] == 1 and host['swapchainsCreated'] == 2,
                'resize/recreate not covered')
        require(host['normalGpuCleanup'] is True and host['presentFenceRequired'] is True and
                host['reason'] == 'none' and host['vkResult'] == 0, 'GPU cleanup/fence/result')
        require(parent['parentVulkanAbsent'] is True and parent['windowDestroyed'] is True and
                parent['messagesDispatched'] > 0 and parent['windowVisible'] is False,
                'parent is not the thin hidden window owner')
        require(host['cpuImageTransfers'] == 0 and parent['gameTouched'] is False and
                host['gameTouched'] is False and parent['globalInputUsed'] is False, 'scope violation')
        require(receipt['sources'] == [identity(p) for p in sources], 'source changed during gate')
        for build in receipt['builds']:
            require(identity(Path(build['exe']['path'])) == build['exe'], 'exe changed during gate')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output / 'receipt.json'),
                      'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
