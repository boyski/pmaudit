"""
Run a command with file auditing or query the resulting audit data.

This is the "Poor Man's File Auditor" maintained aand documented at
https://github.com/boyski/pmaudit.

When run with a "--" separator or -c option %(prog)s will invoke the
specified command and record subsequent file accesses to the file
named by the --json option in .json format. When run without "--"
it reads the audit file and dumps results (lists of prereqs, targets,
etc) from it. The audit file defaults to "%(prog)s.json".

Audited files are divided into 5 categories: prerequisites, intermediate
targets, modified sources, final targets, and unused:

  1. A prerequisite is a file which was opened only for read. Commonly
  this will be a pre-existing source file.

  2. An intermediate target is one that was opened for both write and read
  (in that order). The most common example would be a .o file written
  by the compiler and later read by the linker.

  3. A file opened for both read and write in the wrong order (read
  before write) indicates a logic flaw in the build since it amounts
  to modifying a source file. This case generates a warning.

  4. A "final target" is one which was opened only for write. Typically
  it's one of the deliverable build artifacts, the thing you set out
  to create. The classic example would be an executable file created
  by the linker.

  5. An unused file is one not opened at all during the build.

HOW IT WORKS:

This is the simplest form of file access auditing and could almost be
done by hand without scripting. It relies only on standard Unix file
access (atime) and modification (mtime) semantics. Before running the
audited command the timestamps of all existing files in the audited
directory tree are normalized and recorded.  Then, as soon as the command
finishes, the same set of files is rechecked.  If a file existed before
the audit and neither atime nor mtime has moved, clearly it was unused.
If only the atime has advanced it's a prereq. If it only came into
existence during the audit it's a target, and if atime > mtime it's an
intermediate target. Etc.

A nice benefit of this simplicity is that there's practically no
performance cost. Most build-auditing techniques rely on a special
filesystem or ptrace which can slow things down measurably. This method
has no effect on the running build; the only cost is that of traversing
and stat-ing every file in the audited directory(s) before and after.

It won't work unless the filesystem enables atime-updating behavior,
which is the Unix default but is often disabled by NFS servers for
performance reasons. Thus it must be used either in local disk or
an NFS mount without the "noatime" option or with the "relatime"
option. An error is given if atimes don't seem to be updated in the
audited directory's filesystem.

Also, due to the introduction of "relatime" behavior on Linux it's
become necessary to artificially set each pre-existing file's atime
earlier than its mtime during prior-state collection.  Read up on
relatime for background.  The invention of relatime has made the use
of noatime in NFS less common, which is a big help, and relatime is
becoming a common NFS mount option.

If the underlying filesystem doesn't support sufficient timestamp
precision results may be iffy. Clearly, if atimes and mtimes were
recorded only as seconds all files touched within the same second
would have the same timestamp and confusion may occur. Thus %(prog)s is
best used on a modern filesystem which records nanoseconds or similar.
It's not that nanosecond precision per se is required, just that the
more precision the better. This program assumes nanoseconds and expects
platforms with less granularity to fake them padded with zeroes.

As with any file-auditing technology this might expose underlying
weaknesses in the build model. I.e. if a file shows up in a logically
wrong category it may reveal a race condition.

EXAMPLES:

Run an audited make command and leave the results in %(prog)s.json:

    make clean; %(prog)s -- make foobar
    make clean; %(prog)s -c "make foobar"

Dump the prerequisite set derived by the audit above:

    %(prog)s -P %(prog)s.json

Dump the discovered targets, both intermediate and final:

    %(prog)s -T %(prog)s.json

Only final targets:

    %(prog)s -F %(prog)s.json
"""

###############################################################################
# Copyright (C) 2010-2025 David Boyce
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more detail.
#
# You may have received a copy of the GNU General Public License along with
# this program.  If not, see <http://www.gnu.org/licenses/>.
###############################################################################

import argparse
import datetime
import fcntl
from fnmatch import fnmatch
import json
import logging
import os
import os.path as op
import socket
import stat
import subprocess
import sys
import time

