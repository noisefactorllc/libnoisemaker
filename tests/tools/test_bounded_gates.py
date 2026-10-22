import collections
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = ROOT / 'tools' / 'run_gates.py'


class FixturePartitionTests(unittest.TestCase):
    def synthetic(self, mode):
        # Execute the real generator with synthetic files and a synthetic registry.
        script = r'''
import { readFileSync as read } from 'node:fs';
import vm from 'node:vm';
import { pathToFileURL } from 'node:url';
const root = process.argv[1], mode = process.argv[2];
let partitionFixtureFiles = files => files;
try { ({ partitionFixtureFiles } = await import(pathToFileURL(root + '/tools/fixture_partition.mjs').href)); }
catch (e) { if (e.code !== 'ERR_MODULE_NOT_FOUND') throw e; }
const file = mode === 'dsl' ? 'reference-dsl-tools.mjs' : 'reference-program-state.mjs';
const source = read(root + '/tools/' + file, 'utf8');
const start = source.indexOf(mode === 'dsl' ? 'function * generateFixtureCases' : 'function fixtureScenarios');
const end = source.indexOf(mode === 'dsl' ? '\nfunction formatValueCases' : '\n// Probe values', start);
const files = Array.from({length: 9}, (_, i) => ({dir: 'parity/programs', name: 'p' + i, path: 'p' + i}));
const sandbox = {partitionFixtureFiles, fixtureFiles: () => files,
  readFileSync: path => path.split('/').pop().replace('.dsl', ''),
  readdirSync: () => files.map(x => x.name + '.dsl'), join: (...parts) => parts.join('/'),
  ROOT: '', HOST_UNPARSE_OPTIONS: {}, generateUpdates: () => ({})};
vm.createContext(sandbox);
vm.runInContext(source.slice(start, end) + '\nthis.generate = ' +
  (mode === 'dsl' ? 'generateFixtureCases' : 'fixtureScenarios'), sandbox);
const resolver = {compile(src) { if (src === 'p1' || src === 'p4') throw Error('invalid'); return {plans: []}; }};
const mod = {unparse: () => 'p0', listSteps: () => [{stepIndex: 0, isStarterPosition: false}]};
function run(partition) {
  const gen = {dirs: ['parity/programs'], compatibleEvery: 2, ...(partition ? {partition} : {})};
  return mode === 'dsl' ? [...sandbox.generate(mod, gen, resolver)].map(x => x.id) : sandbox.generate(gen).map(x => x.name);
}
console.log(JSON.stringify({whole: run(), parts: Array.from({length: 3}, (_, index) => run({index, count: 3}))}));
'''
        result = subprocess.run(['node', '--input-type=module', '-e', script, str(ROOT), mode],
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(0, result.returncode, result.stderr)
        return json.loads(result.stdout)

    def test_dsl_partitions_preserve_every_case_and_global_compatible_sampling(self):
        result = self.synthetic('dsl')
        actual = [item for part in result['parts'] for item in part]
        self.assertEqual(collections.Counter(result['whole']), collections.Counter(actual))
        self.assertEqual(len(actual), len(set(actual)))
        self.assertEqual(['programs/p0#compatible', 'programs/p3#compatible',
                          'programs/p6#compatible', 'programs/p8#compatible'],
                         [item for item in result['whole'] if item.endswith('#compatible')])

    def test_program_state_partitions_preserve_every_scenario_once(self):
        result = self.synthetic('state')
        actual = [item for part in result['parts'] for item in part]
        self.assertEqual(collections.Counter(result['whole']), collections.Counter(actual))
        self.assertEqual(len(actual), len(set(actual)))


class BoundedGateTests(unittest.TestCase):
    def runner(self):
        self.assertTrue(RUNNER.is_file(), 'gate children have no bounded runner')
        spec = importlib.util.spec_from_file_location('run_gates', RUNNER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_retained_receipt_skips_a_child_and_invalidates_on_other_head(self):
        runner = self.runner()
        with tempfile.TemporaryDirectory() as directory:
            receipts = pathlib.Path(directory) / 'receipts'
            plan = [{'group': 'gate', 'command': [sys.executable, '-c', 'pass']}]
            import io
            import contextlib
            head_a, head_b = 'a' * 40, 'b' * 40
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                self.assertEqual(0, runner.run_plan(plan, 1, 60, receipts, head_a))
            self.assertIn('[gate] start:', buffer.getvalue())
            self.assertEqual(1, len(list(receipts.glob('*.json'))))
            receipt = json.loads(next(receipts.glob('*.json')).read_text())
            self.assertEqual({'head': head_a, 'command': [sys.executable, '-c', 'pass'], 'exit': 0}, receipt)
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                self.assertEqual(0, runner.run_plan(plan, 1, 60, receipts, head_a))
            self.assertIn('[gate] skip (retained receipt):', buffer.getvalue())
            self.assertNotIn('[gate] start:', buffer.getvalue())
            buffer = io.StringIO()
            with contextlib.redirect_stdout(buffer):
                self.assertEqual(0, runner.run_plan(plan, 1, 60, receipts, head_b))
            # A different head invalidates the receipt and runs the child again.
            self.assertIn('[gate] start:', buffer.getvalue())
            self.assertEqual(2, len(list(receipts.glob('*.json'))))

    def test_start_line_echoes_a_bounded_prefix_of_giant_commands(self):
        runner = self.runner()
        import io
        import contextlib
        long_word = 'x' * 5000
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            self.assertEqual(0, runner.bounded([sys.executable, '-c', 'pass', long_word], 60))
        line = [l for l in buffer.getvalue().splitlines() if l.startswith('[gate] start:')][0]
        self.assertLess(len(line), 700)
        self.assertIn('truncated', line)
        self.assertIn('pass', line)
        self.assertNotIn(long_word, line)

    def interpreter_startup(self):
        started = time.monotonic()
        subprocess.run([sys.executable, '-c', 'import subprocess,pathlib'], timeout=60)
        return time.monotonic() - started

    def test_timeout_stops_only_its_process_group_without_retry(self):
        runner = self.runner()
        with tempfile.TemporaryDirectory() as directory:
            marker = pathlib.Path(directory) / 'late'
            attempts = pathlib.Path(directory) / 'attempts'
            budget = max(.15, self.interpreter_startup() * 3)
            descendant = f'import time,pathlib; time.sleep({budget * 1.5 + .2!r}); pathlib.Path({str(marker)!r}).touch()'
            command = [sys.executable, '-c',
                       'import subprocess,sys,time,pathlib; '
                       f'pathlib.Path({str(attempts)!r}).write_text("once"); '
                       f'subprocess.Popen([sys.executable,"-c",{descendant!r}]); time.sleep(2)']
            # The child writes `attempts` in its first statement, so the kill
            # must land after interpreter startup; the descendant's sleep
            # exceeds the budget, so it must not reach its marker before the
            # kill.
            self.assertEqual(124, runner.bounded(command, budget))
            time.sleep(budget * 1.5 + .2)
            self.assertFalse(marker.exists())
            self.assertEqual('once', attempts.read_text())
            self.assertEqual(7, runner.bounded([sys.executable, '-c', 'raise SystemExit(7)'], 1))

    def test_windows_timeout_terminates_the_exact_child_tree_without_retry(self):
        from types import SimpleNamespace
        from unittest.mock import Mock
        runner = self.runner()
        real = runner.subprocess
        process = SimpleNamespace(pid=12345, wait=Mock(side_effect=[
            real.TimeoutExpired(['synthetic'], .1), -1]))
        launch, stop = Mock(return_value=process), Mock()
        runner.os = SimpleNamespace(name='nt')
        runner.subprocess = SimpleNamespace(Popen=launch, run=stop,
            TimeoutExpired=real.TimeoutExpired, DEVNULL=real.DEVNULL,
            CREATE_NEW_PROCESS_GROUP=512)
        self.assertEqual(124, runner.bounded(['synthetic'], .1))
        launch.assert_called_once_with(['synthetic'], env=None,
                                      start_new_session=False, creationflags=512)
        stop.assert_called_once_with(['taskkill', '/PID', '12345', '/T', '/F'],
            check=True, stdout=real.DEVNULL, stderr=real.DEVNULL, timeout=5)
        self.assertEqual(2, process.wait.call_count)

    def test_plan_covers_original_gates_and_fixture_partitions_exactly_once(self):
        runner = self.runner()
        with tempfile.TemporaryDirectory() as directory:
            plan = runner.gate_plan(ROOT, pathlib.Path(directory))
            for mode, key in [('dsl-tools', 'cases'), ('program-state', 'scenarios')]:
                rows = [row for row in plan if row['group'] == mode]
                expected = sorted((ROOT / 'parity' / mode).glob('*.json'))
                ordinary = [row['suite'] for row in rows if row['partition'] is None]
                self.assertEqual([p for p in expected if p.name != 'fixtures.json'], ordinary)
                parts = [row for row in rows if row['partition'] is not None]
                self.assertEqual(list(range(16)), [row['partition'] for row in parts])
                discovered = runner.fixture_ids(ROOT, json.loads((ROOT / 'parity' / mode / 'fixtures.json').read_text())[key][0])
                self.assertEqual(collections.Counter(discovered), collections.Counter(
                    item for row in parts for item in row['fixture_ids']))
                for row in parts:
                    self.assertEqual('fixtures.json', row['suite'].name)
                    self.assertEqual({'index': row['partition'], 'count': 16},
                                     json.loads(row['suite'].read_text())[key][0]['partition'])
            ordinary_commands = [row['command'][1] for row in plan if row['group'] == 'gate']
            self.assertEqual(
                ['tools/catalog/fetch-catalog.mjs', 'tools/catalog/check-catalog.mjs',
                 'tools/generate-kit-compat.mjs', 'parity/check_func_bodies_differential.mjs']
                # One frontend child per stage: the stages together exceed
                # the 300 s bound, and every stage covers the same programs.
                + ['parity/check_frontend.mjs'] * 12
                + ['parity/check_wgsl.mjs', 'parity/check_uniforms.mjs',
                   'parity/check_backend.mjs', 'parity/check_js_math.mjs', 'parity/check_pipeline.mjs',
                   'parity/check_registry.mjs', 'parity/check_midi_state.mjs',
                   'parity/check_audio_state.mjs', 'parity/check_obj_parser.mjs'],
                ordinary_commands)

    def test_test_entrypoint_bounds_unit_suites_without_running_real_tests(self):
        import os
        import shutil
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name in ('scripts', 'tools', 'tests/tools', 'bin'):
                (root / name).mkdir(parents=True)
            shutil.copyfile(ROOT / 'scripts/test', root / 'scripts/test')
            if RUNNER.exists():
                shutil.copyfile(RUNNER, root / 'tools/run_gates.py')
            (root / 'scripts/gates').write_text('#!/usr/bin/env bash\nexit 0\n')
            (root / 'scripts/gates').chmod(0o700)
            (root / 'tests/__init__.py').touch()
            (root / 'tests/tools/__init__.py').touch()
            (root / 'tests/tools/test_smoke.py').write_text('import unittest\nclass Smoke(unittest.TestCase):\n def test_ok(self): pass\n')
            (root / 'build-test').mkdir()
            for name in ('cmake', 'ninja'):
                path = root / 'bin' / name
                path.write_text('#!' + pathlib.Path(sys.executable).as_posix() + '\nimport pathlib,sys\n'
                                f'pathlib.Path({str(root / "commands")!r}).open("a").write(repr(sys.argv)+"\\n")\n')
                path.chmod(0o700)
            env = dict(os.environ, NM_PYTHON=pathlib.Path(sys.executable).as_posix(),
                       PATH=str(root / 'bin') + os.pathsep + os.environ['PATH'])
            env.pop('NM_CHANGED_FILES', None)
            bash = 'bash'
            if os.name == 'nt':
                # Windows' System32 bash is a WSL launcher, not the Git shell.
                git = pathlib.Path(shutil.which('git'))
                bash = str(git.parent.parent / 'bin/bash.exe')
                self.assertTrue(pathlib.Path(bash).is_file())
            result = subprocess.run([bash, (root / 'scripts/test').as_posix()], env=env,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('--output-on-failure --timeout 300', result.stdout)
            self.assertIn('[gate] start: ctest', result.stdout)
            self.assertIn('-m unittest discover', result.stdout)
            commands = root / 'commands'
            # With NM_CHANGED_FILES the entrypoint runs only the tests that
            # exercise the changed paths.
            commands.write_text('')
            focused = dict(env, NM_CHANGED_FILES='tools/run_gates.py\n')
            result = subprocess.run([bash, (root / 'scripts/test').as_posix()], env=focused,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('-m unittest discover', result.stdout)
            self.assertNotIn('ctest', result.stdout)
            self.assertEqual('', commands.read_text())
            # A path the gates consume selects the build, ctest and the gates.
            commands.write_text('')
            gate_tool = dict(env, NM_CHANGED_FILES='tools/reference-oracle.mjs\n')
            result = subprocess.run([bash, (root / 'scripts/test').as_posix()], env=gate_tool,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('[gate] start: ctest', result.stdout)
            self.assertIn('cmake', commands.read_text())
            core = dict(env, NM_CHANGED_FILES='src/core/value.cpp\nparity/x.json\n')
            result = subprocess.run([bash, (root / 'scripts/test').as_posix()], env=core,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('[gate] start: ctest', result.stdout)
            # A gate run needs the native build, even without core sources.
            self.assertIn('cmake', commands.read_text())
            # An unmatched path does not narrow the run.
            commands.write_text('')
            unmatched = dict(env, NM_CHANGED_FILES='README.md\n')
            result = subprocess.run([bash, (root / 'scripts/test').as_posix()], env=unmatched,
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertIn('[gate] start: ctest', result.stdout)
            self.assertIn('-m unittest discover', result.stdout)
            self.assertIn('cmake', commands.read_text())

    def test_missing_duplicate_or_wrong_partition_receipt_cannot_pass(self):
        runner = self.runner()
        with tempfile.TemporaryDirectory() as directory:
            receipt = pathlib.Path(directory) / 'receipt.json'
            for ids in (None, ['a'], ['a', 'a'], ['b', 'a'], ['a', 'b', 'c']):
                with self.subTest(ids=ids):
                    if ids is not None:
                        receipt.write_text(json.dumps(ids))
                    self.assertFalse(runner.valid_receipt(receipt, ['a', 'b']))
            receipt.write_text('["a", "b"]')
            self.assertTrue(runner.valid_receipt(receipt, ['a', 'b']))


if __name__ == '__main__':
    unittest.main()
