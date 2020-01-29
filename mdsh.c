/******************************************************************************
 * Copyright (C) 2018-2020 David Boyce
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more detail.
 *
 * You may have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef struct {
    const char *path;
    struct timespec times[2];
} pathtimes_s;

static char prog[PATH_MAX] = "??";
static void *stash;

#define MDSH_DBGSH "MDSH_DBGSH"
#define MDSH_PS1 "MDSH>> "
#define MDSH_WATCH "MDSH_WATCH"
#define MDSH_VERBOSE "MDSH_VERBOSE"
#define MDSH_XTRACE "MDSH_XTRACE"

#define SEP ","
#define SHELL "bash"

#define TIME_GT(left, right) ((left.tv_sec > right.tv_sec) || \
        (left.tv_sec == right.tv_sec && left.tv_nsec > right.tv_nsec))

static void
usage(int rc)
{
    FILE *f = (rc == EXIT_SUCCESS) ? stdout : stderr;

    fprintf(f, "\
%s: The 'Make Diagnosis Shell', part of the pmaudit suite.\n\n\
This program execs bash and passes its arguments directly to\n\
it without parsing them. It prints this usage message with -h or\n\
--help but in all other ways it calls through to bash and thus\n\
behaves exactly the same. All value-add comes from environment\n\
variables listed below which can trigger pre- and post-actions.\n",
    prog);

    fprintf(f, "\n\
The variable %s is a comma-separated list of paths to\n\
keep an eye on and report when the bash process has changed any\n\
of their states (created, removed, written, or accessed/read).\n\
The intention is that setting GNU make's SHELL=%s will allow\n\
it to tell us whenever a file we're interested in changes.\n",
    MDSH_WATCH, prog);

    fprintf(f, "\n\
If the %s variable is nonzero the command line will\n\
be printed along with each %s change.\n",
    MDSH_VERBOSE, MDSH_WATCH);

    fprintf(f, "\n\
If the underlying shell process exits with a failure status and\n\
%s is nonzero, %s will run an interactive shell to help\n\
analyze the failing state.\n",
    MDSH_DBGSH, prog);

    fprintf(f, "\n\
If %s is nonzero the shell command will be printed as\n\
with 'set -x'.\n",
    MDSH_XTRACE);

    fprintf(f, "\n\
EXAMPLES:\n\n\
$ MDSH_WATCH=foo,bar mdsh -c 'touch foo'\n\
mdsh: ==-== CREATED: foo\n\
\n\
$ MDSH_WATCH=foo,bar mdsh -c 'touch foo bar'\n\
mdsh: ==-== MODIFIED: foo\n\
mdsh: ==-== CREATED: bar\n\
\n\
$ MDSH_WATCH=foo,bar mdsh -c 'grep blah foo bar'\n\
mdsh: ==-== ACCESSED: foo\n\
mdsh: ==-== ACCESSED: bar\n\
\n\
$ MDSH_WATCH=foo,bar MDSH_VERBOSE=1 mdsh -c 'rm -f foo bar'\n\
mdsh: ==-== REMOVED: foo [bash -c rm -f foo bar]\n\
mdsh: ==-== REMOVED: bar [bash -c rm -f foo bar]\n\
\n\
$ MDSH_WATCH=foo,bar MDSH_VERBOSE=1 mdsh -c 'rm -f foo bar'\n\
(no state change, the files are already gone)\n\
\nReal-life usage via make:\n\n\
$ make SHELL=mdsh MDSH_WATCH=foo MDSH_VERBOSE=1\n\
");

    exit(rc);
}

void
die(char *msg)
{
    fprintf(stderr, "%s: Error: %s\n", prog, msg);
    exit(EXIT_FAILURE);
}

void
insist(int success, const char *term)
{
    if (success) {
        return;
    }
    if (term && *term) {
        fprintf(stderr, "%s: Error: %s: %s\n", prog, term, strerror(errno));
    } else {
        fprintf(stderr, "%s: Error: %s\n", prog, strerror(errno));
    }
    exit(EXIT_FAILURE);
}

static int
pathcmp(const void *pa, const void *pb)
{
    return strcmp(((pathtimes_s *)pa)->path, ((pathtimes_s *)pb)->path);
}

static void
changed(const char *path, const char *change, char *argv[])
{
    char *vb;

    fprintf(stderr, "%s: ==-== %s: %s", prog, change, path);
    if ((vb = getenv(MDSH_VERBOSE)) && *vb && strtoul(vb, NULL, 10)) {
        int i;

        fprintf(stderr, " [%s ", SHELL);
        for (i = 1; argv[i]; i++) {
            fputs(argv[i], stderr);
            if (argv[i + 1]) {
                fputc(' ', stderr);
            }
        }
        fputc(']', stderr);
    }
    fputc('\n', stderr);
}

void
xtrace(char *xt, int argc, char *argv[])
{
    if (xt && *xt && strtoul(xt, NULL, 10)) {
        int i;

        fputs("+ ", stderr);
        for (i = 0; i < argc; i++) {
            fputs(argv[i], stderr);
            fputc(i < argc - 1 ? ' ' : '\n', stderr);
        }
    }
}

int
main(int argc, char *argv[])
{
    int rc = EXIT_SUCCESS;
    char *watch, *path;

    (void)strncpy(prog, basename(argv[0]), sizeof(prog));
    prog[sizeof(prog) - 1] = '\0';

    if (!strcmp(argv[argc - 1], "-h") || !strcmp(argv[argc - 1], "--help")) {
        usage(0);
    }

    xtrace(getenv(MDSH_XTRACE), argc, argv);

    // Record the state (absence/presence and atime/mtime if present) of files.
    if ((watch = getenv(MDSH_WATCH))) {
        watch = strdup(watch);
        for (path = strtok(watch, SEP); path; path = strtok(NULL, SEP)) {
            pathtimes_s *pt;
            struct stat stbuf;

            insist((pt = calloc(sizeof(pathtimes_s), 1)) != NULL, "calloc(pathtimes_s)");
            pt->path = strdup(path);
            if (stat(path, &stbuf) != -1) {
                pt->times[0].tv_sec = stbuf.st_atim.tv_sec;
                pt->times[0].tv_nsec = stbuf.st_atim.tv_nsec;
                pt->times[1].tv_sec = stbuf.st_mtim.tv_sec;
                pt->times[1].tv_nsec = stbuf.st_mtim.tv_nsec;
                // Must push atime behind mtime due to "relatime".
                if (stbuf.st_atim.tv_sec >= pt->times[1].tv_nsec) {
                    pt->times[0].tv_sec = pt->times[1].tv_sec - 2;
                    pt->times[0].tv_nsec = 999;
                    if (utimensat(AT_FDCWD, path, pt->times, 0) == -1) {
                        fprintf(stderr, "%s: Error: %s\n", prog, strerror(errno));
                    }
                }
            } else {
                (void)memset(&stbuf, '\0', sizeof(stbuf));
            }
            insist(tsearch((const void *)pt, &stash, pathcmp) != NULL, "tsearch(&pre)");
        }
        free(watch);
    }

    // Fork and exec the shell.
    {
        pid_t pid;
        int status = EXIT_SUCCESS;

        insist((pid = fork()) >= 0, "fork()");
        if (!pid) {  // In the child.
            argv[0] = SHELL;
            insist(execvp(basename(SHELL), argv) != -1, "execvp()");
        }
        insist(wait(&status) != -1, "wait()");
        rc = WEXITSTATUS(status);
    }

    // Revisit the original list of files and report any changes.
    if ((watch = getenv(MDSH_WATCH))) {
        for (path = strtok(watch, SEP); path; path = strtok(NULL, SEP)) {
            pathtimes_s px, *py, *pt;

            (void)memset(&px, '\0', sizeof(px)); // unneeded
            px.path = path;
            if ((py = tfind(&px, &stash, pathcmp))) {
                struct stat stbuf;

                pt = *((pathtimes_s **)py);
                if (stat(pt->path, &stbuf) == -1) {
                    if (pt->times[0].tv_sec) {
                        changed(path, "REMOVED", argv);
                    }
                } else if (!pt->times[0].tv_sec) {
                    changed(path, "CREATED", argv);
                } else {
                    if (TIME_GT(stbuf.st_mtim, pt->times[1])) {
                        changed(path, "MODIFIED", argv);
                    } else if (TIME_GT(stbuf.st_atim, pt->times[0])) {
                        changed(path, "ACCESSED", argv);
                    }
                }
            } else {
                fprintf(stderr, "%s: Error: lost path!: %s\n", prog, path);
            }
        }
    }

    if (rc != EXIT_SUCCESS) {
        char *dbg;

        if ((dbg = getenv(MDSH_DBGSH)) && *dbg && strtoul(dbg, NULL, 10)) {
            pid_t pid;
            insist((pid = fork()) >= 0, "fork()");
            if (!pid) {  // In the child.
                if (!isatty(0)) {
                    // GNU make with -j tends to close stdin.
                    (void)close(0);
                    insist(open("/dev/tty", O_RDONLY) == 0, "open(/dev/tty)");
                }
                insist(!setenv("PS1", MDSH_PS1, 1), NULL);
                xtrace("1", argc, argv);
                (void)execlp(basename(SHELL), SHELL, "--norc", "-i", (char *)NULL);
            }
            // We don't care about the exit status of the debugging shell.
            insist(wait(NULL) != -1, "wait()");
        }
    }

    return rc;
}

// vim: ts=8:sw=4:tw=80:et:
