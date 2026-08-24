#!/usr/bin/env python3
"""Mutation testing for EP3D: change one thing, and see whether anything notices.

WHY THIS IS IN THE REPO NOW (M26.1)
-----------------------------------
It was a throwaway script re-typed once per milestone, from M18 to M26. That
was tolerable while the oracle was right. It stopped being tolerable when the
oracle turned out to be wrong for the THIRD time in this project's life:

  * ADR-M11-013: `$LASTEXITCODE` did not survive a PowerShell assignment, so
    the harness read a green oracle from a failing test and reported three
    survivors that were not.
  * M22: `shutil.move` restored the backup's older mtime, so the build skipped
    the file and the NEXT measurement ran against the mutated object.
  * M26: a mutation that made a test CRASH printed no `[  FAILED  ]` line, so
    it was scored as a SURVIVOR. Two of them -- an assembly that contains
    itself, and a recursion chain that never grows -- both stack-overflow, and
    both looked like gaps in the tests when the tests were fine.

ADR-M11-013 already wrote the rule this violates: **a mutation harness with a
broken oracle is worse than no harness, because it reports confidence it has
not earned.** Three times is enough to stop re-typing it.

THE ORACLE, STATED
------------------
A mutation is KILLED when, after applying it:

  * the build fails; or
  * any suite prints a `[  FAILED  ]` line; or
  * any suite fails to REACH ITS SUMMARY -- it crashed, hung, or exited
    non-zero without printing `[  PASSED  ]`.

The third clause is the one that was missing. "Nothing said it failed" and "it
passed" are different sentences, and only one of them is evidence.

A mutation SURVIVES only when every suite ran to completion and every one of
them passed. A survivor is a test gap or an equivalent mutation, and the two
are told apart by reading, not by the harness.

USE
---
    python tools/mutate.py mutations.json

where the file is a list of {name, path, old, new} objects. `old` must appear
EXACTLY ONCE in `path`; anything else is reported rather than guessed at, and
the mutation is skipped.
"""

import io
import json
import os
import shutil
import subprocess
import sys

SUITES = [
    'ParametricCADCoreTests',
    'ParametricCADSolverTests',
    'ParametricCADIntegrationTests',
    'ParametricCADKernelOcctTests',
    'ParametricCADImportTests',
]

# THE SHELL, which is not a gtest suite and was therefore invisible here.
#
# The fourth time this project's mutation oracle has been wrong (see the header
# above for the first three). Every check the viewer's `--selftest` makes -- and
# it is the ONLY place that starts the program, paints a toolbar, opens a
# sketch or runs a script through the window -- was outside what this harness
# looked at. Mutating viewer code therefore scored SURVIVED no matter how
# thoroughly the self test caught it, which is confidence the harness had not
# earned, about precisely the code unit tests cannot reach.
#
# Its oracle is its own: it prints SELFTEST OK, not a gtest summary.
SHELL_CHECKS = [
    ('ParametricCADViewer', ['--selftest'], 'SELFTEST OK'),
    ('ParametricCADViewer', ['--selftest', '--sample=m12-sketch'], 'SELFTEST OK'),
]

# Long enough for the OCCT suite on a cold cache; short enough that a mutation
# which hangs is reported this decade. A timeout is a KILL, not an inconclusive
# result: a change that makes the tests stop finishing has been noticed.
SUITE_TIMEOUT_SECONDS = 900


def unlock(target):
    """Renames a RUNNING executable aside so the linker can write a new one.

    The owner usually has a viewer window open while this runs, and Windows
    refuses to overwrite a loaded image (LNK1168) -- which would score every
    mutation as 'did not compile', i.e. as a kill, for a reason that has
    nothing to do with the mutation. Renaming is allowed while it runs.
    """
    path = 'build/Debug/%s.exe' % target
    if not os.path.exists(path):
        return
    try:
        os.replace(path, path + '.busy')
    except OSError:
        pass


