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
#include <regex.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

typedef struct {
    const char *path;
    struct timespec times[2];
} pathtimes_s;

static char prog[PATH_MAX] = "??";
static void *stash;

static char **argv_;

#define MDSH_CMDRE "MDSH_CMDRE"
#define MDSH_DBGSH "MDSH_DBGSH"
#define MDSH_EFLAG "MDSH_EFLAG"
#define MDSH_XTEVS "MDSH_XTEVS"
#define MDSH_PS1 "MDSH>> "
#define MDSH_PATHS "MDSH_PATHS"
#define MDSH_TIMING "MDSH_TIMING"
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
This program execs %s and passes its argv directly to it\n\
without parsing. It prints this usage message with -h or\n\
--help but in all other ways it's a pass-through to %s\n\
and thus behaves exactly the same. All its value-added\n\
comes from the env variables listed below which can trigger\n\
pre- and post-actions. The idea is that setting GNU make's\n\
SHELL=%s along with some subset of the environment variables\n\
listed below may help diagnose complex make problems.\n",
    prog, SHELL, SHELL, prog);

    fprintf(f, "\n\
The variable MDSH_PATHS is a colon-separated list of glob patterns\n\
representing paths to keep an eye on and report when the shell\n\
process has changed any of their states (created, removed,\n\
written, or accessed/read). The intention is that setting GNU\n\
make's SHELL=%s will allow it to tell us whenever a file we're\n\
interested in changes.\n",
    prog);

    fprintf(f, "\n\
If the MDSH_VERBOSE variable is set (nonzero) the command line\n\
will be printed along with each MDSH_PATHS change.\n");

    fprintf(f, "\n\
If MDSH_XTRACE is set the shell command will be printed as\n\
with 'set -x'.\n");

    fprintf(f, "\n\
MDSH_TIMING is similar to MDSH_XTRACE but the command is\n\
printed after it finishes along with the time it took.\n");

    fprintf(f, "\n\
If a regular expression is supplied with MDSH_CMDRE it will be\n\
compared against the shell command. If a match is found an\n\
interactive debug shell will be invoked before the command runs.\n");

    fprintf(f, "\n\
If the underlying shell process exits with a failure status and\n\
MDSH_DBGSH is set, %s will run an interactive shell to help\n\
analyze the failing state.\n",
    prog);

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
\n\
$ MDSH_TIMING=1 mdsh -c 'sleep 2.4'\n\
- mdsh -c sleep 2.4 (2.4s)\n\
\n\
Real-life usage via make:\n\n\
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
                    perror(path);
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
xtrace(int argc, char *argv[], const char *pfx, const char *timing)
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

    fputs(pfx ? pfx : "+ ", stderr);
    for (i = 0; i < argc; i++) {
        // The handling of whitespace and quoting here is rudimentary
        // but it's only for visual purposes. No commitment is made
        // that output can be safely fed back to the shell.
        if (strchr(argv[i], ' ') || strchr(argv[i], '\t')) {
            fprintf(stderr, "'%s'", argv[i]);
        } else {
            fputs(argv[i], stderr);
        }
        if (i < argc - 1) {
            fputc(' ', stderr);
        }
    }
    if (timing) {
        fprintf(stderr, " (%s)", timing);
    }
    fputc('\n', stderr);
    insist(!fflush(stderr), "fflush(stderr)");
}

void
dbgsh(int argc, char *argv[])
{
    static int done;

    if (!done++) {
        pid_t pid;

        xtrace(argc, argv, NULL, NULL);
        insist((pid = fork()) >= 0, "fork()");
        if (!pid) {  // In the child.
            int fd;
            // GNU make with -j tends to close stdin, and stdout/stderr might
            // be redirected too.
            for (fd = 0; fd < 3; fd++) {
                if (!isatty(fd)) {
                    (void)close(fd);
                    insist(open("/dev/tty", fd ? O_WRONLY : O_RDONLY) == 0,
                           "open(/dev/tty)");
                }
            }
            insist(!setenv("PS1", MDSH_PS1, 1), NULL);
            (void)execlp(basename(SHELL), SHELL, "--norc", "-i", (char *)NULL);
            perror(SHELL); // NOTREACHED
        }
        // Ignore the exit status of this debugging shell.
        insist(wait(NULL) != -1, "wait()");
    }
}

int
main(int argc, char *argv[])
{
    int rc = EXIT_SUCCESS;
    char *watch, *pattern;
    struct timeval pretime;

    argv_ = argv; // Hack to preserve command line for later verbosity.

    (void)strncpy(prog, basename(argv[0]), sizeof(prog));
    prog[sizeof(prog) - 1] = '\0';

    if (!strcmp(argv[argc - 1], "-h") || !strcmp(argv[argc - 1], "--help")) {
        usage(0);
    }

    if (ev2int(MDSH_XTRACE)) {
        xtrace(argc, argv, NULL, NULL);
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

    if (getenv(MDSH_CMDRE)) {
        size_t i;
        regex_t re;

        insist(regcomp(&re, getenv(MDSH_CMDRE), REG_EXTENDED) == 0, "regcomp()");
        for (i = 1; argv[i]; i++) {
            if (argv[i - 1][0] == '-' && strchr(argv[i - 1], 'c')) {
                if (!regexec(&re, argv[i], 0, NULL, 0)) {
                    dbgsh(argc, argv);
                    break;
                }
            }
        }
        regfree(&re);
    }

    if (ev2int(MDSH_TIMING)) {
        insist(!gettimeofday(&pretime, NULL), "gettimeofday(&pretime, NULL)");
    }

    // Fork, exec, and wait for the shell.
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

    if (ev2int(MDSH_TIMING)) {
        struct timeval endtime;
        char tbuf[256];
        double delta;

        insist(!gettimeofday(&endtime, NULL), "gettimeofday(&endtime, NULL)");
        delta = ((endtime.tv_sec * 1000000.0) + endtime.tv_usec) -
                ((pretime.tv_sec * 1000000.0) + pretime.tv_usec);
        (void)snprintf(tbuf, sizeof(tbuf), "%.1fs", delta / 1000000.0);
        xtrace(argc, argv, "- ", tbuf);
    }

    // Revisit the original list of files and report any changes.
    twalk(stash, watch_walk);

    if (rc != EXIT_SUCCESS) {
        if (ev2int(MDSH_DBGSH)) {
            dbgsh(argc, argv);
        }

        if (ev2int(MDSH_EFLAG)) {
            fprintf(stderr, "kill -INT %d\n", getppid());
            (void)kill(getppid(), SIGINT);
        }
    }

    return rc;
}

// vim: ts=8:sw=4:tw=80:et:
