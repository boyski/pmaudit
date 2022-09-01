#!/usr/bin/env python3
"""
Support module for pmaudit script.
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

import collections
import datetime
import fcntl
import logging
import os
import socket
import stat
import subprocess
import time

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

# I don't think the mtime - atime delta matters except
# that it must be >1 second to avoid roundoff errors.
DELTA = 24 * 60 * 60


# **NOTE** this is a copy from Python2 os.path.walk() since that function
# has been removed in Python3 in favor of os.walk(). Unfortunately os.walk
# tends to update symlink atimes so we can't use it.
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


class Audit(object):
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

# Local Variables:
# mode: Python
# indent-tabs-mode: nil
# python-indent: 4
# fill-column: 80
# End:
#
# vim: sw=4:et:tw=80:cc=+1
