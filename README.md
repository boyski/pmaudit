"Poor Man's Audit"[*] (with apologies for the implied sexism and classism :-)

This is a script to do build auditing. A build auditor is a tool which
runs a software build (or really any process that reads a set of input
files and produces a set of output files) and afterward can report which
files were which. In other words the audit would tell you which files were
used during the build, which were produced, which were ignored, etc.

Most build-auditing (aka file auditing) tools are complex and expensive
(in both senses of the word). They may involve a custom filesystem or rely
on ptrace (essentially running audited processes in the debugger).  This
script is at the other end of the spectrum: it's very simple and
unambitious, relies only on standard Unix filesystem semantics, and has
essentially no performance cost.

Invoke the script with --help for a more detailed explanation.
