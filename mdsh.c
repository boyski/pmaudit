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

#define MDSH_WATCH "MDSH_WATCH"
#define SEP ","
#define SHELL "bash"

#define TIME_GT(left, right) ((left.tv_sec > right.tv_sec) || \
        (left.tv_sec == right.tv_sec && left.tv_nsec > right.tv_nsec))

static void
usage(int rc)
{
    FILE *f = (rc == EXIT_SUCCESS) ? stdout : stderr;

    fprintf(f, "%s: The \"Make Diagnosis Shell\".\n\n", prog);
    fprintf(f, "This program simply execs bash and passes its argv directly to\n");
    fprintf(f, "bash without parsing it. It does not and can not change what\n");
    fprintf(f, "bash does; the only value it adds is to look at the environment\n");
    fprintf(f, "variable %s, a comma-separated list of paths to keep\n", MDSH_WATCH);
    fprintf(f, "an eye on, and report whenever the bash process changes their\n");
    fprintf(f, "state (created, removed, written, or read).\n");
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

int
main(int argc, char *argv[])
{
    int rc = EXIT_SUCCESS;
    char *watch, *path;

    (void)strncpy(prog, basename(argv[0]), sizeof(prog));
    prog[sizeof(prog) - 1] = '\0';

    while (--argc) {
        if (!strcmp(argv[argc], "-h") || !strcmp(argv[argc], "--help")) {
            usage(0);
        }
    }

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
                        fprintf(stderr, "%s: =-= REMOVED: %s\n", prog, path);
                    }
                } else if (!pt->times[0].tv_sec) {
                    fprintf(stderr, "%s: =-= CREATED: %s\n", prog, path);
                } else {
                    if (TIME_GT(stbuf.st_mtim, pt->times[1])) {
                        fprintf(stderr, "%s: =-= MODIFIED: %s\n", prog, path);
                    } else if (TIME_GT(stbuf.st_atim, pt->times[0])) {
                        fprintf(stderr, "%s: =-= ACCESSED: %s\n", prog, path);
                    }
                }
            } else {
                fprintf(stderr, "%s: Error: lost path!: %s\n", prog, path);
            }
        }
    }

    return rc;
}

// vim: ts=8:sw=4:tw=80:et:
