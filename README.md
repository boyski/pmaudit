# Poor Man's File Auditor[*]

This project comprises two programs which can act as build auditors. A
build auditor is a tool which runs a software build (or really any process
that reads a set of input files and produces a set of output files) and
afterward can report which files were which. In other words a build audit
can determine the set of files consumed by the build, those produced by
it, and those ignored by it.

Most build-auditing (aka file auditing) tools are complex and expensive
(in both senses of the word). They may involve a custom filesystem such as
ClearCase, server processes, or rely on ptrace (essentially running
audited processes in the debugger).  This system is at the other end of
the spectrum: it's very simple and unambitious, relying only on standard
Unix filesystem semantics, and with minimal performance cost (aside from
the loss of parallelism, see below).

One script (pmaudit) is written in Python, the other (pmash) is a C
program using similar techniques.  Naturally pmash is much faster but
pmaudit has more capabilities. Either can be used as a shell wrapper. See
details below.

Invoke these with --help for detailed explanation, usage, and examples.

## The Programs

There are two different auditing tools which rely on the same basic
technique while having slightly different features. Either can be used as
a shell replacement in make though of course the C program will be faster.
When used as shell wrappers they dump prerequisite data in GNU make
format.

### pmaudit

This is a Python script typically used from the top level of a build. In
this mode it derives a complete JSON "database" of all files involved in
the build and categorizes them as prerequisites, targets, and unused. This
database is useful for many things but it does not have data with
sufficient granularity to tell you which prereqs were required by which
targets.

The pmaudit script can also be used as a shell wrapper to generate
per-target dependency data in make format. In this mode it's similar
to pmash but slower.

### pmash

This is a C program which operates as a wrapper over the shell. Due to
being written in C it's much faster than pmaudit but has fewer features.
It derives only per-target prerequisite data and writes it only in make
format.

Other features of pmaudit could be imported into pmash for speed.

### pmamake

This is a tiny shell wrapper provided to document ways by which either
tool could be introduced into a GNU make build. It's intended solely as a
demo of what can be done.

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
disk space reasons. Auditing can help you determine that subset.

### Source Packaging

Imagine you have a proprietary code base and contracts with various
customers to provide source code for what you sell them. Not every
customer gets every product: customer A has a contractual right to source
for products X and Y while customer B gets Y and Z. If the build of each
product derives the set of files involved as a side effect, that list can
be used with tar or zip to generate a minimal but guaranteed-complete
source package.

### Dependency Analysis

If we know the exact set of files required to build a particular object
file we can pass it to "make" or similar to create a complete dependency
graph resulting in more reliable and/or parallelizable builds. There
are many existing solutions to this but most are language-specific
e.g. the gcc -M option. File-level auditing has the benefit of being
language-agnostic.

## What Could Go Wrong?

Are there flaws in this system? Because of its radical simplicity and
lack of ambition there isn't much room for bugs but there are some
limitations. In general, auditing the entire build from the top as a
black box is fairly robust as long as your filesystem is configured
to update atimes and you have the required permissions. Auditing on a
per-recipe basis can be more finicky.

Here are some concerns I can think of:

### Interference From Another Process

If some unrelated process comes over and opens files in the audit area
during the run the results will of course be contaminated, so don't
do that. Of course this would be a problem for an unaudited build too.

### No Support For Parallelism At The Shell Level

Because the "pmash" technique involves sampling the states of files before
and after the recipe runs, parallel processing could interfere with it.
This is really just the same as above with the interference coming from
within.  This is not a problem for top-level, black-box auditing since all
changes are subsumed into one transaction; it's only per-shell or
per-target auditing that has parallelism restrictions.

Of course this raises a conundrum; much of the value in having a complete
dependency graph is that it enables robust parallelism. This tool helps
us derive a complete dependency graph while at the same time ruling
out parallelism, so what's the point? The best answer for now is that
it may be helpful to generate/update dependency data in a scheduled
(hourly, daily, etc) serial build and let developers rely on that
slightly stale data to do parallel builds. Or use it occasionally to
find gaps in hardwired data, or to debug a particular build race, etc.

### Unseen Files

The script needs to know which files to monitor, and files in an
unmonitored area will not be recorded. This can be seen as a feature or a
bug. For instance, when natively compiling a project of C code do you want
it to record /usr/include/stdio.h as one of the prereqisites or is that
TMI? Pmaudit will only record accesses to files in monitored locations
which can be configured with the --watch option.

