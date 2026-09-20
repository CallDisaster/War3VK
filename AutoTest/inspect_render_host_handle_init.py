"""Read-only self-handle probe of an explicitly supplied owned CPU lab executable.

Not an acceptance gate; failed lifetimes remain failed. Never opens game memory.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

from run_render_host_protocol_gate import ROOT, identity


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--client', type=Path, required=True)
    p.add_argument('--host', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--probe', action='store_true')
    a = p.parse_args()
    directory = a.output.resolve(); directory.mkdir(exist_ok=False)
    client = a.client.resolve(strict=True); host = a.host.resolve(strict=True)
    report = dict(diagnosticOnly=True, accepted=False, client=identity(client), host=identity(host), runs=[])
    for i in range(4):
        case = directory / str(i); case.mkdir()
        env = os.environ.copy()
        env.pop('WARVK_HOST_LAB_HANDLE_PROBE', None)
        if a.probe:
            env['WARVK_HOST_LAB_HANDLE_PROBE'] = '1'
        r = subprocess.run([str(client), str(host), 'p3-to-p2', str(case / 'host.ndjson')], cwd=ROOT,
                           env=env, capture_output=True, text=True, timeout=23,
                           creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
        (case / 'stdout.txt').write_text(r.stdout, encoding='utf-8')
        (case / 'stderr.txt').write_text(r.stderr, encoding='utf-8')
        blocks = re.findall(r'SELF_HANDLES (\S+) count=(\d+)\n([^\n]*)', r.stderr)
        snapshots = [(phase, int(count), dict(re.findall(r'(\d+):([A-Za-z0-9 ]+?)(?= \d+:| $|$)', line)))
                     for phase, count, line in blocks]
        record = dict(index=i, exit=r.returncode, snapshots=snapshots)
        if len(snapshots) == 2:
            before, after = snapshots[0][2], snapshots[1][2]
            record['added'] = {h: t for h, t in after.items() if before.get(h) != t}
            record['removed'] = {h: t for h, t in before.items() if after.get(h) != t}
        report['runs'].append(record)
        print(json.dumps({k: v for k, v in record.items() if k != 'snapshots'}), flush=True)
    with (directory / 'diagnostic.json').open('x', encoding='utf-8') as f:
        json.dump(report, f, indent=2); f.write('\n')


if __name__ == '__main__':
    main()
