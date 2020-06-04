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

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
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
static int verbose;

#define MDSH_CMDRE "MDSH_CMDRE"
#define MDSH_DBGSH "MDSH_DBGSH"
#define MDSH_EFLAG "MDSH_EFLAG"
#define MDSH_PRE_FLUSH_PATHS "MDSH_PRE_FLUSH_PATHS"
#define MDSH_POST_FLUSH_PATHS "MDSH_POST_FLUSH_PATHS"
#define MDSH_HTTP_SERVER "MDSH_HTTP_SERVER"
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
This program execs the shell and passes its argv directly to it\n\
unparsed. It prints this usage message with -h or --help\n\
but in all other ways it's a pass-through to the shell and\n\
thus behaves exactly the same. Its only value-added comes\n\
from the env variables listed below which can trigger pre-\n\
and post-actions. The idea is that setting GNU make's\n\
SHELL=%s along with some subset of the environment variables\n\
listed below may help diagnose complex make problems.\n",
    prog, prog);

    fprintf(f, "\n\
The variable MDSH_PATHS is a colon-separated list of glob patterns\n\
representing paths to keep an eye on and report when the shell\n\
process has changed any of their states (created, removed,\n\
written, or accessed/read).\n");

    fprintf(f, "\n\
If the MDSH_VERBOSE variable is set (nonzero) the command line\n\
will be printed along with each MDSH_PATHS change message.\n");

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
However, be aware that starting an interactive debug shell can\n\
run into trouble in -j mode which sometimes closes stdin. Such a\n\
shell requires stdin and stdout to be available to the terminal.\n");

    fprintf(f, "\n\
MDSH_PRE_FLUSH_PATHS and MDSH_POST_FLUSH_PATHS are colon-separated\n\
lists of paths on which to attempt NFS cache-flushing before or after\n\
the recipe runs. The first thing done with each listed path, if it's\n\
a directory, is to open and close it. This may flush the filehandle\n\
cache according to http://tss.iki.fi/nfs-coding-howto.html.\n");

    fprintf(f, "\n\
If MDSH_HTTP_SERVER is passed it should be the name of an HTTP server\n\
with read access to listed files. A GET request will be issued for each\n\
path on MDSH_PRE_FLUSH_PATHS whether file or directory. This is said to\n\
force all dirty NFS caches for that path to be flushed.\n");

    fprintf(f, "\n\
NFS cache flushing is a very complex topic and the situation varies by\n\
protocol (NFSv3 vs v4 etc), NFS server vendor, etc. Multiple flushing\n\
techniques are supported and both 'pull' (flush before reading) and 'push'\n\
(flush after writing) models are supported to allow experimental tuning.\n");

    fprintf(f, "\n\
A hypothetical linker recipe could flush the directory containing object\n\
files to make sure they're all present before it starts linking by\n\
setting MDSH_PRE_FLUSH_PATHS=$(@D), for instance. Or $^ could be flushed.\n\
Generally we think pull is more correct than push but having a compile\n\
recipe, say, use MDSH_POST_FLUSH_PATHS=$@ to push-flush the .o may be\n\
worth experimenting with too.\n");

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
(no state change messages, the files are already gone)\n\
\n\
$ MDSH_TIMING=1 mdsh -c 'sleep 2.4'\n\
- mdsh -c sleep 2.4 (2.4s)\n\
\n\
Real-life usage via make:\n\n\
$ MDSH_PATHS=foo %s=1 make SHELL=mdsh\n\
\n\
$ make SHELL=mdsh %s=1\n\
",
    MDSH_VERBOSE, SHELL, SHELL, MDSH_VERBOSE, MDSH_VERBOSE, MDSH_DBGSH);

    exit(rc);
}

static int
ev2int(const char *ev)
{
    char *val;

    val = getenv(ev);
    return val && *val ? atoi(val) : 0;
}

static void
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
    char *mlev;

    if (verbose && (mlev = getenv("MAKELEVEL"))) {
        fprintf(stderr, "%s: [%s] %s %s: %s", prog, mlev, MARK, change, path);
    } else {
        fprintf(stderr, "%s: %s %s: %s", prog, MARK, change, path);
    }

    if (verbose) {
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

static void
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

static void
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

/*
 * As I understand it, when a change is made to file or directory X on
 * host A the client may choose to cache anything (data or metadata)
 * but it always makes one synchronous round trip communication to
 * the server to say "Hey, I've got a dirty cache for X" so the server
 * will always know about the caching. Because of that, when a request
 * for X comes in on host B the server will go back to host A and say
 * "Give me what you've got" before responding to B. Thus, all cached
 * results on any other host are guaranteed to be flushed to the server
 * before the response to B.
 *
 * To make use of this we can rely on an HTTP 1.1 web server which has
 * read access to all of NFS and runs on a dedicated machine and will
 * therefore fulfill the requirements of a "host B" for any "host A".
 */
static int
http_request(const char *server, const char *path)
{
    struct addrinfo *result, hints;
    struct stat stbuf;
    int retval, srvfd, count;
    char *abspath, *slash, *request;
    char readbuf[1024];

    if (!server) {
        return 0;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((retval = getaddrinfo(server, "80", &hints, &result))) {
        fprintf(stderr, "%s: Error: %s: %s\n", prog, server, gai_strerror(retval));
        return EXIT_FAILURE;
    }

    if ((srvfd = socket(result->ai_family, SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        return EXIT_FAILURE;
    }

    if ((connect(srvfd, result->ai_addr, result->ai_addrlen) == -1)) {
        perror("connect()");
        return EXIT_FAILURE;
    }

    insist((abspath = realpath(path, NULL)) != NULL, "realpath(path)");
    slash = (!stat(abspath, &stbuf) && S_ISDIR(stbuf.st_mode)) ? "/" : "";
    if (asprintf(&request,
        "GET %s%s HTTP/1.1\nHost: %s\nUser-agent: %s\nRange: bytes=0-%lu\n\n",
            abspath, slash, server, prog, sizeof(readbuf) - 1) == -1) {
        perror("asprintf()");
        return EXIT_FAILURE;
    }
    free(abspath);

    if (verbose) {
            size_t len = verbose > 1 ? strlen(request) :
                (size_t)(strchr(request, '\n') - request) + 1;
            (void)fwrite(request, sizeof(char), len, stderr);
    }

    if (write(srvfd, request, strlen(request)) == -1) {
        perror("write()");
        return EXIT_FAILURE;
    }

    if (shutdown(srvfd, SHUT_WR) == -1) {
        perror("shutdown()");
        return EXIT_FAILURE;
    }

    // We don't really need to read the whole file - it should be
    // enough to make and satisfy any read request. If the first
    // line doesn't look like "206 Partial-Content" we dump the
    // buffer as an ersatz error message.
    if ((count = read(srvfd, readbuf, sizeof(readbuf))) == -1) {
        if (write(1, readbuf, count) == -1) {
            perror("read()");
            return EXIT_FAILURE;
        }
    } else {
        char *nl, *ok;

        nl = strchr(readbuf, '\n');
        ok = strstr(readbuf, " 206 ");
        if (!nl || !ok || ok > nl) {
            fputs(readbuf, stderr);
            return EXIT_FAILURE;
        }
    }

    if (close(srvfd) == -1) {
        perror("close()");
        return EXIT_FAILURE;
    }

    free(request);

    return 0;
}

static void
create_remove(const char *path)
{
    char *tmpf;
    int fd;

    if (asprintf(&tmpf, "%s/.nfs_flush-%d.tmp", path, (int)getpid()) == -1) {
        perror("asprintf");
    } else {
        if (verbose) {
            fprintf(stderr, "create(%s)\n", tmpf);
        }
        if ((fd = open(tmpf, O_CREAT | O_EXCL, 0666)) == -1) {
            fprintf(stderr, "creat(%s): %s\n", tmpf, strerror(errno));
        } else {
            if (verbose) {
                fprintf(stderr, "remove(%s)\n", tmpf);
            }
            if (close(fd) == -1) {
                fprintf(stderr, "close(%s): %s\n", tmpf, strerror(errno));
            }
            if (unlink(tmpf) == -1) {
                fprintf(stderr, "unlink(%s): %s\n", tmpf, strerror(errno));
            }
        }

        free(tmpf);
    }
}

static int
nfs_flush_dir(const char *path)
{
    DIR *odir;

    // Rather than check whether it's a directory, just run opendir
    // and let it fail if not.
    if ((odir = opendir(path))) {
        if (verbose) {
            fprintf(stderr, "opendir(\"%s\")\n", path);
            fprintf(stderr, "closedir(\"%s\")\n", path);
        }
        (void)closedir(odir);

        // Create and remove a temp file to tickle the filehandle cache.
        create_remove(path);

        return 0;
    }

    return 1;
}

// NFS-flush each path (file or directory) on the specified path.
static int
nfs_flush(const char *ev)
{
    char *paths;

    if ((paths = getenv(ev))) {
        const char *path;
        char *http_server;

        http_server = getenv(MDSH_HTTP_SERVER);

        insist((paths = strdup(paths)) != NULL, "strdup(paths)");
        for (path = strtok(paths, SEP); path; path = strtok(NULL, SEP)) {
            DIR *odir;

            nfs_flush_dir(path);

            (void)http_request(http_server, path);

            /* Flush the immediate subdirs of each dir. */
            if ((odir = opendir(path))) {
                struct dirent *dp;
                char *tpath;

                while ((dp = readdir(odir))) {
                    if (!strcmp(dp->d_name, ".git") || !strcmp(dp->d_name, ".svn")) {
                        // Ignore obvious SCM/VCS subdirectories.
                    } else if (dp->d_name[0] == '.') {
                        // Ignore all "dot" files, unlikely to be used in a build.
                    } else if (strcmp(dp->d_name, "..") && strcmp(dp->d_name, ".")) {
                        if (asprintf(&tpath, "%s/%s", path, dp->d_name) == -1) {
                            perror("asprintf");
                            continue;
                        }

                        nfs_flush_dir(tpath);

                        (void)http_request(http_server, tpath);

                        free(tpath);
                    }
                }

                (void)closedir(odir);
            }

            (void)free(paths);
        }
    }

    return 0;
}

int
main(int argc, char *argv[])
{
    int rc = EXIT_SUCCESS;
    char *watch, *pattern;
    struct timeval pretime;

    argv_ = argv; // Hack to preserve command line for later verbosity.
    verbose = ev2int(MDSH_VERBOSE); // Global verbosity flag.

    (void)strncpy(prog, basename(argv[0]), sizeof(prog));
    prog[sizeof(prog) - 1] = '\0';

    if (!strcmp(argv[argc - 1], "-h") || !strcmp(argv[argc - 1], "--help")) {
        usage(0);
    }

    if (ev2int(MDSH_XTRACE)) {
        xtrace(argc, argv, NULL, NULL);
    }

    // Optionally flush before the recipe.
    (void)nfs_flush(MDSH_PRE_FLUSH_PATHS);

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

    // Optionally flush after the recipe.
    (void)nfs_flush(MDSH_POST_FLUSH_PATHS);

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
