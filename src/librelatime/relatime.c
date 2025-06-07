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

// Intercepted write() function.
ssize_t write(int fd, const void *buf, size_t count)
{
    static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
    ssize_t result;
    struct stat st;
    struct timespec ts[2];

    // Find the original write function and use it.
    if (!real_write) {
        real_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    }
    result = real_write(fd, buf, count);

    // If write was unsuccessful, just return and let the caller handle it.
    if (result == -1) {
	return result;
    }

    // If write was successful, update access time.
    // Start by getting the current times.
    if (fstat(fd, &st) == -1) {
	perror("fstat()");
	return result;
    }

    // Set atime to exactly one day (86400 seconds) before mtime.
    ts[0].tv_sec = st.st_mtime - 86400;  // atime: mtime - 1 day
    ts[0].tv_nsec = st.st_mtim.tv_nsec;  // Keep same nanoseconds as mtime

    // Leave mtime unchanged.
    ts[1] = st.st_mtim;  // mtime (preserve original)

    // Update file times with nanosecond precision.
    if (futimens(fd, ts) == -1) {
	perror("futimens()"); // Carry on if we can't update times.
    }

    // Temporary debug hack.
    {
	char proc[4096] = {0};
	char path[4096] = {0};

	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	readlink(proc, path, sizeof(path) - 1);
	fprintf(stderr, "=-= moved atime of %s back a day\n", path);
    }

    return result;
}
