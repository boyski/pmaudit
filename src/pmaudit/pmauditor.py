"""
Run a command with file auditing or query the resulting audit data.

When run with a "--" separator %(prog)s will invoke the command to the
right of the separator and record subsequent file accesses to the file
named by the --json option in .json format. When run without "--" it reads the
audit file and dumps results (things like prereqs, targets, etc) from it.
The audit file defaults to "%(prog)s.json".

Audited files are divided into 5 categories: prerequisites, intermediate
targets, modified sources, final targets, and unused:

  1. A prerequisite is a file which was opened only for read; commonly
  this will be a source file originally checked out from SCM.

  2. An intermediate target is one that was opened for both write and read
  (in that order). The most common example would be a .o file written
  by the compiler and later read by the linker.

  3. A file opened for both read and write in the other order (read before
  write) indicates a logic flaw in the build since it amounts to modifying
  a source file. This case generates a warning.

  4. A final target is a file which was opened only for write.
  Typically it's one of the final, deliverable build artifacts, the
  thing you set out to build. The classic example would be an executable
  file created by the linker.

  5. An unused file, naturally, is one not opened at all during the build.

HOW IT WORKS:

This is the simplest form of file access auditing and could almost be
done by hand without scripting. It relies only on standard Unix file
access (atime) and modification (mtime) semantics. Before running
the audited command the timestamps of all existing files in the
audited directory tree are normalized and recorded.  Then, as soon as
the command finishes, the same set of files is rechecked.  If a file
existed before the audit and neither atime nor mtime has moved,
clearly it was unused.  If only the atime has advanced it's a prereq. If
it came into existence during the audit it's a target, and if atime >
mtime it's an intermediate target.  Etc.

A nice benefit of this simplicity is that there's practically no
performance cost. Most build-auditing techniques rely on a special
filesystem or ptrace which can slow things down measurably. This method
has no effect on the running build; the only cost is that of traversing
and stat-ing every file in the audited directory before and after.

It won't work unless the filesystem enables atime-updating behavior, which
is the traditional default but is often disabled by NFS servers for
performance reasons. Thus it must be used either in local disk or an NFS
mount without the "noatime" option. An error is given if atimes don't seem
to be updated in the audited directory's filesystem.

Also, due to the introduction of "relatime" behavior on Linux it's
become necessary to artificially set each pre-existing file's atime
prior to its mtime during prior-state collection.  Read up on relatime
for background.  The invention of relatime has made the use of noatime
in NFS less common, which is a big help, and relatime is becoming the
default NFS mount option.

If the underlying filesystem doesn't support sufficient timestamp
precision, results may be iffy. Clearly, if atimes and mtimes were
recorded only as seconds all files touched within the same second would
have the same timestamp and confusion may occur. Thus this is best used on
a modern filesystem which records nanoseconds or similar.  It's not that
nanosecond precision per se is required, just that the more precision the
better.

As with any file-auditing technology this might expose underlying
weaknesses in the build model. I.e. if a file shows up in the wrong
category it may reveal a race condition.

EXAMPLES:

Run an audited make command and leave the results in %(prog)s.json:

    make -C subdir clean; %(prog)s -- make -C subdir ...

Dump the prerequisite set derived by the audit above:

    %(prog)s -P %(prog)s.json

Dump the discovered targets, both intermediate and final:

    %(prog)s -T %(prog)s.json
"""

###############################################################################
# Copyright (C) 2010-2022 David Boyce
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
import collections
import datetime
import fcntl
import json
import logging
import os
import socket
import stat
import subprocess
import sys
import time

PROG = os.path.basename(__file__)

CUSTOM = 'custom'
LB, RB = '{', '}'

FMT1 = '%.07f'  # Format for one time value

HOSTNAME = 'hostname'
BASE = 'base'
CMD = 'cmd'
FINAL_CMD = 'final_cmd'
START_TIME = 'start_time'
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
DELTA = 24 * 60 * 60


# **NOTE** this is a copy from Python2 os.path.walk() since that function
# has been removed in Python3 in favor of os.walk(). Unfortunately os.walk()
# tends to update symlink atimes so it can't be used here.
def os_path_walk(top, func, arg):
    """Directory tree walk with callback function.

    For each directory in the directory tree rooted at top (including top
    itself, but excluding '.' and '..'), call func(arg, dirname, fnames).
    dirname is the name of the directory, and fnames a list of the names of
    the files and subdirectories in dirname (excluding '.' and '..').  func
    may modify the fnames list in-place (e.g. via del or slice assignment),
    and walk will only recurse into the subdirectories whose names remain in
    fnames; this can be used to implement a filter, or to impose a specific
    order of visiting.  No semantics are defined for, or required of, arg,
    beyond that arg is always passed to func.  It can be used, e.g., to pass
    a filename pattern, or a mutable object designed to accumulate
    statistics.  Passing None for arg is common."""

    try:
        names = os.listdir(top)
    except OSError:
        return
    func(arg, top, names)
    for name in names:
        name = os.path.join(top, name)
        try:
            st = os.lstat(name)
        except OSError:
            continue
        if stat.S_ISDIR(st.st_mode):
            os_path_walk(name, func, arg)