PROG = op.basename(__file__)

UTF8 = 'utf-8'

LB, RB = '{', '}'

HOSTNAME = 'hostname'
BASE = 'base'
CMD = 'cmd'
FINAL_CMD = 'final_cmd'
PRE_TIME = 'pre_time'
REF_TIME = 'ref_time'
END_TIME = 'end_time'
PRIOR_COUNT = 'prior_count'
AFTER_COUNT = 'after_count'
PREREQS = 'prereqs'
INTERMEDIATES = 'intermediates'
FINALS = 'finals'
UNUSED = 'unused'
DB = 'db'
JSON = 'pmaudit.json'

# The (mtime - atime) delta probably doesn't matter except
# that it must be >1 second to avoid roundoff errors.
NANOS = 10 ** 9
ATIME_DELTA = 24 * 60 * 60 * NANOS


def time_details(reftime):
    """Turn reftime into readable time information for JSON file."""
    dt = datetime.datetime.utcfromtimestamp(reftime / NANOS)
    refstr = dt.strftime('%Y-%m-%dT%H:%M:%S.%fZ')
    return [refstr, reftime]


def nfs_flush(path):
    """Do whatever it takes to force NFS flushing of metadata if needed."""
    with open(path, encoding=UTF8) as f:
        fcntl.lockf(f.fileno(), fcntl.LOCK_SH, 1, 0, 0)
        fcntl.lockf(f.fileno(), fcntl.LOCK_UN, 1, 0, 0)


class WatchDir():
    """Wrap over dirs and remember whether they need nfs-flushing."""

    def __init__(self, path):
        self.path = path
        self.needflush = self.test_fs()

    def __str__(self):
        return self.path

    def __repr__(self):
        return str(self)

    def test_fs(self):
        """Determine how atime updates are handled in this filesystem."""
        testfile = op.join(
            self.path, f'.{op.basename(__file__)}.{os.getpid()}.tmp')

        with open(testfile, mode='w', encoding=UTF8) as f:
            f.write('random data\n')
            ostats = os.fstat(f.fileno())
        os.utime(testfile, ns=(ostats.st_mtime_ns - ATIME_DELTA,
                               ostats.st_mtime_ns))
        with open(testfile, encoding=UTF8) as f:
            f.read()
        nstats = os.stat(testfile)
        needflush = nstats.st_atime_ns < nstats.st_mtime_ns
        if needflush:
            nfs_flush(testfile)
            with open(testfile, encoding=UTF8) as f:
                f.read()
            nstats = os.stat(testfile)
            absdir = op.dirname(op.abspath(testfile))
            if nstats.st_atime_ns < nstats.st_mtime_ns:
                msg = f'atimes not updated in {absdir}'
                logging.warning(msg)
            else:
                logging.info('NFS flush required in %s', absdir)
        os.remove(testfile)
        return needflush


class PathState():
    """Represent a pathname and its related atime/mtime/size/etc state."""

    def __init__(self, watchdir, relpath, pseudo=False):
        self.watchdir = watchdir
        self.relpath = relpath

        if pseudo:
            self.atime = -2
            self.mtime = -1
            self.size = None
            self.mode = 0
            return

        self.flush()
        stats = os.lstat(self.relpath)
        self.atime = stats.st_atime_ns
        self.mtime = stats.st_mtime_ns
        self.size = stats.st_size
        self.mode = stats.st_mode

    def __str__(self):
        return self.relpath

    def __repr__(self):
        return 'atime=%s mtime=%s size=%s%s' % (
            self.atime,
            self.mtime,
            self.size,
            '*' if self.watchdir.needflush else '')

    def flush(self):
        """Do whatever it takes to force NFS flushing of metadata."""
        if self.watchdir.needflush:
            nfs_flush(self.relpath)

    def islink(self):
        """True iff the file is a symbolic link."""
        return stat.S_ISLNK(self.mode)


class StartFile():
    """Find and sample the file acting as the starting gun."""

    def __init__(self, pattern):
        self.pattern = pattern
        self.path = self.mtime = None

    def __str__(self):
        return '%s: mtime=%d' % (self.path, self.mtime)

    def match(self, path):
        """Try matching the specified path against the stored pattern."""
        matched = fnmatch(op.basename(path), self.pattern)
        if matched and self.path:
            raise RuntimeError('multiple matches for "%s": %s and %s' % (
                self.pattern, self.path, path))
        if not self.path:
            if op.exists(self.pattern):
                self.path = self.pattern
                self.mtime = os.lstat(self.path).st_mtime_ns
            elif matched:
                self.path = path
                self.mtime = os.lstat(self.path).st_mtime_ns


class FinalState():
    """Represent the changes to a given path during the build."""

    def __init__(self, path, prestate, poststate):
        self.path = path
        self.prestate = prestate
        self.poststate = poststate

    def __str__(self):
        return str(self.todict())

    def __repr__(self):
        return str(self)

    def todict(self):
        """Support for JSON serialization."""
        data = {
            'atime': [self.prestate.atime if self.prestate else None,
                      self.poststate.atime],
            'mtime': [self.prestate.mtime if self.prestate else None,
                      self.poststate.mtime],
            'size': [self.prestate.size if self.prestate else None,
                     self.poststate.size],
        }
        return data


class FinalStateEncoder(json.JSONEncoder):
    """Help serialize FinalState objects to JSON."""

    def default(self, o):
        if isinstance(o, FinalState):
            return o.todict()
        return super().default(o)


class PMAudit():
    """Track files used (prereqs) and generated (targets)."""

    def __init__(self, watchdirs, exclude_set=(), startfile=None):
        self.watchdirs = watchdirs
        self.exclude_set = exclude_set
        self.prereqs = {}
        self.intermediates = {}
        self.finals = {}
        self.unused = {}
        self.starttime = None
        self.reftime = None
        self.endtime = None
        self.startfile = startfile
        self.pre_state = {}
        self.post_state = {}

    def getpaths(self, watchdirs, startobj=None):
        """Walk the specified dirs and return a map {path: stats}."""

        paths = {}
        for watchdir in watchdirs:
            for parent, dnames, fnames in os.walk(str(watchdir)):
                dnames[:] = [dname for dname in dnames
                             if not any(fnmatch(dname, pattern)
                                        for pattern in self.exclude_set)]
                for fname in (fn for fn in fnames
                              if not any(fnmatch(fn, pattern)
                                         for pattern in self.exclude_set)):
                    relpath = op.relpath(op.join(parent, fname))
                    pathstate = PathState(watchdir, relpath)
                    if not pathstate.islink():
                        paths[relpath] = pathstate
                    if startobj:
                        startobj.match(relpath)

        return paths

    def start(self):
        """
        Start the build audit.

        There are some builds which touch their prerequisites,
        causing them to look like targets. To protect against
        that we use the belt-and-suspenders approach of checking
        against a list of files which predated the build.

        Also, due to the introduction of the Linux 'relatime' option
        it's necessary to prime the atime pump before starting. This
        is done by making all atimes a bit earlier than their mtimes.
        """

        self.pre_state = self.getpaths(self.watchdirs)
        for path, pre in self.pre_state.items():
            # Modern Linux may not update atime unless it's
            # older than mtime (the "relatime" feature).
            # Set each atime behind its mtime by some delta.
            atime, mtime = pre.atime, pre.mtime
            atime = mtime - ATIME_DELTA
            os.utime(path, ns=(atime, mtime))
            pre.atime = atime

    def finish(self, cmd=None, final_cmd=None):
        """
        End the build audit.
        """

        # Record the set of surviving files with their times,
        # dividing them into the standard categories.
        # For each recorded file 4 timestamps are kept:
        # pre-atime,pre-mtime,post-atime,post-mtime.
        # This data isn't needed once files have been categorized
        # but may be helpful in analysis or debugging so it's
        # preserved in the output file.

        startobj = StartFile(self.startfile) if self.startfile else None

        self.post_state = self.getpaths(self.watchdirs, startobj=startobj)

        # If a start file was requested and found, shift the reftime to match.
        if startobj:
            if not startobj.path:
                raise RuntimeError('no matches for "%s"' % startobj.pattern)
            offset = startobj.mtime - self.reftime
            logging.info('%s: bump start time by %d (%fs)',
                         startobj.path, offset, offset / NANOS)
            self.reftime = startobj.mtime

        # Walk through the post-state, comparing with the pre-state
        # and categorizing.
        for path, post in self.post_state.items():
            pre = self.pre_state.get(path)
            if pre:
                # Compare pre- vs post-states to classify the path.
                endstate = FinalState(path, pre, post)
                if post.atime > pre.atime:
                    if post.mtime > pre.mtime:
                        if post.mtime > post.atime:
                            logging.info('pre-existing %s is FINAL', path)
                            self.finals[path] = endstate
                        else:
                            logging.info('pre-existing %s is TARGET', path)
                            self.intermediates[path] = endstate
                    else:
                        self.prereqs[path] = endstate
                elif post.mtime > pre.mtime:
                    logging.info('pre-existing %s is MODIFIED', path)
                    self.finals[path] = endstate
                else:
                    logging.debug('pre-existing %s is UNUSED', path)
                    self.unused[path] = endstate
            else:
                # In this clause the file didn't exist at all pre-command.
                newfile = PathState(post.watchdir, post.relpath, pseudo=True)
                endstate = FinalState(path, newfile, post)
                if post.mtime <= self.reftime:
                    # This new file has mod time prior to reftime which is
                    # theoretically impossible since it would have been
                    # picked up by the pre-traversal. Therefore the only
                    # way this could happen is if the reftime was shifted
                    # forward by the ref file in which case it must be
                    # considered a prereq.
                    logging.info('pre-generated %s is PREREQ', path)
                    self.prereqs[path] = endstate
                elif post.mtime < post.atime:
                    # Read-after-write makes it an intermediate target.
                    logging.info('generated %s is INTERMEDIATE', path)
                    self.intermediates[path] = endstate
                elif post.mtime > post.atime:
                    # Write-after-read is a weird intermediate/target.
                    logging.info('generated %s is TARGET', path)
                    self.intermediates[path] = endstate
                else:
                    # Otherwise it must be a final target.
                    logging.info('generated %s is FINAL', path)
                    self.finals[path] = endstate

        # Build up and return a serializable database.
        root = {}
        root[HOSTNAME] = socket.gethostname()
        root[BASE] = os.getcwd()
        root[CMD] = str(cmd)
        root[FINAL_CMD] = final_cmd
        root[PRE_TIME] = time_details(self.starttime)
        root[REF_TIME] = time_details(self.reftime)
        root[END_TIME] = time_details(self.endtime)
        root[PRIOR_COUNT] = str(len(self.pre_state))
        after_count = len(self.prereqs) + len(self.intermediates) + \
            len(self.finals) + len(self.unused)
        root[AFTER_COUNT] = str(after_count)
        root[DB] = {}
        root[DB][PREREQS] = self.prereqs
        root[DB][INTERMEDIATES] = self.intermediates
        root[DB][FINALS] = self.finals
        root[DB][UNUSED] = self.unused

        return root


def dump_json(data, filename):
    """Generate and store JSON data with control over formatting."""

    def json_dump_flat(data, open_file, current=0, indent=2):
        """Perform json.dump but display each list as a single line."""
        # Unfortunately json.dump() provides very little formatting control
        # so we'll just re-implement what we need.
        # We assume starting indentation (if needed) has already occurred.
        if data and isinstance(data, dict) and 'atime' not in data:
            open_file.write(LB + '\n')
            new_indent = current + indent
            indent_text = new_indent * ' '
            first = True
            for key in data:
                if not first:  # Add separator (comma) and new line
                    open_file.write(',\n')
                open_file.write(indent_text)
                open_file.write(json.dumps(key))
                open_file.write(': ')
                json_dump_flat(data[key], open_file, new_indent, indent)
                first = False
            open_file.write('\n')  # End last line
            open_file.write(current * ' ')
            open_file.write(RB)
            # Lists have no long dicts so re-implementation isn't needed.
            # Instead, everything *but* dicts are forced onto a single line.
        else:
            json.dump(data, open_file, cls=FinalStateEncoder)

    os.makedirs(op.dirname(filename) or os.curdir, exist_ok=True)
    with open(filename, mode='w', encoding=UTF8) as f:
        # We don't use json.dump() directly because it produces
        # many multi-line lists.
        json_dump_flat(data, f)
        f.write('\n')


