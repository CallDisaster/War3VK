"""Strict, process-local empty implicit-layer discovery for native render labs.

No imports here query Windows or modify global environment/registry. The caller
must use a verified Medium token and freeze the actual loader identity. Unlike
VK_LOADER_LAYERS_DISABLE alone, VK_IMPLICIT_LAYER_PATH changes discovery before
stale system manifests are parsed. This is not a production compatibility fix.
Contract: Vulkan-Loader v1.4.341, loader/loader.c discovery blocks, and
docs/LoaderLayerInterface.md (Windows Layer Discovery).
"""
from pathlib import Path
import re

from run_render_host_transport_gate import require


LOADER_LINE = re.compile(
    r'^\s*(?:\[Vulkan Loader\]\s*)?([A-Z]+(?:\s*\|\s*[A-Z]+)*)\s*:\s*(.*)$')
NO_SETTINGS = 'No valid vk_loader_settings.json file found, no loader settings will be active'


def normalized_path(path):
    return str(path).replace('\\', '/').casefold()


def loader_records(text):
    records = []
    for line in text.splitlines():
        match = LOADER_LINE.fullmatch(line)
        if match:
            records.append((frozenset(value.strip() for value in match[1].split('|')), match[2].strip()))
        else:
            require(not line.lstrip().startswith('[Vulkan Loader]')
                    and not re.match(r'^\s*(?:ERROR|WARNING|WARN)\b', line),
                    'malformed/truncated loader diagnostic: '+line)
    return records


def check_loader_module_trace(text, loader_path, version='1.4.341'):
    records = loader_records(text)
    paths = [match[1] for channels, message in records if channels == {'INFO'}
             and (match := re.fullmatch(r'Using Vulkan Loader (.+)', message))]
    versions = [match[1] for channels, message in records if channels == {'INFO'}
                and (match := re.fullmatch(r'Vulkan Loader Version (\d+\.\d+\.\d+)', message))]
    require(paths and versions, 'actual loader module identity/version missing')
    require(all(normalized_path(path) == normalized_path(loader_path) for path in paths), 'unexpected actual loader path')
    require(all(value == version for value in versions), 'actual loader version outside frozen diagnostic contract')
    return dict(path=str(loader_path), version=version, actualLogPathBound=True,
                fileBytesProvenBySeparateFrozenIdentity=True)


def empty_directory_witness(path):
    path = Path(path)
    require(path.is_absolute() and path.is_dir() and not path.is_symlink()
            and not path.is_junction(), 'private layer source must be a real absolute directory')
    require(path.resolve(strict=True) == path, 'private layer source cannot traverse a redirected path')
    require(not list(path.iterdir()), 'private layer source must remain completely empty')
    stat = path.stat(follow_symlinks=False)
    return dict(path=str(path), device=stat.st_dev, fileId=stat.st_ino, empty=True)


def create_empty_source(output):
    output = Path(output).resolve(strict=True)
    path = output/'empty-implicit-layers'
    path.mkdir(exist_ok=False)
    return empty_directory_witness(path)


def check_empty_source_trace(text, path, allowed_layers=()):
    """Pure reader. A requested env var or a path substring is not proof."""
    wanted = normalized_path(path)
    require(Path(path).is_absolute(), 'absolute source path required')
    records = loader_records(text)
    require(records, 'actual loader trace missing')
    messages = [row[1] for row in records]
    starts = [i for i, value in enumerate(messages) if value == 'Searching for implicit layer manifest files']
    require(starts, 'actual implicit discovery blocks missing')
    for start in starts:
        require(len(messages) >= start+4 and messages[start+1] == 'In following locations:'
                and messages[start+3] == 'Found no files', 'incomplete/nonempty implicit discovery block')
        require(normalized_path(messages[start+2]) == wanted, 'implicit source differs from exact owned path')
        require(all(records[i][0] == {'LAYER'} for i in range(start, start+4)), 'implicit discovery channel differs')
    require(NO_SETTINGS in messages, 'absence of active loader settings not witnessed')
    for channels, message in records:
        require(not channels & {'ERROR', 'WARNING', 'WARN'}, 'loader diagnostic is not clean: '+message)
        require('implicitlayers' not in message.casefold(), 'implicit registry discovery still active')
        if 'loader settings' in message.casefold() or 'vk_loader_settings' in message.casefold():
            require(message == NO_SETTINGS, 'unproved loader settings override: '+message)
        require(not re.search(r'elevated|environment.*ignor|ignor.*environment', message, re.I),
                'secure process environment was ignored')
    inserted = re.findall(r'Insert(?:ed)? (?:instance|device) layer "([^"\r\n]+)"', text)
    require(all(name in allowed_layers for name in inserted), 'unexpected layer inserted')
    if not allowed_layers:
        require('Loading layer library' not in text, 'unexpected layer library loaded')
    return dict(mode='private-empty-override', path=str(path), discoveryBlocks=len(starts),
                implicitRegistryDiscovery=False, discoveredImplicitManifests=0,
                insertedLayers=inserted, actualDiscoveryProven=True)
