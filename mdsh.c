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
#include <glob.h>
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

static char **argv_;

#define MDSH_DBGSH "MDSH_DBGSH"
#define MDSH_EFLAG "MDSH_EFLAG"
#define MDSH_XTEVS "MDSH_XTEVS"
#define MDSH_PS1 "MDSH>> "
#define MDSH_PATHS "MDSH_PATHS"
#define MDSH_VERBOSE "MDSH_VERBOSE"
#define MDSH_XTRACE "MDSH_XTRACE"

#define MARK "==-=="
#define SEP ":"
#define SHELL "bash"

#define TIME_GT(left, right) ((left.tv_sec > right.tv_sec) || \
        (left.tv_sec == right.tv_sec && left.tv_nsec > right.tv_nsec))

static void
usage(int rc)
{
    FILE *f = (rc == EXIT_SUCCESS) ? stdout : stderr;

    fprintf(f, "\
%s: The 'Make Diagnosis Shell', part of the pmaudit suite.\n\n\
This program execs %s and passes its arguments directly to\n\
it without parsing them. It prints this usage message with -h or\n\
--help but in all other ways it calls through to %s and thus\n\
behaves exactly the same. All its value-add comes from the env\n\
variables listed below which can trigger pre- and post-actions.\n",
    prog, SHELL, SHELL);

    fprintf(f, "\n\
The variable MDSH_PATHS is a colon-separated list of glob patterns\n\
representing paths to keep an eye on and report when the %s\n\
process has changed any of their states (created, removed,\n\
written, or accessed/read). The intention is that setting GNU\n\
make's SHELL=%s will allow it to tell us whenever a file we're\n\
interested in changes.\n",
    SHELL, prog);

    fprintf(f, "\n\
If the %s variable is nonzero the command line will\n\
be printed along with each MDSH_PATHS change.\n",
    MDSH_VERBOSE);

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
$ MDSH_PATHS=foo:bar mdsh -c 'touch foo'\n\
mdsh: ==-== CREATED: foo\n\
\n\
$ MDSH_PATHS=foo:bar mdsh -c 'touch foo bar'\n\
mdsh: ==-== MODIFIED: foo\n\
mdsh: ==-== CREATED: bar\n\
\n\
$ MDSH_PATHS=foo:bar mdsh -c 'grep blah foo bar'\n\
mdsh: ==-== ACCESSED: foo\n\
mdsh: ==-== ACCESSED: bar\n\
\n\
$ MDSH_PATHS=foo:bar %s=1 mdsh -c 'rm -f foo bar'\n\
mdsh: ==-== REMOVED: foo [%s -c rm -f foo bar]\n\
mdsh: ==-== REMOVED: bar [%s -c rm -f foo bar]\n\
\n\
$ MDSH_PATHS=foo:bar %s=1 mdsh -c 'rm -f foo bar'\n\
(no state change, the files are already gone)\n\
\nReal-life usage via make:\n\n\
$ make SHELL=mdsh MDSH_PATHS=foo %s=1\n\
\n\
$ make SHELL=mdsh %s=1\n\
",
    MDSH_VERBOSE, SHELL, SHELL, MDSH_VERBOSE, MDSH_VERBOSE, MDSH_DBGSH);

    exit(rc);
}

void
die(char *msg)
{
    fprintf(stderr, "%s: Error: %s\n", prog, msg);
    exit(EXIT_FAILURE);
}

int
ev2int(const char *ev)
{
    char *val;

    return ((val = getenv(ev)) && *val && atoi(val));
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
report(const char *path, const char *change)
{
    char *vbev, *mlev;
    int vb;

    vbev = getenv(MDSH_VERBOSE);
    vb = vbev && *vbev && strtoul(vbev, NULL, 10);

    if (vb && (mlev = getenv("MAKELEVEL"))) {
        fprintf(stderr, "%s: [%s] %s %s: %s", prog, mlev, MARK, change, path);
    } else {
        fprintf(stderr, "%s: %s %s: %s", prog, MARK, change, path);
    }

    if (vb) {
        char *cwd;
        int i;

        insist((cwd = getcwd(NULL, 0)) != NULL, "getcwd(NULL, 0)");
        fprintf(stderr, " [%s] (%s ", cwd, SHELL);
        for (i = 1; argv_[i]; i++) {
            if (strpbrk(argv_[i], " \t")) {
                fprintf(stderr, "'%s'", argv_[i]);
            } else {
                fputs(argv_[i], stderr);
            }
            if (argv_[i + 1]) {
                fputc(' ', stderr);
            }
        }
        fputc(')', stderr);
    }

    fputc('\n', stderr);
    insist(!fflush(stderr), "fflush(stderr)");
}

static void
watch_walk(const void *nodep, const VISIT which, const int depth)
{
    pathtimes_s *pt = *((pathtimes_s **)nodep);
    struct stat stbuf;
    glob_t refound;
    size_t i;

    (void)depth; // don't need this

    if (which != leaf && which != postorder) {
        return;
    }

    (void)memset(&refound, 0, sizeof(refound));
    switch (glob(pt->path, 0, NULL, &refound)) {
        case 0:
            for (i = 0; i < refound.gl_pathc; i++) {
                char *path = refound.gl_pathv[i];

                if (stat(path, &stbuf) == -1) {
                    if (pt->times[0].tv_sec) {
                        report(path, "DELETED");
                    }
                } else if (!pt->times[0].tv_sec) {
                    report(path, "CREATED");
                } else {
                    if (TIME_GT(stbuf.st_mtim, pt->times[1])) {
                        report(path, "MODIFIED");
                    } else if (TIME_GT(stbuf.st_atim, pt->times[0])) {
                        report(path, "ACCESSED");
                    }
                }
            }
            break;
        case GLOB_NOMATCH:
            if (pt->times[0].tv_sec) {
                report(pt->path, "REMOVED");
            }
            break;
        default:
            insist(0, pt->path);
    }

    globfree(&refound);
}

void
xtrace(int argc, char *argv[])
{
    int i;

    if (getenv(MDSH_XTEVS)) {
        char *evlist, *ev;

        insist((evlist = strdup(getenv(MDSH_XTEVS))) != NULL, "strdup()");
        for (ev = strtok(evlist, SEP); ev; ev = strtok(NULL, SEP)) {
            if (getenv(ev)) {
                fprintf(stderr, "+++ %s=%s\n", ev, getenv(ev));
            }
        }
        (void)free(evlist);
    }

    fputs("+ ", stderr);
    for (i = 0; i < argc; i++) {
        fputs(argv[i], stderr);
        fputc(i < argc - 1 ? ' ' : '\n', stderr);
    }

    insist(!fflush(stderr), "fflush(stderr)");
}

int
main(int argc, char *argv[])
{
    int rc = EXIT_SUCCESS;
    char *watch, *pattern;

    argv_ = argv; // Hack to preserve command line for later verbosity.

    (void)strncpy(prog, basename(argv[0]), sizeof(prog));
    prog[sizeof(prog) - 1] = '\0';

    if (!strcmp(argv[argc - 1], "-h") || !strcmp(argv[argc - 1], "--help")) {
        usage(0);
    }

    if (ev2int(MDSH_XTRACE)) {
        xtrace(argc, argv);
    }

    // Record the state (absence/presence and atime/mtime if present) of files.
    if ((watch = getenv(MDSH_PATHS))) {
        size_t i;
        glob_t found;
        int globflags = GLOB_NOCHECK;

        // Run through the patterns, deriving a list of matched paths.
        (void)memset(&found, 0, sizeof(found));
        insist((watch = strdup(watch)) != NULL, "strdup(watch)");
        for (pattern = strtok(watch, SEP); pattern; pattern = strtok(NULL, SEP)) {
            switch (glob(pattern, globflags, NULL, &found)) {
                case 0:
                case GLOB_NOMATCH:
                    break;
                default:
                    insist(0, pattern);
                    break;
            }
            globflags |= GLOB_APPEND;
        }

        for (i = 0; i < found.gl_pathc; i++) {
            pathtimes_s *pt;
            struct stat stbuf;

            insist((pt = calloc(sizeof(pathtimes_s), 1)) != NULL, "calloc(pathtimes_s)");
            pt->path = strdup(found.gl_pathv[i]);
            if (stat(pt->path, &stbuf) != -1) {
                pt->times[0].tv_sec = stbuf.st_atim.tv_sec;
                pt->times[0].tv_nsec = stbuf.st_atim.tv_nsec;
                pt->times[1].tv_sec = stbuf.st_mtim.tv_sec;
                pt->times[1].tv_nsec = stbuf.st_mtim.tv_nsec;
                // Must push atime behind mtime due to "relatime".
                if (stbuf.st_atim.tv_sec >= pt->times[1].tv_nsec) {
                    pt->times[0].tv_sec = pt->times[1].tv_sec - 2;
                    pt->times[0].tv_nsec = 999;
                    if (utimensat(AT_FDCWD, pt->path, pt->times, 0) == -1) {
                        fprintf(stderr, "%s: Error: %s\n", prog, strerror(errno));
                    }
                }
            } else {
                (void)memset(&stbuf, '\0', sizeof(stbuf));
            }
            insist(tsearch((const void *)pt, &stash, pathcmp) != NULL, "tsearch(&pre)");
        }

        globfree(&found);
        (void)free(watch);
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
    twalk(stash, watch_walk);

    if (rc != EXIT_SUCCESS) {
        if (ev2int(MDSH_DBGSH)) {
            pid_t pid;
            insist((pid = fork()) >= 0, "fork()");
            if (!pid) {  // In the child.
                if (!isatty(0)) {
                    // GNU make with -j tends to close stdin.
                    (void)close(0);
                    insist(open("/dev/tty", O_RDONLY) == 0, "open(/dev/tty)");
                }
                insist(!setenv("PS1", MDSH_PS1, 1), NULL);
                xtrace(argc, argv);
                (void)execlp(basename(SHELL), SHELL, "--norc", "-i", (char *)NULL);
            }
            // We don't care about the exit status of the debugging shell.
            insist(wait(NULL) != -1, "wait()");
        }

        if (ev2int(MDSH_EFLAG)) {
            fprintf(stderr, "kill -INT %d\n", getppid());
            (void)kill(getppid(), SIGINT);
        }
    }

    return rc;
}

// vim: ts=8:sw=4:tw=80:et:
