"""Run the required parity gates with bounded child processes."""
import argparse
import collections
import hashlib
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
    words = [str(word) for word in command]
    line = shlex.join(words)
    # Giant echoed commands can wedge the runner's output capture when the
    # stream stalls; keep the echoed start line short.
    if len(line) > 400:
        shown = 0
        kept = []
        for word in words:
            width = len(shlex.quote(word)) + 1
            if shown + width > 400:
                break
            kept.append(word)
            shown += width
        line = shlex.join(kept) + f' ... [truncated, {len(words)} args, {len(line)} chars]'
    print('[gate] start: ' + line, flush=True)
    windows = os.name == 'nt'
    process = subprocess.Popen(command, env=env, start_new_session=not windows,
                               creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if windows else 0)
    try:
        result = process.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        if windows:
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        else:
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


def head_id():
    result = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=str(ROOT),
                            capture_output=True, text=True, timeout=30)
    head = result.stdout.strip()
    return head if result.returncode == 0 and head else ''


def receipt_key(row, head):
    if row.get('partition') is not None:
        # Partition commands carry a per-run temporary path; key on the stable
        # inputs instead: the mode, the suite, the partition and its inputs.
        inputs = hashlib.sha256('\n'.join(row['fixture_ids']).encode()).hexdigest()
        stable = f'{head}|{row["group"]}|{row["suite"].name}|{row["partition"]}|{inputs}'
    else:
        stable = f'{head}|{shlex.join(map(str, row["command"]))}'
    return stable


def receipt_path(directory, row, head):
    return directory / (hashlib.sha256(receipt_key(row, head).encode()).hexdigest() + '.json')


def receipt_body(row, head):
    if row.get('partition') is not None:
        # Partition commands carry a per-run temporary path; store only the
        # stable identity so a later run can recognize the retained receipt.
        return {'head': head, 'group': row['group'], 'suite': row['suite'].name,
                'partition': row['partition'], 'fixture_ids': row['fixture_ids'], 'exit': 0}
    return {'head': head, 'command': [str(part) for part in row['command']], 'exit': 0}


def program_shard(root, index, count):
    """Deterministic disjoint program shard; the union covers every program."""
    files = []
    for directory in ('programs', 'corpus', 'coverage', 'timed', 'curated'):
        d = root / 'parity' / directory
        files += sorted(str(p) for p in d.glob('*.dsl'))
    if not files:
        raise ValueError('program discovery must find files')
    return files[index::count]


def gate_plan(root, temporary):
    build = os.environ.get('NM_BUILD_DIR', str(root / 'build'))
    commands = [
        ['tools/catalog/fetch-catalog.mjs', '--cache', '.cache/catalog', '--offline'],
        ['tools/catalog/check-catalog.mjs', build + '/catalog'],
        ['parity/check_func_bodies_differential.mjs'],
        # One child per stage: the six stages together exceed the 300 s
        # bound, and every stage covers the same full program set.
        *[['parity/check_frontend.mjs', stage]
          for stage in ('tokens', 'ast', 'validated', 'expanded', 'graph', 'graph-normalized')],
        # The --isolated stages are the slowest; two disjoint program shards
        # each keep every child inside the bound with the same total coverage.
        *[[f'parity/check_frontend.mjs', stage, '--isolated', '--programs',
           *program_shard(root, index, 2)]
          for stage in ('validated', 'expanded', 'graph') for index in (0, 1)],
        *[[f'parity/check_{name}.mjs'] for name in (
            'wgsl', 'uniforms', 'backend', 'js_math', 'pipeline', 'registry',
            'midi_state', 'audio_state', 'obj_parser')],
    ]
    plan = [{'group': 'gate', 'command': ['node', *command]} for command in commands]
    # The catalog fetch populates the offline cache; keep it ahead of the pool.
    plan[0]['serial'] = True
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


def gate_row(row, seconds, receipts, head):
    """Run one plan row; a passing fixture partition must cover its inputs.

    A retained receipt records a passing child bound to this exact head and
    inputs; the child is skipped while that receipt stays valid."""
    path = receipt_path(receipts, row, head) if head else None
    expected = receipt_body(row, head)
    if path is not None and valid_receipt(path, expected):
        print('[gate] skip (retained receipt): ' + shlex.join(map(str, row['command'])), flush=True)
        return 0
    env = dict(os.environ)
    env.pop('NM_GATE_FIXTURE_REPORT', None)
    if row.get('partition') is not None:
        env['NM_GATE_FIXTURE_REPORT'] = str(row['receipt'])
    result = bounded(row['command'], seconds, env)
    if result:
        return result
    if row.get('partition') is not None:
        if not valid_receipt(row['receipt'], row['fixture_ids']):
            print('[gate] failed: fixture coverage receipt does not match selected inputs', file=sys.stderr)
            return 1
        print(f'[gate] fixture coverage: {row["group"]} partition={row["partition"]} '
              f'files={len(row["fixture_ids"])}', flush=True)
    if path is not None:
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(expected))
        except OSError as error:
            print(f'[gate] receipt not retained: {error}', file=sys.stderr)
    return 0


def run_plan(plan, jobs, seconds, receipts, head):
    """Run the plan with a bounded worker pool so the whole coverage fits the
    bounded execution budget. Every child keeps its own per-gate timeout."""
    import concurrent.futures
    if jobs < 1:
        raise ValueError('gate worker count must be positive')
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = []
        for row in plan:
            if row.get('serial'):
                # Serial rows run before the pool so the pool sees their cache.
                result = gate_row(row, seconds, receipts, head)
                if result:
                    return result
                continue
            futures.append(pool.submit(gate_row, row, seconds, receipts, head))
        failure = 0
        for future in futures:
            try:
                result = future.result()
            except concurrent.futures.CancelledError:
                continue
            if result and not failure:
                failure = result
                for pending in futures:
                    pending.cancel()
        return failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timeout-seconds', type=float, default=300)
    parser.add_argument('--jobs', type=int, default=int(os.environ.get('NM_GATE_JOBS', '0')) or
                        min(8, os.cpu_count() or 2))
    parser.add_argument('--receipts', type=str,
                        default='' if os.environ.get('NM_GATE_FRESH') else str(ROOT / '.cache' / 'gates'))
    parser.add_argument('--run', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not math.isfinite(args.timeout_seconds) or not 0 < args.timeout_seconds <= 300:
        parser.error('timeout must be positive and at most 300 seconds')
    if args.run is not None:
        if not args.run:
            parser.error('--run requires a command')
        return bounded(args.run, args.timeout_seconds)
    head = head_id() if args.receipts else ''
    receipts = pathlib.Path(args.receipts) if args.receipts else None
    with tempfile.TemporaryDirectory(prefix='nm-gates-') as directory:
        return run_plan(gate_plan(ROOT, pathlib.Path(directory)), args.jobs, args.timeout_seconds,
                        receipts or pathlib.Path(directory), head)


if __name__ == '__main__':
    raise SystemExit(main())