def time_details(reftime):
    """Turn reftime into time information for JSON file."""
    dt = datetime.datetime.utcfromtimestamp(reftime)
    refstr = dt.strftime('%Y-%m-%dT%H:%M:%S.%fZ')
    return [refstr, FMT1 % reftime]


def nfs_flush(priors, host=None):
    """Do whatever it takes to force NFS flushing of metadata."""
    apaths = sorted([os.path.abspath(p)
                     for p in priors if priors[p]['needflush']])
    if host and apaths:
        oldest = int(min((priors[k]['mtime'] for k in priors)))
        cmd = ['ssh', host, '--', 'xargs', 'touch', '-a', '-t']
        cmd.append(time.strftime('%Y%m%d%H%M', time.localtime(oldest - DELTA)))
        if len(apaths) > 1:
            logging.info('flushing %d files with "%s"',
                         len(apaths), ' '.join(cmd))
        cmd.insert(1, '-oLogLevel=error')
        touches = '\n'.join(apaths) + '\n'
        with subprocess.Popen(cmd, stdin=subprocess.PIPE) as proc:
            proc.stdin.write(touches.encode('utf-8'))
            proc.stdin.close()
    else:
        for path in sorted(priors):
            with open(path, encoding='utf-8') as f:
                fcntl.lockf(f.fileno(), fcntl.LOCK_SH, 1, 0, 0)
                fcntl.lockf(f.fileno(), fcntl.LOCK_UN, 1, 0, 0)