The simplest approach here is to start the build from the base of the
source tree using the GNU make -C option or equivalent e.g.:

    $ cd sub/dir && pmaudit -- make ...       # BAD
    $ pmaudit -- make -C sub/dir ...          # GOOD

Since the default watch location is the current working directory, the
first case will see only files under sub/dir while the second will see all
files in the source tree.

### Atimes Not Updated Due to Mount Settings

This can be a show stopper but the tools run a test to detect it. System
admins may turn off atime updating on NFS as a performance enhancer. This
is really a system admin issue, not something the script can deal with, so
it just dies when atimes aren't behaving. Consider asking system (NFS)
administrators to use the relatively new "relatime" feature rather than
turning off atime updates altogether. Fortunately relatime is a default
mount option in modern Linux.

Local filesystems almost always have atimes enabled.

### Weak Granularity of File Timestamps

Another common issue: some filesystems still record only seconds
or milliseconds and the resulting roundoff errors can lead to bogus
results. The pmaudit technique works best on high-resolution filesystems
such as Linux ext4 which records nanoseconds.

### Difference From Traditional Dependency Generation

Not a flaw, just a fact: usual dependency generation techniques such as
gcc -M restrict themselves to a given language but file-level auditing
records every file read or written. Thus it may include not just .h
files but makefiles, shell scripts, etc. This is generally considered
a feature.

### Tricky Shell Scripting

Assuming GNU make here: many recipes, in particular those generated by
automake, are complex and stateful. Some may present corner cases which
require careful and holistic thought aka "what in the shell is going
on here?". In particular .ONESHELL may be needed (vide infra).

# EXAMPLES

A good sample use is building GNU make itself (which already generates
its own dependency data but we're going to ignore that). First we unpack
and configure GNU make version 4.2.1 and use "make clean" to be sure
its state is fresh.

## Black Box Auditing

A complete black-box audit from the top:

% pmaudit --json pmaudit.json -- make > /dev/null

This leaves us with a little JSON database:

% wc pmaudit.json
  493   980 38058 pmaudit.json

Which we can query to see that 115 files were involved of which 56 were
prerequisites, 58 intermediates, and 1 final target which is of course the
executable file "make":

% pmaudit pmaudit.json --all | wc
115     115    1312

% pmaudit pmaudit.json --prerequisites | wc
56      56     592

% pmaudit pmaudit.json --intermediates | wc
58      58     715

% pmaudit pmaudit.json --final
make

## Per-Target Auditing

Alternatively (and after another make clean) we can build with the
auditor inserted on a per-recipe basis by overriding the shell:

% make --eval=.ONESHELL: SHELL=pmash .SHELLFLAGS='-d $@.d -c' > make.log 2>&1

This will run each recipe using "pmash -d [target].d -c 'recipe'".
Here's one of the dependency files it generates:

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

Compare this with GNU make's native generated deps file (.deps/job.Po)
which contains data for system files as well.

Let's break down the command line above. SHELL=pmash is used to force
make to use the auditor as a shell wrapper and .SHELLFLAGS controls
the flags passed to it; in particular pmash takes the usual -c string
and passes it to the shell. It also accepts "-d $@.d" to send derived
prereq data to foo.d when building foo.

The use of .ONESHELL is needed to cause build activity for each target
to take place in a single shell process. Otherwise, since each recipe
line is a different shell and they share the same value of $@ the
output file would be overwritten. Again GNU make is a good example.
Its recipe for each object file looks like this (simplified):

    gcc -MT job.o -MD -MP -MF .deps/job.Tpo -c -o job.o job.c
    mv -f .deps/job.Tpo .deps/job.Po

Without .ONESHELL the mv command would run last and in its own shell
so jobs.o.d would end up recording only the actions of mv, not gcc.

## Per-Target Detailed Auditing with Pmaudit

Let's say that you want to record detailed information about what was
read and written for each make target. Here's one way to do that:

% make --eval=.ONESHELL: SHELL=pmaudit .SHELLFLAGS='--multiline --json $@.json -c'

Here we are using pmaudit, which currently provides more options than
pmash, as the shell wrapper.  We use its --multiline option to more
accurately reflect how make normally runs commands (normally make runs a
line at a time and ends the recipe if any of them fail).  We also use
the --json option which records  more information than --depsfile.

[*] With apologies for the implied classism and sexism :-)
