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

#define NOPENFD 20

static char short_opts[] = "c:o:w:";
static struct option long_opts[] = {
   {"command", required_argument, NULL, 'c'},
   {"outfile", required_argument, NULL, 'o'},
   {"watch", required_argument, NULL, 'w'},
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

static void
usage(int rc)
{
    FILE *fp = (rc == EXIT_SUCCESS) ? stdout : stderr;
    const char *fmt = "   %-18s %s\n";

    fprintf(fp, "Usage: %s -c <cmd> -o <outfile> [-w dir[,dir,...]]\n", prog);
    fprintf(fp, fmt, "-h/--help", "Print this usage summary");
    fprintf(fp, fmt, "-c/--command", "Command to invoke");
    fprintf(fp, fmt, "-o/--outfile", "File path to save prereq list");
    fprintf(fp, fmt, "-w/--watch", "Directories to monitor");
    exit(rc);
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

    // Record atimes/mtimes but only after setting atimes behind mtimes
    // for "relatime" reasons.
    p1 = calloc(sizeof(pathentry_s), 1);
    p1->path = strdup(fpath);
    p1->times1[0].tv_sec = sb->st_mtime - 1;
    p1->times1[0].tv_nsec = 0L;
    p1->times1[1].tv_sec = sb->st_mtime;
    p1->times1[1].tv_nsec = sb->st_mtim.tv_nsec;
    if (utimensat(AT_FDCWD, fpath, p1->times1, 0)) {
        perror(fpath);
    }
    if (!tsearch((const void *)p1, &tree1, pathcmp)) {
        perror("tsearch(&pre)");
        return 1;
    }

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
    if (!tsearch((const void *)p2, &tree2, pathcmp)) {
        perror("tsearch(&post)");
        return 1;
    }

    return 0;
}

static void
post_walk(const void *nodep, const VISIT which, const int depth)
{
    pathentry_s *p = *((pathentry_s **)nodep);

    (void)depth;
    if (which == postorder || which == leaf) {
        // If mtime has moved it's a target ...
        if (p->times2[1].tv_sec > p->times1[1].tv_sec) {
            return;
        } else if (p->times2[1].tv_sec == p->times1[1].tv_sec &&
                   p->times2[1].tv_nsec > p->times1[1].tv_nsec) {
            return;
        }
        // ... and if atime has not moved it's unused.
        if (p->times2[0].tv_sec <= p->times1[0].tv_sec) {
            return;
        } else if (p->times2[0].tv_sec == p->times1[0].tv_sec &&
                   p->times2[0].tv_nsec <= p->times1[0].tv_nsec) {
            return;
        }
        // Must be a prereq.
        fprintf(fp, "%s\n", p->path);
    }
}

int
main(int argc, char *argv[])
{
    char *path;
    char *cmdstr = NULL, *outfile = NULL, *watchdirs = ".";
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
            case 'o':
                outfile = optarg;
                break;
            case 'w':
                watchdirs = optarg;
                break;
        }
    }

    if (!cmdstr) {
        usage(EXIT_FAILURE);
    }

    if (outfile) {
        if (!(fp = fopen(outfile, "w"))) {
            perror(outfile);
            exit(EXIT_FAILURE);
        }
    } else {
        fp = stdout;
    }

    for (path = strtok(strdup(watchdirs), ","); path; path = strtok(NULL, ",")) {
        if (nftw(path, nftw_pre_callback, NOPENFD, FTW_MOUNT) == -1) {
            perror(path);
            exit(EXIT_FAILURE);
        }
    }

    if (system(cmdstr)) {
        rc = EXIT_FAILURE;
    }

    for (path = strtok(strdup(watchdirs), ","); path; path = strtok(NULL, ",")) {
        if (nftw(path, nftw_post_callback, NOPENFD, FTW_MOUNT) == -1) {
            perror(path);
            rc = EXIT_FAILURE;
        }
    }

    twalk(tree2, post_walk);

    exit(rc);
}

// vim: ts=8:sw=4:tw=80:et:
