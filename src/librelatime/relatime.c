/*
 * The purpose of this library is to support tracking atimes (access times)
 * in NFS file operations. Most modern NFS mounts use the "relatime" option.
 * Brief summary is that updating atimes over a network is expensive and
 * relatime is an optimization: the atime is updated IFF it's older than the
 * mtime (mod time). Thus the atime is updated a maximum of once per file
 * no matter how many reads are made until the file is updated after which
 * the cycle repeats.
 * Relatime is a good feature but it interferes with auditing and analysis
 * of builds. Consider a toy Hello World application which compiles "hello.c"
 * to "hello.o" and then links it to "hello". It may be necessary to reset
 * hello.o's atime back before mtime in order to cause the linker to update
 * its atime. Knowing that hello.o was written-then-read allows us to infer
 * that it's an intermediate target.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

void
adjust_atime(int fd)
{
    struct stat st;
    struct timespec ts[2];

    // Start by getting the current times.
    if (fstat(fd, &st) == -1) {
	perror("fstat()");
	return;
    }

    // Set atime to exactly one day (86400 seconds) before mtime.
    ts[0].tv_sec = st.st_mtime - 86400;  // atime: mtime - 1 day
    ts[0].tv_nsec = st.st_mtim.tv_nsec;  // Keep same nanoseconds as mtime

    // Leave mtime unchanged.
    ts[1] = st.st_mtim;  // mtime (preserve original)

    // Update the file times with nanosecond precision.
    if (futimens(fd, ts) == -1) {
	// perror("futimens()"); // Carry on if we can't update times.
    } else {
	// Temporary debug hack.
	char proc[4096] = {0};
	char path[4096] = {0};

	(void)snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	readlink(proc, path, sizeof(path) - 1);
	fprintf(stderr, "=-= moved atime of %s a day behind mtime\n", path);
    }
}

// Intercepted write() function.
ssize_t
write(int fd, const void *buf, size_t count)
{
    static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
    ssize_t result;

    // Look up the original write function and use it.
    if (!real_write) {
        real_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    }
    result = real_write(fd, buf, count);

    // If successful, update access time.
    if (result != -1) {
	adjust_atime(fd);
    }

    return result;
}