class PMAudit(object):
    """Track files used (prereqs) and generated (targets)."""

    def __init__(self, watchdirs, exclude=None):
        self.watchdirs = watchdirs
        self.exclude = set(['.git', '.svn'])
        if exclude:
            self.exclude |= exclude
        self.prereqs = collections.OrderedDict()
        self.intermediates = collections.OrderedDict()
        self.finals = collections.OrderedDict()
        self.unused = collections.OrderedDict()
        self.reftime = None
        self.endtime = None
        self.prior = {}

    def start(self, flush_host=None, keep_going=False):
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

        mkflags = os.getenv('MAKEFLAGS')
        if mkflags and ' -j' in mkflags:
            raise RuntimeError('not supported in -j mode')

        for watchdir in self.watchdirs:
            # Figure out how atime updates are handled in this filesystem.
            ref_fname = os.path.join(
                watchdir, f'.{os.path.basename(__file__)}.{os.getpid()}.tmp')
            with open(ref_fname, mode='w', encoding='utf-8') as f:
                f.write('data\n')
                ostats = os.fstat(f.fileno())
            os.utime(ref_fname, (ostats.st_mtime - DELTA, ostats.st_mtime))
            with open(ref_fname, encoding='utf-8') as f:
                f.read()
            nstats = os.stat(ref_fname)
            needflush = nstats.st_atime < nstats.st_mtime
            if needflush:
                nfs_flush({ref_fname: {'atime': nstats.st_atime,
                                       'mtime': nstats.st_mtime,
                                       'size': nstats.st_size,
                                       'needflush': True}}, host=flush_host)
                with open(ref_fname, encoding='utf-8') as f:
                    f.read()
                nstats = os.stat(ref_fname)
                apath = os.path.dirname(os.path.abspath(ref_fname))
                if nstats.st_atime < nstats.st_mtime:
                    msg = f'atimes not updated in {apath}'
                    if not keep_going:
                        raise RuntimeError(msg)
                    logging.warning(msg)
                else:
                    logging.info('NFS flush required in %s', apath)
            os.remove(ref_fname)

            for parent, dnames, fnames in os.walk(watchdir):
                dnames[:] = (dn for dn in dnames if dn not in self.exclude)
                for fname in fnames:
                    if fname in self.exclude:
                        continue
                    path = os.path.relpath(os.path.join(parent, fname))
                    if os.path.islink(path):
                        continue
                    # Modern Linux won't update atime unless it's
                    # older than mtime (the "relatime" feature).
                    stats = os.stat(path)
                    atime, mtime = (stats.st_atime, stats.st_mtime)
                    if atime >= mtime:
                        atime = mtime - DELTA
                        os.utime(path, (atime, mtime))
                    self.prior[path] = {'atime': atime,
                                        'mtime': mtime,
                                        'size': stats.st_size,
                                        'needflush': needflush}

        nfs_flush(self.prior, host=flush_host)

        self.reftime = time.time()

    def finish(self, cmd=None, final_cmd=None):
        """End the audit, return the result."""

        # Record the set of surviving files with their times,
        # dividing them into the standard categories.
        # For each recorded file 4 timestamps are kept:
        # "pre-atime,pre-mtime,post-atime,post-mtime".
        # This data isn't needed once files have been categorized
        # but may be helpful in analysis or debugging.
        # Note: we can't use os.walk() for this because it has a
        # way of updating symlink atimes.
        prereqs, intermediates, finals, unused = {}, {}, {}, {}

        def visit(prunedirs, parent, fnames):
            """Callback function for os_path_walk() to categorize files."""
            for prunedir in prunedirs:
                if parent.startswith(prunedir):
                    return
            if os.path.basename(parent) in self.exclude:
                prunedirs.add(parent + os.sep)
                return
            for fname in fnames:
                if fname in self.exclude:
                    continue
                path = os.path.relpath(os.path.join(parent, fname))
                if os.path.isdir(path):
                    continue
                stats = os.lstat(path)
                atime, mtime = stats.st_atime, stats.st_mtime
                pstate = self.prior.get(path)
                if pstate:
                    if atime > pstate['atime']:
                        val = {'atime': [FMT1 % pstate['atime'], FMT1 % atime],
                               'mtime': [FMT1 % pstate['mtime'], FMT1 % mtime],
                               'size': [pstate['size'], stats.st_size]}
                        if mtime > pstate['mtime']:
                            if mtime > atime:
                                finals[path] = val
                                msg = 'pre-existing file is final'
                            else:
                                intermediates[path] = val
                                msg = 'pre-existing file is target'
                            logging.info('%s: %s', msg, path)
                        else:
                            prereqs[path] = val
                    elif mtime > pstate['mtime']:
                        val = {'atime': [FMT1 % pstate['atime'], FMT1 % atime],
                               'mtime': [FMT1 % pstate['mtime'], FMT1 % mtime],
                               'size': [pstate['size'], stats.st_size]}
                        finals[path] = val
                        logging.info('pre-existing file modified: %s', path)
                    else:
                        val = {'atime': [FMT1 % pstate['atime'], '0'],
                               'mtime': [FMT1 % pstate['mtime'], '0'],
                               'size': [pstate['size'], pstate['size']]}
                        unused[path] = val
                        continue
                else:
                    val = {'atime': ['-2', FMT1 % atime],
                           'mtime': ['-1', FMT1 % mtime],
                           'size': [None, stats.st_size]}
                    if mtime < atime:
                        intermediates[path] = val
                    else:
                        finals[path] = val

        for watchdir in self.watchdirs:
            os_path_walk(watchdir, visit, set())  # pylint: disable=no-member

        # Sort the data just derived. Not needed but helps readability.
        for k in sorted(prereqs):
            self.prereqs[k] = prereqs[k]
        for k in sorted(intermediates):
            self.intermediates[k] = intermediates[k]
        for k in sorted(finals):
            self.finals[k] = finals[k]
        for k in sorted(unused):
            self.unused[k] = unused[k]

        # Build up and return a serializable database.
        root = collections.OrderedDict()
        root[HOSTNAME] = socket.gethostname()
        root[BASE] = os.getcwd()
        root[CMD] = str(cmd)
        root[FINAL_CMD] = final_cmd
        root[START_TIME] = time_details(self.reftime)
        root[PRIOR_COUNT] = str(len(self.prior))
        after_count = len(self.prereqs) + len(self.intermediates) + \
            len(self.finals) + len(self.unused)
        root[AFTER_COUNT] = str(after_count)
        root[DB] = collections.OrderedDict()
        root[DB][PREREQS] = self.prereqs
        root[DB][INTERMEDIATES] = self.intermediates
        root[DB][FINALS] = self.finals
        root[DB][UNUSED] = self.unused

        self.endtime = time.time()
        root[END_TIME] = time_details(self.endtime)

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
            json.dump(data, open_file)

    os.makedirs(os.path.dirname(filename) or os.curdir, exist_ok=True)
    with open(filename, mode='w', encoding='utf-8') as f:
        # We don't use json.dump() directly because it produces
        # many multi-line lists.
        json_dump_flat(data, f)
        f.write('\n')


def custom_results(entries):
    """Convert a list of key=value pairs into a dictionary."""
    result = {}
    if entries:
        for entry in entries:
            if '=' in entry:
                key, value = entry.split('=', maxsplit=1)
                result[key] = value
            else:  # if no value is given, treat it as a boolean.
                result[entry] = True
    return result


