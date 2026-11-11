"""Freeze complete native log bytes at launch return, then recheck them.

This is file evidence, not a process/GPU acceptance checker. A missing initial
manifest cannot be reconstructed from a later JSON summary or current files.
Failed launches still retain the available files and explicit missing names.
"""
from pathlib import Path
import re

from run_render_host_protocol_gate import identity
from run_render_host_transport_gate import require


SCHEMA = 'render-lab-initial-raw/v1'


def _names(values):
    require(type(values) is list and bool(values)
            and all(type(name) is str and name and Path(name).name == name
                    and name not in ('.', '..') for name in values), 'exact raw artifact names required')
    require(len(set(values)) == len(values), 'duplicate required raw artifact')
    return sorted(values)


def _population(directory, required, include_logs):
    names = set(required)
    if include_logs:
        names.update(path.name for path in directory.iterdir() if path.is_file() and path.suffix == '.log')
    return sorted(names)


def capture_raw_artifacts(directory, required_names, include_logs=False):
    """Call immediately after desktop.run returns, before parsing/verdicts."""
    directory = Path(directory).resolve(strict=True)
    required = _names(required_names)
    require(type(include_logs) is bool, 'raw log discovery flag')
    files, missing = [], []
    for name in _population(directory, required, include_logs):
        path = directory/name
        if not path.is_file():
            missing.append(name)
            continue
        require(path.resolve(strict=True) == path and not path.is_symlink(), 'redirected raw artifact')
        files.append(identity(path))
    return dict(schema=SCHEMA, capturedAt='launch-return', directory=str(directory),
                requiredNames=required, includeLogs=include_logs, files=files, missingNames=missing)


def verify_raw_artifacts(manifest, directory=None, required_names=None, include_logs=None):
    """Check the initial manifest, never replace its hashes with current ones.

Consumers of an older receipt also pass the independently expected directory,
required names and log-discovery policy; producer finalization can use its own
initial manifest. All files, including non-JSON diagnostics, remain bound.
    """
    require(type(manifest) is dict and set(manifest) == {
        'schema', 'capturedAt', 'directory', 'requiredNames', 'includeLogs', 'files', 'missingNames'},
        'initial raw manifest required; historical evidence cannot be retroactively frozen')
    require(manifest['schema'] == SCHEMA and manifest['capturedAt'] == 'launch-return', 'initial raw capture scope')
    root = Path(manifest['directory'])
    require(root.is_absolute() and root.resolve(strict=True) == root and root.is_dir(), 'raw artifact root')
    required = _names(manifest['requiredNames'])
    require(required == manifest['requiredNames'] and type(manifest['includeLogs']) is bool,
            'canonical raw capture policy')
    if directory is not None:
        require(root == Path(directory).resolve(strict=True), 'raw artifact directory differs')
    if required_names is not None:
        require(required == _names(required_names), 'required raw artifact population differs')
    if include_logs is not None:
        require(type(include_logs) is bool and manifest['includeLogs'] is include_logs, 'raw log discovery policy differs')
    require(type(manifest['missingNames']) is list and not manifest['missingNames'], 'initial raw artifacts missing')
    records = manifest['files']
    require(type(records) is list and records, 'initial raw file identities missing')
    names = []
    for record in records:
        require(type(record) is dict and set(record) == {'path', 'bytes', 'sha256'}
                and type(record['path']) is str and type(record['bytes']) is int and record['bytes'] >= 0
                and type(record['sha256']) is str and re.fullmatch(r'[0-9A-F]{64}', record['sha256']),
                'exact raw file identity required')
        path = Path(record['path'])
        require(path.is_absolute() and path.parent == root and path.resolve(strict=True) == path
                and not path.is_symlink(), 'raw artifact escaped or redirected')
        names.append(path.name)
        require(identity(path) == record, 'raw artifact changed since launch return: '+str(path))
    require(names == sorted(set(names)) and set(required) <= set(names), 'missing/duplicate raw file identity')
    require(names == _population(root, required, manifest['includeLogs']), 'raw artifact population changed')
    return records


def raw_manifests(receipt):
    """Existing captures only, so failed partial runs still keep their evidence."""
    if 'probeRawArtifacts' in receipt:
        yield receipt['probeRawArtifacts']
    yield from receipt.get('stepRawArtifacts', [])
    for case in receipt.get('cases', []):
        if 'initialRawArtifacts' in case:
            yield case['initialRawArtifacts']
