# "Poor Man's Audit"[*]

[*] With apologies for the implied classism and sexism :-)

This is a script to do build auditing. A build auditor is a tool which
runs a software build (or really any process that reads a set of input
files and produces a set of output files) and afterward can report which
files were which. In other words an audit can tell you which files were
consumed by the build, which were created during it, which were ignored by
it, etc.

Most build-auditing (aka file auditing) tools are complex and expensive
(in both senses of the word). They may involve a custom filesystem or rely
on ptrace (essentially running audited processes in the debugger).  This
script is at the other end of the spectrum: it's very simple and
unambitious, relies only on standard Unix filesystem semantics, and has
essentially no performance cost.

Invoke the script with --help for a more detailed explanation, usage, and
examples.

# Uses

What are some of the uses of build auditing? Here are a few examples.

## Cleanup

Say you have a huge number of files in a source code repository (svn, git,
etc). After many changes and refactorings over the years you no longer
know which files are still needed. A build audit can tell you this so you
can prune the rest.

## Streamlined Checkouts

Similar to above: if you know the list of files needed to build project A,
even if other files must be preserved for building projects B, C, etc, it
may be helpful to check out only the subset needed by A for bandwidth or
disk space reasons.

## Source Packaging

Imagine you have a proprietary code base and deals with various customers
to provide source code for what you sell them. Not every customer gets
every jewel: customer A has a contractual right to source for products X
and Y while customer B gets source for product Z.  If the build of a
product produces the list of files involved in making it, that list can be
used with tar or zip to make up a source package.

## Dependency Analysis

If we know the exact set of files required to build a particular object
file we can pass it to "make" or similar, creating a perfect dependency
graph, resulting in more reliable and/or parallelizable builds.
