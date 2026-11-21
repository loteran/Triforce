"""Fault-inject the actual pipeline_cache_save using real C++ containers/threads and authored file bytes.

No Vulkan loader, GPU, game or cache input. Each case is a fresh, bounded child, including detached-worker faults.
Optional --baseline-source verifies the unmodified helper's uncaught worker/caller exceptions too.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]
COMPILER = None
BASELINE = None


def save_function(path):
    source = path.read_text(encoding='utf-8')
    marker = 'void pipeline_cache_save(bool force) {'
    if source.count(marker) != 1:
        raise AssertionError('expected exactly one production cache-save function')
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def checkpoint_function(path):
    """(Triforce) vk_draw.cpp's pcache_checkpoint_save, as it is (gpu/vk/warm_checkpoint.h): the preparation's checkpoint save
    through pipeline_cache_save, always the source under test's own"""
    source = path.read_text(encoding='utf-8')
    marker = 'size_t pcache_checkpoint_save() {'
    if source.count(marker) != 1:
        raise AssertionError('expected exactly one checkpoint save function')
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def carry_part(path):
    """(Triforce) vk_draw.cpp's carry part (pcache_carry_past, pcache_carry_save_ok, pcache_carry_check, pcache_carry_held,
    pcache_carry_early, pcache_carry_saved),
    as it is: always the source under test's own, also for the optional original's save function"""
    source = path.read_text(encoding='utf-8')
    start_mark = "// ---- (Triforce) The carry's state and its rules in use"
    end_mark = "// ---- (Triforce) The carry's part ends ----"
    if source.count(start_mark) != 1 or source.count(end_mark) != 1:
        raise AssertionError('expected exactly one carry part')
    return source[source.index(start_mark):source.index(end_mark)]


class PipelineCacheSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = COMPILER or shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise AssertionError('a C++20 compiler is required')
        cls.temporary = tempfile.TemporaryDirectory(prefix='hookshot-pipeline-save-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name).resolve()
        if not cls.directory.is_relative_to(Path(tempfile.gettempdir()).resolve()):
            raise AssertionError('fixture must stay in its owned temporary directory')
        cls.executables = {}
        for name, source in [('fixed', ROOT / 'gpu/vk/vk_draw.cpp'), *([('original', BASELINE)] if BASELINE else [])]:
            directory = cls.directory / name
            directory.mkdir()
            (directory / 'pipeline_cache_save.inc').write_text(save_function(source), encoding='utf-8')
            (directory / 'pcache_carry_part.inc').write_text(carry_part(ROOT / 'gpu/vk/vk_draw.cpp'), encoding='utf-8')
            (directory / 'pcache_checkpoint_save.inc').write_text(checkpoint_function(ROOT / 'gpu/vk/vk_draw.cpp'),
                                                                  encoding='utf-8')
            executable = directory / 'check.exe'
            command = [compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
                       '-I' + str(directory), '-I' + str(ROOT / 'gpu/vk'), str(ROOT / 'tests/pipeline_cache_save_test.cpp'),
                       '-o', str(executable)]
            if os.name == 'nt':
                command += ['-D_CRT_SECURE_NO_WARNINGS']  # Match the production Windows CRT contract.
            options = {'creationflags': subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == 'nt' else {}
            result = subprocess.run(command, capture_output=True, text=True, timeout=60, **options)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[name] = executable

    def run_case(self, case, original=False):
        directory = self.directory / ('data-' + case + ('-original' if original else ''))
        directory.mkdir()
        return subprocess.run([str(self.executables['original' if original else 'fixed']), case, str(directory)],
                              capture_output=True, text=True, timeout=10)

    def test_success_threshold_and_forced_completion(self):
        self.check_case('success')
        if BASELINE:
            result = self.run_case('success', original=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_forced_vector_allocation_failure(self):
        self.check_case('force-vector')

    def test_detached_vector_allocation_failure(self):
        self.check_case('async-vector')

    def test_oversize_vector_length_error(self):
        self.check_case('force-length')

    def test_forced_temporary_path_allocation_failure(self):
        self.check_case('force-string')

    def test_detached_temporary_path_allocation_failure(self):
        self.check_case('async-string')

    def test_actual_thread_state_allocation_failure(self):
        self.check_case('thread-start')

    def test_vulkan_empty_and_error_returns(self):
        self.check_case('api-results')

    def test_failed_write_preserves_existing_file(self):
        self.check_case('short-write')

    def test_forced_wait_preserves_existing_single_waiter_behavior(self):
        self.check_case('forced-wait')

    def test_unchanged_data_is_not_written_again(self):
        # The precompile's pipelines, all found in the cache, count towards a save: data of the size read or last written
        # is not fetched nor written again; a write that did not land is tried again.
        self.check_case('unchanged')
        if BASELINE:
            result = self.run_case('unchanged', original=True)
            self.assertNotEqual(result.returncode, 0, 'the original rewrites unchanged data')
            self.assertIn('FAIL line', result.stderr)

    def test_data_over_the_bound_is_not_written(self):
        # gpu/vk/pcache_bound.h: the file keeps its last save, which the next start reads.
        self.check_case('over-bound')

    def test_carried_cache_is_not_written_past_its_cap(self):
        # (Triforce) gpu/vk/pcache_carry.h: carried over a stamp change, up to 1.25 x its base; a fresh cache after.
        self.check_case('carry-cap')

    def test_carried_cache_over_the_bound_is_past_what_it_may_hold(self):
        # (Triforce) Over the bound and under the cap: a rebuild is asked, not the bound's silent refusal with the old file.
        self.check_case('carry-bound')

    def test_carried_history_is_open_until_the_preparation_is_done(self):
        # (Triforce) pcache_carry.h Side::open: a stopped carried preparation's save stays capped when begun again.
        self.check_case('carry-done')

    def test_checkpoint_keeps_a_carried_preparation_capped(self):
        # (Triforce) gpu/vk/warm_checkpoint.h: a checkpoint mid-warm in a carried session saves under the new stamp with its
        # history open, as a stopped one's save, so the start after a crash there is carried still and capped; the RAM, the
        # cap and nothing new each leave the file as it was.
        self.check_case('checkpoint')

    def test_checkpoint_fetches_a_growing_cache_with_room(self):
        # (Triforce) gpu/vk/warm_checkpoint.h: the workers build while a checkpoint fetches; fetched with room, the cap and the
        # bound looked at again on what was fetched, asked again past the room, flushed and put in place written through.
        self.check_case('checkpoint-grow')

    def test_checkpoint_gives_up_on_a_stop_and_a_short_disk(self):
        # (Triforce) a stop asked before the fetch or in the write, the disk short of room: nothing written, no .tmp left.
        self.check_case('checkpoint-stop')

    def test_carried_history_is_written_before_the_data(self):
        # (Triforce) gpu/vk/pcache_carry.h begun: a carried save cut before its .for is read as that session's, capped.
        self.check_case('carry-begun')

    def test_own_marker_names_a_file_of_the_part_alone(self):
        # (Triforce) <file>.own: a checkpoint's file before a crash is not taken for an older build's (warm_libraries).
        self.check_case('own')

    def test_carried_cache_size_is_looked_at_while_preparing(self):
        # (Triforce) pcache_carry_check: only a carried cache is asked; not on Intel while workers build; once past, it stays.
        self.check_case('carry-check')

    def test_carried_cache_is_dropped_early_on_its_projection(self):
        # (Triforce) pcache_carry_early: a carried cache that would grow past what it may hold by the margin is dropped as past
        # it once its sample is built, and not saved; a sample too small, under the margin, past it already, on Intel or
        # switched off it is not (and on Intel or switched off no sample is taken: the warm keeps its own order).
        self.check_case('carry-early')

    def test_save_in_play_waits_while_the_copy_does_not_fit_in_ram(self):
        # (Triforce) gpu/vk/mem_parts.h copy_fits: the whole cache fetched in play is what Windows pages out with the RAM
        # short; the save waits for the next check, and the one at the game's close does not ask.
        self.check_case('ram-short')

    def test_cache_let_go_is_not_saved(self):
        # (Triforce) gpu/vk/pcache_gate.h: let go for the RAM, the driver is asked nothing and the file keeps its last save.
        self.check_case('let-go')

    def check_case(self, case):
        result = self.run_case(case)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_original_uncaught_exceptions(self):
        if not BASELINE:
            self.skipTest('optional original-source negative')
        direct = subprocess.run([str(self.executables['fixed']), 'terminate-control'], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(direct.returncode, 86, direct.stdout + direct.stderr)
        self.assertIn('uncaught worker exception', direct.stderr)
        control = subprocess.run([str(self.executables['fixed']), 'uncaught-thread-control'], capture_output=True,
                                 text=True, timeout=10)
        # This MSVC-target Clang/STL combination fail-fasts before the custom handler. Accept only that exact
        # independently reproduced runtime result, never an arbitrary crash, loader error or nonzero exit.
        if os.name == 'nt' and control.returncode == 0xC0000409:
            self.assertEqual(control.stderr, 'MSVC-target uncaught-thread control\n')
            worker_code, worker_reason = 0xC0000409, ''
        else:
            self.assertEqual(control.returncode, 86, control.stdout + control.stderr)
            self.assertIn('uncaught worker exception', control.stderr)
            worker_code, worker_reason = 86, 'uncaught worker exception'
        for case, code, reason in [('async-vector', worker_code, worker_reason),
                                   ('async-string', worker_code, worker_reason),
                                   ('force-vector', 87, 'uncaught caller exception'),
                                   ('force-length', 87, 'uncaught caller exception'),
                                   ('force-string', 87, 'uncaught caller exception'),
                                   ('thread-start', 87, 'uncaught caller exception')]:
            with self.subTest(case=case):
                result = self.run_case(case, original=True)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                if reason:
                    self.assertIn(reason, result.stderr)
                else:
                    self.assertEqual(result.stderr, '')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx')
    parser.add_argument('--baseline-source', type=Path)
    args, remaining = parser.parse_known_args()
    COMPILER, BASELINE = args.cxx, args.baseline_source
    unittest.main(argv=[__file__, *remaining])
