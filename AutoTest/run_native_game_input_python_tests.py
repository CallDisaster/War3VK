"""Frozen targeted Python/source/fake checks. No native, game, or full-suite claim."""
import argparse
import json
from pathlib import Path
import re
import sys
from run_render_host_protocol_gate import ROOT, identity
from run_render_host_core_gate import execute

TESTS = (
    'test_d3d9_fixed_function_shared_contract.py',
    'test_d3d9_migration_census.py',
    'test_fixed_function_key_tool_static.py',
    'test_native_d3d9_survey_static.py',
    'test_d3d9_migration_census_integration.py',
    'test_fixed_function_key_equivalence.py',
)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.parent != ROOT/'AutoTest/artifacts':
        raise ValueError('fresh evidence child required')
    out.mkdir(exist_ok=False)
    sources = [ROOT/'AutoTest'/name for name in TESTS]
    before = [identity(p) for p in sources]
    receipt = dict(schema='warvk-native-game-input-python-tests/v1',ok=False,
                   sources=before, runs=[], tests=0, nativeRun=False, fullSuite=False)
    try:
        for path in sources:
            prefix = out/path.stem
            step = execute([sys.executable,'-B',path],ROOT,prefix,60)
            stderr = prefix.with_suffix('.stderr').read_text('utf-8')
            match = re.search(r'Ran ([0-9]+) tests? in .*\n\nOK\s*$',stderr)
            if not match:
                raise ValueError('exact unittest completion missing: '+path.name)
            receipt['tests'] += int(match[1])
            receipt['runs'].append(dict(test=path.name, tests=int(match[1]), execution=step,
                stdout=identity(prefix.with_suffix('.stdout')),stderr=identity(prefix.with_suffix('.stderr'))))
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = f'{type(error).__name__}: {error}'
    finally:
        receipt['sourcesUnchanged'] = before == [identity(p) for p in sources]
        receipt['ok'] &= receipt['sourcesUnchanged']
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps({k:receipt.get(k) for k in ('ok','tests','error','sourcesUnchanged')}))
    return 0 if receipt['ok'] else 1

if __name__ == '__main__':
    sys.exit(main())
