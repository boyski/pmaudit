# "Poor Man's Audit"[*]

[*] With apologies for the implied classism and sexism :-)

This project comprises two tools which act as build auditors. A build
auditor is a tool which runs a software build (or really any process
which reads a set of input files and produces a set of output files)
and afterward can report which files were which. In other words an
audit can tell you the list of files consumed by the build, those
created during it, those which were ignored by it, etc.

Most build-auditing (aka file auditing) tools are complex and expensive
(in both senses of the word). They may involve a custom filesystem or
rely on ptrace (essentially running audited processes in the debugger).
This tool is at the other end of the spectrum: it's very simple and
unambitious, relying only on standard Unix filesystem semantics, and with
minimal performance cost (aside from the loss of parallelism, see below).

One tool (pmaudit) is written in Python, the other (pmash) is a C program.
Naturally pmash is much faster but pmaudit is more flexible. Either can
be used as a shell wrapper. See details below.

Invoke them with --help for detailed explanation, usage, and examples.

## Uses

What are some of the uses of build auditing? Here are a few examples.

### Cleanup

Say you have a huge number of files in a source code repository (svn,
git, etc). After many changes and refactorings over the years you no
longer know which files are still in use. A build audit can tell you
this so you can prune the rest.

### Streamlined Checkouts

Similar to above: if you know the list of files needed to build project A,
even if other files must be preserved for building projects B, C, etc, it
may be helpful to check out only the subset needed by A for bandwidth or
disk space reasons. Auditing can tell you that subset.

### Source Packaging

Imagine you have a proprietary code base and contracts with various
customers to provide source code for what you sell them. Not every
customer gets every jewel: customer A has a contractual right to source
for products X and Y while customer B gets source for product Z.  If the
build of a product produces the list of files involved in making it,
that list can be used with tar or zip to make up a source package
guaranteed to be a minimal but complete set.

### Dependency Analysis

If we know the exact set of files required to build a particular object
file we can pass it to "make" or similar, creating a complete dependency
graph, resulting in more reliable and/or parallelizable builds.

## What Could Go Wrong?

Are there flaws in this system? Because of its radical simplicity and
lack of ambition there isn't room for too many, but here are those I
can think of:

### Interference from another process

If some unrelated process comes over and opens files in the audit area
during the run the results will of course be contaminated. So don't
do that.

### Cannot support parallelism

Because this technique involves snapshotting the states of files before
and after, parallel processes would interfere with it. This is really
just the same as above with the interference coming from within.

### Permission problems

Due to the necessity of updating access times (atimes) you may need
to own the input files. In any build scenario you'd need the right
to write output files anyway, and of course you'd own those, so this
basically extends the ownership requirement to prerequisite files.

### Unseen files

The script needs to know which files to monitor and files in an
unmonitored area will not be recorded. This can be seen as a feature or
a bug. For instance, if compiling a collection of .c and .h files do
you want it to record /usr/include/stdio.h as one of the prereqisites
or is that TMI? Regardless, it will only record accesses to files in
monitored locations.

### Inability to update atimes

This is a big one but there's a test to detect it. It's really a system
admin issue, not something the script can deal with, so it just tries
to fail when atimes aren't behaving.

### Weak granularity of file timestamps

This is a common issue. Many filesystems still record only seconds
or milliseconds and the resulting roundoff errors can lead to bogus
results. Best to use it on high-resolution filesystems (such as Linux
ext4 which records nanoseconds).

## Tools

There are two different tools here which rely on the same basic technique
while having slightly different functions. Either can be used as a SHELL
replacement in make though of course the C program will be much faster.
When used as shell wrappers they dump prerequisite data in "make" format.

### pmaudit

This is a Python script which may be used from the top level of a build.
It derives a complete "database" of all files accessed (and not accessed)
during the build and categorizes them as prerequisites, targets, etc
as described previously. This database is useful for many things but
it does not have data with sufficient granularity to tell you which
prereqs were required by which targets.

It can be used as a shell wrapper.

### pmash

This is a C program which operates as a wrapper over the shell. Due
to being written in C it's much faster than pmaudit but more limited.
It derives only per-target prerequisite data.

#### pmamake

This is a tiny shell wrapper which exists to document ways by which
either tool can be introduced into a GNU make build. It's intended
solely as a demo.
