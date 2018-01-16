# Poor Man's Audit[*]

[*] With apologies for the implied classism and sexism :-)

This project comprises two programs which act as build auditors. A
build auditor is a tool which runs a software build (or really any
process which reads a set of input files and produces a set of output
files) and afterward can report which files were which. In other words
an audit can tell you the list of files consumed by the build, those
created during it, and ignored by it.

Most build-auditing (aka file auditing) tools are complex and expensive
(in both senses of the word). They may involve a custom filesystem,
server processes, or rely on ptrace (essentially running audited
processes in the debugger).  This system is at the other end of the
spectrum: it's very simple and unambitious, relying only on standard
Unix filesystem semantics, and with minimal performance cost (aside
from the loss of parallelism, see below).

One tool (pmaudit) is written in Python, the other (pmash) is a C program
using similar techniques.  Naturally pmash is much faster but pmaudit is
more flexible. Either can be used as a shell wrapper. See details below.

Invoke them with --help for detailed explanation, usage, and examples.

## The Programs

There are two different tools here which rely on the same basic technique
while having slightly different features. Either can be used as a SHELL
replacement in make though of course the C program will be much faster.
When used as shell wrappers they dump prerequisite data in "make"
format. Either may be told which directory(s) to monitor via the -W,
--watch option.

### pmaudit

A Python script which may be used from the top level of a build.
It derives a complete "database" of all files accessed (and not accessed)
during the build and categorizes them as prerequisites, targets, etc
as described previously. This database is useful for many things but
it does not have data with sufficient granularity to tell you which
prereqs were required by which targets.

It can be used as a shell wrapper.

### pmash

A C program which operates as a wrapper over the shell. Due to being
written in C it's much faster than pmaudit but more limited.  It derives
only per-target prerequisite data.

#### pmamake

A tiny shell wrapper which exists to document ways by which either tool
can be introduced into a GNU make build. It's intended solely as a demo.

## Uses of Build Auditing

A few examples of where build auditing could be useful:

### Cleanup

Say you have a large number of files in your source code repository.
After many changes and refactorings over the years you no longer know
which files are still in use. A build audit can tell you this so you
can prune the rest.

### Streamlined Checkouts

Similar to above: if you know the list of files needed to build project A,
even if other files must be preserved for building projects B, C, etc, it
may be helpful to check out only the subset needed by A for bandwidth or
disk space reasons. Auditing can help you find that subset.

### Source Packaging

Imagine you have a proprietary code base and contracts with various
customers to provide source code for what you sell them. Not every
customer gets every product: customer A has a contractual right to
source for products X and Y while customer B gets source for Z. If the
build of a product produces the list of files involved in making it
as a side effect, that list can be used with tar or zip to make up a
minimal but guaranteed-complete source package.

### Dependency Analysis

If we know the exact set of files required to build a particular object
file we can pass it to "make" or similar to create a complete dependency
graph resulting in more reliable and/or parallelizable builds. There
are many existing solutions to this but most are language-specific
e.g. the gcc -M option. File-level auditing is lagnuage-agnostic.

## What Could Go Wrong?

Are there flaws in this system? Because of its radical simplicity
and lack of ambition there isn't much room for bugs but plenty of
limitations.  Here are some I can think of:

### Interference from another process

If some unrelated process comes over and opens files in the audit area
during the run the results will of course be contaminated. So don't
do that.

### Cannot support parallelism

Because this technique involves sampling the states of files before and
after, parallel processes would interfere with it. This is really just
the same as above with the interference coming from within.

Of course this raises a conundrum; much of the value of having a complete
dependency graph is that it enables robust parallelism. This tool helps
us derive a complete dependency graph while at the same time ruling out
parallelism so what's the point? The best answer for now is that it may
be helpful to generate/update dependency data in a scheduled (hourly,
daily, etc) serial build and let developers rely on that slightly stale
data. Or use it occasionally to find gaps in hardwired data, or to
debug a particular intermittent build race, etc.

### Permission problems

Due to the necessity of updating access times (atimes) you may need
to own the input files. In any build scenario you'd need the right
to write output files anyway, and of course you'd own those, so this
basically extends the ownership requirement to prerequisite files.

### Unseen files

The script needs to know which files to monitor, and files in an
unmonitored area will not be recorded. This can be seen as a feature or
a bug. For instance, when compiling a collection of .c and .h files do
you want it to record /usr/include/stdio.h as one of the prereqisites
or is that TMI? Regardless, it will only record accesses to files in
monitored locations and monitored locations must generally be writable.

### Atimes not updated due to mount settings

This is a big one but there's a test to detect it. System admins often
turn off atime updating on NFS as a performance enhancer. This is
really a system admin issue, not something the script can deal with,
so it just dies when atimes aren't behaving. It's possible that system
admins could be convinced to use the relatively new "relatime" feature
rather than turning off atime updates altogether.

### Weak granularity of file timestamps

Another common issue. Some filesystems still record only seconds
or milliseconds and the resulting roundoff errors can lead to bogus
results. It works best on high-resolution filesystems such as Linux
ext4 which records nanoseconds.

# Example

A good sample use is building GNU make itself (of course it already
generates its own dependency data but we're going to ignore that).
First we unpack and configure version 4.2.1 and make sure the state
is clean:

% make clean >clean.log

Then build with the auditor inserted:

% make --eval='.ONESHELL:' SHELL=pmash .SHELLFLAGS='-o $@.d -c' > make.log 2>&1

And look at a typical generated dependency file:

% cat job.o.d
job.o: \
  commands.h \
  config.h \
  debug.h \
  filedef.h \
  getopt.h \
  gettext.h \
  gnumake.h \
  hash.h \
  job.c \
  job.h \
  makeint.h \
  os.h \
  output.h \
  variable.h

Feel free to compare this with GNU make's native generated deps file
(.deps/job.Po) which contains data for system files as well.

Let's break down the command line above. We use SHELL=pmash to force
make to use our auditor as a shell wrapper and .SHELLFLAGS to control
the flags passed to it; in particular it takes the usual -c option and
passes that command line to the shell. We add "-o $@.d" to send pmash
output to foo.d when building foo.

The use of .ONESHELL is necessary to cause all build activity for the
target to take place in a single shell process. Otherwise, since each
recipe line is a different shell and they share the same value of $@
the output file would be overwritten. Here GNU make is a good example:
the recipe for each object file tends to look like this (simplified):

    gcc -MT job.o -MD -MP -MF .deps/job.Tpo -c -o job.o job.c
    mv -f .deps/job.Tpo .deps/job.Po

Without .ONESHELL the mv command would run last and in its own shell
so we'd end up recording only the actions of mv, not gcc.
