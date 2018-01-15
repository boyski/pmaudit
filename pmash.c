/******************************************************************************
 * Copyright (C) 2010-2018 David Boyce
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

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <libgen.h>
#include <search.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#define TMFMT "a1=%010ld.%09ld m1=%010ld.%09ld a2=%010ld.%09ld m2=%010ld.%09ld"

#define NOPENFD 20

static char short_opts[] = "c:eo:VW:";
static struct option long_opts[] = {
   {"command", required_argument, NULL, 'c'},
   {"errexit", no_argument, NULL, 'e'},
   {"outfile", required_argument, NULL, 'o'},
   {"verbose", no_argument, NULL, 'V'},
   {"watch", required_argument, NULL, 'W'},
   {"help", no_argument, NULL, 'h'},
   {NULL, 0, NULL, 0}
};

static const char *prog = "??";

typedef struct {
    const char *path;
    struct timespec times1[2];
    struct timespec times2[2];
} pathentry_s;

static void *tree1, *tree2;

static FILE *fp;
static char *outfile;
static unsigned verbosity;
static unsigned prq_count;

static void
usage(int rc)
{
    FILE *fp = (rc == EXIT_SUCCESS) ? stdout : stderr;
    const char *fmt = "   %-18s %s\n";

    fprintf(fp, "Usage: %s -c <cmd> [-o <outfile>] [-W dir[,dir,...]]\n", prog);
    fprintf(fp, fmt, "-h/--help", "Print this usage summary");
    fprintf(fp, fmt, "-c/--command", "Command to invoke");
    fprintf(fp, fmt, "-e/--errexit", "Exit on first error");
    fprintf(fp, fmt, "-o/--outfile", "File path to save prereq list");
    fprintf(fp, fmt, "-V/--verbose", "Bump verbosity mode");
    fprintf(fp, fmt, "-W/--watch", "Directories to monitor (default='.')");
    fprintf(fp, "\nEXAMPLES:\n\n");
    fprintf(fp, "Compile foo.o leaving prereq data in foo.o.d:\n\n");
    fprintf(fp, "    %s -c 'gcc -c foo.c' -o foo.o.d\n", prog);
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
    if (!success) {
        fprintf(stderr, "%s: Error: %s: %s\n", prog, term, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static int
pathcmp(const void *pa, const void *pb)
{
    return strcmp(((pathentry_s *)pa)->path, ((pathentry_s *)pb)->path);
}

static int
nftw_pre_callback(const char *fpath, const struct stat *sb,
        int tflag, struct FTW *ftwbuf)
{
    pathentry_s *p1;

    (void)ftwbuf; /* unused */

    /* We're only interested in files. */
    if (tflag != FTW_F) {
        return 0;
    }

    if (strstr(fpath, ".git") || strstr(fpath, ".svn") || strstr(fpath, ".swp")) {
        return 0;
    }

    if (fpath[0] == '.' && fpath[1] == '/') {
        fpath += 2;
    }

    // Record atimes/mtimes but only after setting atimes behind mtimes
    // for "relatime" reasons.
    p1 = calloc(sizeof(pathentry_s), 1);
    p1->path = strdup(fpath);
    p1->times1[0].tv_sec = sb->st_mtime - 1;
    p1->times1[0].tv_nsec = 0L;
    p1->times1[1].tv_sec = sb->st_mtime;
    p1->times1[1].tv_nsec = sb->st_mtim.tv_nsec;
    insist(utimensat(AT_FDCWD, fpath, p1->times1, 0) != -1, fpath);
    insist(tsearch((const void *)p1, &tree1, pathcmp) != NULL, "tsearch(&pre)");

    return 0;
}

static int
nftw_post_callback(const char *fpath, const struct stat *sb,
        int tflag, struct FTW *ftwbuf)
{
    const void *px;
    pathentry_s *p1, *p2;

    (void)ftwbuf; /* unused */

    /* We're only interested in files. */
    if (tflag != FTW_F) {
        return 0;
    }

    if (strstr(fpath, ".git") || strstr(fpath, ".svn") || strstr(fpath, ".swp")) {
        return 0;
    }

    if (fpath[0] == '.' && fpath[1] == '/') {
        fpath += 2;
    }

    // Record atimes/mtimes but only after setting atime behind mtime
    // for "relatime" reasons.
    p2 = calloc(sizeof(pathentry_s), 1);
    p2->path = strdup(fpath);
    p2->times1[0].tv_sec = -2L;
    p2->times1[1].tv_sec = -1L;
    p2->times2[0].tv_sec = sb->st_atime;
    p2->times2[0].tv_nsec = sb->st_atim.tv_nsec;
    p2->times2[1].tv_sec = sb->st_mtime;
    p2->times2[1].tv_nsec = sb->st_mtim.tv_nsec;
    if ((px = tfind((const void *)p2, &tree1, pathcmp))) {
        p1 = *((pathentry_s **)px);
        p2->times1[0].tv_sec = p1->times1[0].tv_sec;
        p2->times1[0].tv_nsec = p1->times1[0].tv_nsec;
        p2->times1[1].tv_sec = p1->times1[1].tv_sec;
        p2->times1[1].tv_nsec = p1->times1[1].tv_nsec;
    }
    insist(tsearch((const void *)p2, &tree2, pathcmp) != NULL, "tsearch(&post)");

    return 0;
}