def main():
    """Entry point for standalone use."""

    extended_help = len(sys.argv) > 1 and '-H' in sys.argv[1]
    if extended_help:
        sys.argv[1] = sys.argv[1].lower()

    parser = argparse.ArgumentParser(
        epilog=__doc__.strip(),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser_run = parser.add_argument_group('Run')
    parser_run.add_argument(
        '-a', '--audit-dir', action='append',
        metavar='DIR',
        help="audit activity within DIRs ['.']")
    parser_run.add_argument(
        '-c', '--cmd', action='store_true',
        help="run and audit the specified command line")
    parser_run.add_argument(
        '-d', '--depsfile',
        metavar='FILE',
        help="save prerequisite data to FILE in makefile format"
        if extended_help else argparse.SUPPRESS)
    parser_run.add_argument(
        '-e', action='store_true',
        help="run CMD in -e (errexit) mode"
        if extended_help else argparse.SUPPRESS)
    parser_run.add_argument(
        '--exclude', action='append', default=['.git', '.svn'],
        help="globs to exclude from searches")
    parser_run.add_argument(
        '--flush-host',
        help="a second host from which to force client flushes"
        if extended_help else argparse.SUPPRESS)
    parser.add_argument(
        '-j', '--json',  # default='%s.json' % PROG
        metavar='FILE',
        help="save audit data in JSON format to FILE")
    parser_run.add_argument(
        '--multiline', action='store_true',
        help="separately run each line in the -c string"
        if extended_help else argparse.SUPPRESS)
    parser_run.add_argument(
        '-s', '--start-file',
        help="offset timestamps from this generated file")
    parser.add_argument(
        '--shell', default='/bin/sh',
        help="name of shell to run [%(default)s]"
        if extended_help else argparse.SUPPRESS)
    parser.add_argument(
        '--shellflags', default='-c',
        help="space-separated flags for shell [%(default)s]"
        if extended_help else argparse.SUPPRESS)
    parser_run.add_argument(
        '-V', '--verbosity', action='count', default=0,
        help="bump verbosity level")

    parser_query = parser.add_argument_group('Query')
    parser_query.add_argument(
        '-A', '--query-all-involved', action='store_true',
        help="list all involved files")
    parser_query.add_argument(
        '-C', '--query-cmd', action='store_true',
        help="show the command(s) run")
    parser_query.add_argument(
        '-F', '--query-final-targets', action='store_true',
        help="list final target files")
    parser_query.add_argument(
        '-I', '--query-intermediates', action='store_true',
        help="list intermediate target files")
    parser_query.add_argument(
        '-P', '--query-prerequisites', action='store_true',
        help="list prerequisite files")
    parser_query.add_argument(
        '-T', '--query-targets', action='store_true',
        help="list all target files (intermediate and final")
    parser_query.add_argument(
        '-U', '--query-unused', action='store_true',
        help="list files present but unused")
    parser_query.add_argument(
        '--alpha-sort', action='store_true',
        help="sort results alphabetically rather than by atime")

    opts, unparsed = parser.parse_known_args()

    # Configure logging.
    logging.basicConfig(
        format=PROG + ': %(levelname)s: %(message)s',
        level=max(logging.DEBUG, logging.WARNING - (
            logging.DEBUG * opts.verbosity)))

    if '--' in sys.argv or opts.cmd:
        if '--' in sys.argv:
            if '--' in unparsed:
                unparsed.remove('--')
            while '=' in unparsed[0]:
                var, val = unparsed[0].split('=', 1)
                os.environ[var] = val
                unparsed.pop(0)
            assert not opts.e
            cmds = [unparsed]
            cmd_text = ' '.join(unparsed)
        else:
            cmdstr = unparsed.pop(0)
            if unparsed:
                raise RuntimeError(f'unparsed options: {unparsed}')
            cmd = [opts.shell]
            if opts.e:
                cmd.append('-e')
            if opts.verbosity:
                cmd.append('-x')
            if opts.shellflags:
                cmd += opts.shellflags.split()
            if opts.multiline:
                cmds = [cmd + [line] for line in cmdstr.splitlines()]
            else:
                cmds = [cmd + [cmdstr]]
            cmd_text = cmdstr

        if not opts.json and not opts.depsfile:  # Implement default save.
            opts.json = JSON

        wdirs = []
        if opts.audit_dir:
            for word in opts.audit_dir:
                wdirs.extend(word.split(','))
        else:
            wdirs.append(os.curdir)

        exclude_set = set(opts.exclude)
        if opts.json:
            exclude_set.add(opts.json)
        if opts.depsfile:
            exclude_set.add(opts.depsfile)

        audit = PMAudit([WatchDir(w) for w in wdirs],
                        exclude_set=exclude_set,
                        startfile=opts.start_file)
        audit.start()
        audit.starttime = audit.reftime = time.time_ns()
        for cmd in cmds:  # Execute each line in sequence.
            rc = subprocess.call(cmd)
            # Stop if we get an error.  Note that "make" does this *even*
            # in its -k/--keep-going mode because -k is per recipe whereas
            # this is per line of recipe.
            if rc != 0:
                break
        audit.endtime = time.time_ns()
        adb = audit.finish(cmd=cmd_text, final_cmd=cmds)

        if opts.depsfile:
            prqs = adb[DB][PREREQS]
            # *Always* create depsfile, even if prqs is empty.
            os.makedirs(op.dirname(opts.depsfile), exist_ok=True)
            with open(opts.depsfile, mode='w', encoding=UTF8) as f:
                f.write(op.splitext(opts.depsfile)[0] + ': \\\n')
                for i, prq in enumerate(prqs):
                    eol = ' \\\n' if i < len(prqs) - 1 else '\n'
                    f.write(f'  {prq}{eol}')
                for prq in prqs:
                    f.write(f'\n{prq}:\n')
        if opts.json:
            dump_json(adb, opts.json)
        sys.exit(2 if rc else 0)

    # Don't analyze, e.g., report from an existing audit log.
    # In this mode we are not a wrapper so can reparse the
    # command line more strictly.
    parser.add_argument(
        'dbfile', default=JSON, nargs='?',
        metavar='FILE',
        help="query audit data from FILE [%(default)s]")
    opts = parser.parse_args()

    with open(opts.dbfile, encoding=UTF8) as f:
        root = json.load(f)

    if opts.query_cmd:
        print(root['cmd'])

    if opts.query_all_involved:
        opts.query_prerequisites = True
        opts.query_intermediates = True
        opts.query_final_targets = True
    elif opts.query_targets:
        opts.query_intermediates = True
        opts.query_final_targets = True

    results = {}

    db = root[DB]
    if opts.query_intermediates:
        results.update(db[INTERMEDIATES])
    if opts.query_prerequisites:
        results.update(db[PREREQS])
    if opts.query_final_targets:
        results.update(db[FINALS])
    if opts.query_unused:
        results.update(db[UNUSED])

    def by_atime(item):
        """Sort by post-atime."""
        return float(item[1]['atime'][1])

    key = None if opts.alpha_sort else by_atime
    for path in [k for k, _ in sorted(results.items(), key=key)]:
        print(path)

# Local Variables:
# mode: Python
# indent-tabs-mode: nil
# python-indent: 4
# fill-column: 80
# End:
#
# vim: sw=4:et:tw=80:cc=+1
