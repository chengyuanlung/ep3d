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
  * M47: `unlock` renamed the VIEWER aside so the linker could overwrite it,
    but not the five gtest suites. A suite executable that Windows still had
    open -- the run that had just finished, or a scanner reading the freshly
    written image -- failed to link, and the harness scored "did not compile",
    which its oracle counts as a KILL. A run of twenty-nine mutations came back
    twenty-nine kills, none of which had been measured at all. The tell was
    that EVERY line said it, and "everything was caught" is the one result a
    mutation run should never be believed about without looking.
  * M34: a mutation whose failure text carried a byte the console codepage
    (cp950 on this machine) cannot encode killed the HARNESS, on `print`, two
    thirds of the way through a run. Every mutation after it went unmeasured
    and no summary printed at all -- so a run that HAD found four real gaps
    reported none of them. A harness that dies mid-run reports confidence it
    has not earned in exactly the way ADR-M11-013 names.

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


def say(text):
    """Print, whatever the console's codepage thinks of the bytes.

    A test's failure message is arbitrary text: it can carry a diameter sign, a
    replacement character from a decode further up, or anything else somebody
    typed. On a cp950 console `print` raises on those and the traceback ends
    the run -- see M34 above. The report is worth more than the exact glyph, so
    what the console cannot encode is replaced rather than thrown.
    """
    try:
        print(text)
    except UnicodeEncodeError:
        encoding = getattr(sys.stdout, 'encoding', None) or 'ascii'
        print(text.encode(encoding, 'replace').decode(encoding, 'replace'))
    sys.stdout.flush()


# Bumped for every rename so two aside-files never share a name (see unlock).
_aside = [0]


def unlock(target):
    """Renames a RUNNING executable aside so the linker can write a new one.

    The owner usually has a viewer window open while this runs, and Windows
    refuses to overwrite a loaded image (LNK1168) -- which would score every
    mutation as 'did not compile', i.e. as a kill, for a reason that has
    nothing to do with the mutation. Renaming is allowed while it runs.

    A UNIQUE NAME EVERY TIME (M47). This used one fixed name, `.busy`, and put
    every rename there. The second rename in a run therefore had to overwrite
    the first -- and the first is, by construction, the file Windows would not
    let go of. `os.replace` raised, the `except` swallowed it, the executable
    stayed exactly where it was, and the link failed anyway. Which the oracle
    reads as a kill.

    That is how a run comes back with every single mutation killed and nothing
    measured: not one bug, but a lock the unlocker could not break because it
    was aiming at a locked file.
    """
    path = 'build/Debug/%s.exe' % target
    if not os.path.exists(path):
        return
    _aside[0] += 1
    try:
        os.rename(path, '%s.aside-%d' % (path, _aside[0]))
    except OSError as problem:
        # SAID OUT LOUD. Silence here is what let the fixed name hide for six
        # milestones: the build then fails for a reason the report calls a
        # kill, and the run looks like good news.
        say('could not move %s aside (%s) -- the next link will probably fail'
            % (path, problem))


def sweep_aside():
    """Deletes the renamed executables that are no longer held."""
    folder = 'build/Debug'
    if not os.path.isdir(folder):
        return
    for entry in os.listdir(folder):
        if '.exe.aside-' not in entry and not entry.endswith('.exe.busy'):
            continue
        try:
            os.remove(os.path.join(folder, entry))
        except OSError:
            pass   # still held; the next run will get it


def build():
    """Returns None on success, or the reason it failed."""
    targets = SUITES + sorted({name for name, _, _ in SHELL_CHECKS})
    # EVERY TARGET, not just the shell (M47). A suite whose executable is still
    # open links as LNK1168, and this harness reads a failed link as a kill --
    # so one locked file turns a whole run green without measuring anything.
    for name in targets:
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

    # A RUN THAT WAS KILLED LEAVES THE TREE MUTATED.
    #
    # `finally` restores the file when a mutation finishes or throws. It does
    # NOT run when the process is killed from outside -- a timeout, a Ctrl-C
    # that lands in the wrong place -- and what is left behind is a source file
    # with a deliberate defect in it and a .bak beside it.
    #
    # That happened: a run killed at a timeout left Hatch.cpp with its "a loop
    # of three points encloses an area" test reading two, and the NEXT full run
    # measured twenty-eight mutations against it. Nothing went red, because
    # another check happened to cover the same case -- so the damage was
    # invisible and every number in that report was about the wrong build.
    #
    # This is ADR-M11-013 again, for the fifth time: a mutation harness with a
    # broken oracle is worse than no harness. So the first thing a run does is
    # put back anything the last one dropped.
    stray = 0
    for mutation in mutations:
        backup = mutation['path'] + '.bak'
        if os.path.exists(backup):
            shutil.move(backup, mutation['path'])
            os.utime(mutation['path'], None)
            say('PUT BACK %s -- a previous run was killed before it could' % mutation['path'])
            stray += 1
    if stray:
        say('%d file(s) were left mutated by an interrupted run and have been restored.\n'
            'Rebuilding before measuring anything.' % stray)
        subprocess.run(['cmake', '--build', 'build', '--config', 'Debug'], capture_output=True)

    # THE BASELINE HAS TO BUILD, and it is checked before anything is changed.
    #
    # Every clause of this harness's oracle -- a red test, a crash, a failed
    # build -- reads as "the mutation was noticed". None of them can tell that
    # apart from "the tree was already broken", and a tree that does not build
    # scores a perfect run: every mutation killed, nothing measured. That is
    # ADR-M11-013's failure exactly, and it is the one that arrives looking
    # like good news.
    sweep_aside()
    say('Building the tree as it stands, before changing anything.')
    problem = build()
    if problem is not None:
        say('THE BASELINE DID NOT BUILD, so nothing below would have been measured.\n'
            'Every mutation would have scored as killed for a reason that has nothing\n'
            'to do with the mutation. Fix the build and run again.')
        return 2
    say('The baseline builds. Measuring %d mutation(s).\n' % len(mutations))

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
            say('%-62s -> PATTERN MATCHES %d TIMES, skipped' % (name, found))
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

        say('%-62s -> %d red' % (name, len(reasons)))
        for reason in reasons[:2]:
            say('       ' + reason)
        if not reasons:
            survivors.append(name)

    # ...and the same sweep on the way out, so a run that threw somewhere
    # unexpected still hands back a clean tree.
    for mutation in mutations:
        backup = mutation['path'] + '.bak'
        if os.path.exists(backup):
            shutil.move(backup, mutation['path'])
            os.utime(mutation['path'], None)
            say('PUT BACK %s on the way out' % mutation['path'])

    subprocess.run(['cmake', '--build', 'build', '--config', 'Debug'], capture_output=True)
    say('restored')
    if survivors:
        say('\n%d SURVIVED -- each is a test gap or an equivalent mutation, and telling '
            'them apart is a reading job:' % len(survivors))
        for name in survivors:
            say('   ' + name)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