static void
post_walk(const void *nodep, const VISIT which, const int depth)
{
    pathentry_s *p = *((pathentry_s **)nodep);
    int prereq = 0;

    (void)depth;
    if (which == postorder || which == leaf) {
        // If mtime has moved it's a target 
        // and if atime hasn't moved it's unused.
        if (p->times2[1].tv_sec > p->times1[1].tv_sec) {
            // Fall through.
        } else if (p->times2[1].tv_sec == p->times1[1].tv_sec &&
                   p->times2[1].tv_nsec > p->times1[1].tv_nsec) {
            // Fall through.
        } else if (p->times2[0].tv_sec <= p->times1[0].tv_sec) {
            // Fall through.
        } else if (p->times2[0].tv_sec == p->times1[0].tv_sec &&
                   p->times2[0].tv_nsec <= p->times1[0].tv_nsec) {
            // Fall through.
        } else {
            prereq = 1;
        }
        if (prereq) {
            if (outfile) {
                if (prq_count++) {
                    fputs(" \\\n  ", fp);
                } else {
                    const char *c, *e;

                    e = strrchr(outfile, '.');
                    for (c = outfile; c < e; c++) {
                        fputc(*c, fp);
                    }
                    fputs(": \\\n  ", fp);
                }
                fputs(p->path, fp);
            } else {
                fputs(p->path, fp);
                fputc('\n', fp);
            }
        }
    }
}

int
main(int argc, char *argv[])
{
    char *path;
    char *p;
    char *cmdstr = NULL, *watchdirs = ".";
    int eflag = 0;
    int rc = EXIT_SUCCESS;

    prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    while (1) {
        int c;

        c = getopt_long(argc, argv, short_opts, long_opts, NULL);
        if (c == -1)
            break;

        switch (c) {
            case 'h':
                usage(EXIT_SUCCESS);
                break;
            case 'c':
                cmdstr = optarg;
                break;
            case 'e':
                eflag++;
                break;
            case 'o':
                outfile = optarg;
                break;
            case 'V':
                verbosity++;
                break;
            case 'W':
                watchdirs = optarg;
                break;
        }
    }

    if (!cmdstr) {
        usage(EXIT_FAILURE);
    }

    /*
     * It's hard to see how this could ever work in parallel builds
     * so that use is disallowed.
     */
    if ((p = getenv("MAKEFLAGS"))) {
        char *eq = strchr(p, '=');
        char *jf = strstr(p, " -j");
        if (jf && (!eq || jf < eq)) {
            die("not supported in -j mode");
        }
    }

    if (outfile) {
        insist((fp = fopen(outfile, "w")) != NULL, outfile);
    } else {
        fp = stdout;
    }

    for (path = strtok(strdup(watchdirs), ","); path; path = strtok(NULL, ",")) {
        char *tmpf;
        char buf[] = {"data\n"};
        struct stat ostats, nstats;
        struct timespec otimes[2] = {{-1, 0L}, {0, UTIME_OMIT}};
        int fd;

        /*
         * Create, read, and remove a temp file to check that
         * atimes are being updated.
         */
        insist((asprintf(&tmpf, "%s/audit.%ld.tmp", path,
                        (long)getpid())) != -1, "asprintf()");
        insist((fd = open(tmpf, O_CREAT|O_WRONLY|O_EXCL, 0644)) != -1, tmpf);
        insist(write(fd, buf, strlen(buf)) != -1, tmpf);
        insist(fstat(fd, &ostats) != -1, tmpf);
        otimes[0].tv_sec = ostats.st_mtime - 1;
        insist(futimens(fd, otimes) != -1, tmpf);
        insist(close(fd) != -1, tmpf);
        insist((fd = open(tmpf, O_RDONLY)) != -1, tmpf);
        insist(read(fd, buf, sizeof(buf)) != -1, tmpf);
        insist(close(fd) != -1, tmpf);
        insist(stat(tmpf, &nstats) != -1, tmpf);
        insist(unlink(tmpf) != -1, tmpf);
        (void)free(tmpf);
        if (nstats.st_atime < nstats.st_mtime ||
                (nstats.st_atime == nstats.st_mtime &&
                 nstats.st_atim.tv_nsec < nstats.st_mtim.tv_nsec)) {
            die("atimes not updated here");
        }

        insist(nftw(path, nftw_pre_callback, NOPENFD, FTW_MOUNT) != -1, path);
    }

    if (verbosity || getenv("PMASH_VERBOSITY")) {
        if (verbosity > 1) {
            int i;

            fputs("++ ", stderr);
            for (i = 0; i < argc; i++) {
                if (strstr(argv[i], " ")) {
                    fputc('"', stderr);
                    fputs(argv[i], stderr);
                    fputc('"', stderr);
                } else {
                    fputs(argv[i], stderr);
                }
                if (i < (argc - 1)) {
                    fputc(' ', stderr);
                }
            }
            fputc('\n', stderr);
        }
        insist(asprintf(&cmdstr, "set -x; %s", cmdstr) != -1, "asprintf()");
    }

    if (eflag) {
        insist(asprintf(&cmdstr, "set -e; %s", cmdstr) != -1, "asprintf()");
    }

    if (system(cmdstr)) {
        rc = EXIT_FAILURE;
    }

    for (path = strtok(strdup(watchdirs), ","); path; path = strtok(NULL, ",")) {
        insist(nftw(path, nftw_post_callback, NOPENFD, FTW_MOUNT) != -1, path);
    }

    twalk(tree2, post_walk);
    fputc('\n', fp);

    if (outfile) {
        fclose(fp);
        // Don't keep empty deps files around.
        if (!prq_count) {
            insist(unlink(outfile) != -1, outfile);
        }
    }

    exit(rc);
}

// vim: ts=8:sw=4:tw=80:et:
