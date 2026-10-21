"""Run the required parity gates with bounded child processes."""
import argparse
import collections
import json
import math
import os
import pathlib
import shlex
import signal
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
PARTITIONS = 16


def bounded(command, seconds=300, env=None):
    if not math.isfinite(seconds) or not 0 < seconds <= 300:
        raise ValueError('gate timeout must be positive and at most 300 seconds')
    started = time.monotonic()
    print('[gate] start: ' + shlex.join(map(str, command)), flush=True)
    process = subprocess.Popen(command, env=env, start_new_session=True)
    try:
        result = process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        print('[gate] timeout: child process group stopped', flush=True)
        result = 124
    print(f'[gate] end: exit={result} seconds={time.monotonic() - started:.3f}', flush=True)
    return result


def fixture_ids(root, generator):
    files = [str(root / directory / path.name)
             for directory in generator['dirs']
             for path in sorted((root / directory).glob('*.dsl'))]
    if not files or len(files) != len(set(files)):
        raise ValueError('fixture discovery must contain unique files')
    return files


def valid_receipt(path, expected):
    try:
        return json.loads(path.read_text()) == expected
    except (OSError, ValueError):
        return False


def gate_plan(root, temporary):
    build = os.environ.get('NM_BUILD_DIR', str(root / 'build'))
    commands = [
        ['tools/catalog/fetch-catalog.mjs', '--cache', '.cache/catalog', '--offline'],
        ['tools/catalog/check-catalog.mjs', build + '/catalog'],
        ['parity/check_func_bodies_differential.mjs'],
        ['parity/check_frontend.mjs', 'tokens', 'ast', 'validated', 'expanded', 'graph', 'graph-normalized'],
        ['parity/check_frontend.mjs', 'validated', 'expanded', 'graph', '--isolated'],
        *[[f'parity/check_{name}.mjs'] for name in (
            'wgsl', 'uniforms', 'backend', 'js_math', 'pipeline', 'registry',
            'midi_state', 'audio_state', 'obj_parser')],
    ]
    plan = [{'group': 'gate', 'command': ['node', *command]} for command in commands]
    for mode, key, checker in (
            ('dsl-tools', 'cases', 'check_dsl_tools.mjs'),
            ('program-state', 'scenarios', 'check_program_state.mjs')):
        for suite in sorted((root / 'parity' / mode).glob('*.json')):
            content = json.loads(suite.read_text())
            if suite.name != 'fixtures.json':
                plan.append({'group': mode, 'suite': suite, 'partition': None,
                             'command': ['node', 'parity/' + checker, str(suite)]})
                continue
            generators = content[key]
            if len(generators) != 1 or generators[0].get('generate') != 'fixtures':
                raise ValueError('fixture partition suite has an unexpected shape')
            files = fixture_ids(root, generators[0])
            groups = [files[index::PARTITIONS] for index in range(PARTITIONS)]
            if collections.Counter(item for group in groups for item in group) != collections.Counter(files):
                raise ValueError('fixture partitions do not cover the exact input set')
            for index, selected in enumerate(groups):
                directory = temporary / mode / str(index)
                directory.mkdir(parents=True)
                partitioned = json.loads(suite.read_text())
                partitioned[key][0]['partition'] = {'index': index, 'count': PARTITIONS}
                path = directory / suite.name
                path.write_text(json.dumps(partitioned))
                plan.append({'group': mode, 'suite': path, 'partition': index,
                             'fixture_ids': selected, 'receipt': directory / 'fixtures-read.json',
                             'command': ['node', 'parity/' + checker, str(path)]})
    return plan


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timeout-seconds', type=float, default=300)
    parser.add_argument('--run', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not math.isfinite(args.timeout_seconds) or not 0 < args.timeout_seconds <= 300:
        parser.error('timeout must be positive and at most 300 seconds')
    if args.run is not None:
        if not args.run:
            parser.error('--run requires a command')
        return bounded(args.run, args.timeout_seconds)
    with tempfile.TemporaryDirectory(prefix='nm-gates-') as directory:
        for row in gate_plan(ROOT, pathlib.Path(directory)):
            env = dict(os.environ)
            env.pop('NM_GATE_FIXTURE_REPORT', None)
            if row.get('partition') is not None:
                env['NM_GATE_FIXTURE_REPORT'] = str(row['receipt'])
            result = bounded(row['command'], args.timeout_seconds, env)
            if result:
                return result
            if row.get('partition') is not None:
                if not valid_receipt(row['receipt'], row['fixture_ids']):
                    print('[gate] failed: fixture coverage receipt does not match selected inputs', file=sys.stderr)
                    return 1
                print(f'[gate] fixture coverage: {row["group"]} partition={row["partition"]} '
                      f'files={len(row["fixture_ids"])}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