def main():
    """Entry point for standalone use."""

    if len(sys.argv) == 1:
        sys.argv.append('--help')

    parser = argparse.ArgumentParser(
        epilog=__doc__.strip(),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        '-c', action='store_true',
        help="run and audit the specified command line")
    parser.add_argument(
        '--custom', action='append',
        metavar='KEY_VALUE_PAIR',
        help="include custom key=value pair in JSON results")
    parser.add_argument(
        '-d', '--depsfile',
        metavar='FILE',
        help="save prerequisite data to FILE in makefile format")
    parser.add_argument(
        '-e', action='store_true',
        help="run CMD in -e (errexit) mode")
    parser.add_argument(
        '--flush-host',
        help="a second host from which to force client flushes")
    parser.add_argument(
        '-j', '--json',  # default='%s.json' % PROG
        metavar='FILE',
        help="save audit data in JSON format to FILE")
    parser.add_argument(
        '-k', '--keep-going', action='store_true',
        help="continue even if atimes aren't updated")
    parser.add_argument(
        '--multiline', action='store_true',
        help="separately run each line in the -c string")
    parser.add_argument(
        '-A', '--all-involved', action='store_true',
        help="list all involved files")
    parser.add_argument(
        '-F', '--final-targets', action='store_true',
        help="list final target files")
    parser.add_argument(
        '-I', '--intermediates', action='store_true',
        help="list intermediate target files")
    parser.add_argument(
        '-P', '--prerequisites', action='store_true',
        help="list prerequisite files")
    parser.add_argument(
        '-T', '--targets', action='store_true',
        help="list all target files (intermediate and final")
    parser.add_argument(
        '-U', '--unused', action='store_true',
        help="list files present but unused")
    parser.add_argument(
        '-V', '--verbosity', action='count', default=0,
        help="bump verbosity level")
    parser.add_argument(
        '-W', '--watch', action='append',
        metavar='DIR',
        help="audit activity within DIRs (default='.')")
    parser.add_argument(
        '--shell', default='/bin/sh',
        help="name of shell to run (default=%(default)s)")
    parser.add_argument(
        '--shellflags', default='-c',
        help="space-separated flags to pass to shell (default=%(default)s)")

    opts, unparsed = parser.parse_known_args()

    # Configure logging.
    logging.basicConfig(
        format=PROG + ': %(levelname)s: %(message)s',
        level=max(logging.DEBUG, logging.WARNING - (
            logging.DEBUG * opts.verbosity)))

    if '--' in sys.argv or opts.c:
        if '--' in sys.argv:
            if '--' in unparsed:
                unparsed.remove('--')
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
        if opts.watch:
            for word in opts.watch:
                wdirs.extend(word.split(','))
        else:
            wdirs.append(os.curdir)
        exclude_set = set()
        if opts.json:
            exclude_set.add(opts.json)
        if opts.depsfile:
            exclude_set.add(opts.depsfile)
        audit = PMAudit(wdirs, exclude=exclude_set)
        audit.start(flush_host=opts.flush_host, keep_going=opts.keep_going)
        for cmd in cmds:  # Execute each line in sequence.
            rc = subprocess.call(cmd)
            # Stop if we get an error.  Note that "make" does this *even*
            # with -k/--keep-going mode because -k is per recipe whereas
            # this is per line of recipe.
            if rc != 0:
                break
        adb = audit.finish(cmd=cmd_text, final_cmd=cmds)
        adb[CUSTOM] = custom_results(opts.custom)
        if opts.depsfile:
            prqs = adb[DB][PREREQS]
            # *Always* create depsfile, even if prqs is empty.
            os.makedirs(os.path.dirname(opts.depsfile), exist_ok=True)
            with open(opts.depsfile, mode='w', encoding='utf-8') as f:
                f.write(os.path.splitext(opts.depsfile)[0] + ': \\\n')
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
        help="query audit data from FILE (default=%(default)s)")
    opts = parser.parse_args()

    with open(opts.dbfile, encoding='utf-8') as f:
        root = json.load(f)
    db = root[DB]

    if opts.all_involved:
        opts.prerequisites = opts.intermediates = opts.final_targets = True
    elif opts.targets:
        opts.intermediates = opts.final_targets = True

    results = {}

    if opts.intermediates:
        results.update(db[INTERMEDIATES])
    if opts.prerequisites:
        results.update(db[PREREQS])
    if opts.final_targets:
        results.update(db[FINALS])
    if opts.unused:
        results.update(db[UNUSED])

    def by_atime(item):
        """Sort by post-atime."""
        return float(item[1]['atime'][1])

    for path in [k for k, _ in sorted(results.items(), key=by_atime)]:
        print(path)

# Local Variables:
# mode: Python
# indent-tabs-mode: nil
# python-indent: 4
# fill-column: 80
# End:
#
# vim: sw=4:et:tw=80:cc=+1