def build():
    """Returns None on success, or the reason it failed."""
    targets = SUITES + sorted({name for name, _, _ in SHELL_CHECKS})
    for name, _, _ in SHELL_CHECKS:
        unlock(name)
    result = subprocess.run(
        ['cmake', '--build', 'build', '--config', 'Debug', '--target'] + targets,
        capture_output=True)
    return None if result.returncode == 0 else 'did not compile'


def run_suites():
    """Every reason a suite gave to think the mutation was noticed."""
    reasons = []
    for suite in SUITES:
        path = './build/Debug/%s.exe' % suite
        try:
            result = subprocess.run([path], capture_output=True, text=True,
                                    errors='replace', timeout=SUITE_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            reasons.append('%s HUNG' % suite)
            continue
        except OSError as problem:
            reasons.append('%s could not be run (%s)' % (suite, problem))
            continue

        reasons += [line.strip() for line in result.stdout.splitlines()
                    if line.startswith('[  FAILED  ] ') and '(' in line]

        # THE CLAUSE THAT WAS MISSING. A suite that never reached its summary
        # did not pass, whatever it did or did not print on the way.
        if '[  PASSED  ]' not in result.stdout:
            reasons.append('%s never reached its summary (exit %d)'
                           % (suite, result.returncode))
        elif result.returncode != 0:
            reasons.append('%s exited %d' % (suite, result.returncode))

    # ...and the shell, with the oracle IT answers to.
    for target, arguments, wanted in SHELL_CHECKS:
        label = target + ' ' + ' '.join(arguments)
        try:
            result = subprocess.run(['./build/Debug/%s.exe' % target] + arguments,
                                    capture_output=True, text=True, errors='replace',
                                    timeout=SUITE_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            reasons.append('%s HUNG' % label)
            continue
        except OSError as problem:
            reasons.append('%s could not be run (%s)' % (label, problem))
            continue
        if wanted not in result.stdout and wanted not in result.stderr:
            first = ''
            for line in (result.stderr or result.stdout).splitlines():
                if 'FAIL' in line:
                    first = ' -- ' + line.strip()
                    break
            reasons.append('%s did not say %s (exit %d)%s'
                           % (label, wanted, result.returncode, first))
    return reasons


def main(argv):
    if len(argv) != 2:
        sys.stderr.write('usage: mutate.py <mutations.json>\n')
        return 2
    mutations = json.load(io.open(argv[1], encoding='utf-8'))

    survivors = []
    for mutation in mutations:
        name = mutation['name']
        path = mutation['path']
        old = mutation['old']
        new = mutation['new']

        source = io.open(path, encoding='utf-8').read()
        found = source.count(old)
        if found != 1:
            # NOT guessed at. A pattern that matches twice would mutate
            # whichever came first, and the result would be a measurement of
            # something nobody chose.
            print('%-62s -> PATTERN MATCHES %d TIMES, skipped' % (name, found))
            sys.stdout.flush()
            continue

        shutil.copyfile(path, path + '.bak')
        io.open(path, 'w', encoding='utf-8', newline='').write(source.replace(old, new, 1))
        try:
            failure = build()
            reasons = ['(%s -- also a kill)' % failure] if failure else run_suites()
        finally:
            shutil.move(path + '.bak', path)
            # The move restores the BACKUP's mtime, which is older than the
            # object file -- so the next build would skip this file and the next
            # measurement would run against the mutated code. Cost a whole
            # afternoon in M22.
            os.utime(path, None)

        print('%-62s -> %d red' % (name, len(reasons)))
        for reason in reasons[:2]:
            print('      ', reason)
        sys.stdout.flush()
        if not reasons:
            survivors.append(name)

    subprocess.run(['cmake', '--build', 'build', '--config', 'Debug'], capture_output=True)
    print('restored')
    if survivors:
        print('\n%d SURVIVED -- each is a test gap or an equivalent mutation, and telling '
              'them apart is a reading job:' % len(survivors))
        for name in survivors:
            print('  ', name)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
