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

// Set atime to exactly two days before mtime.
#define ATIME_DELTA_SECS (86400 * 2)

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

    ts[0].tv_sec = st.st_mtime - ATIME_DELTA_SECS;  // make atime secs older than mtime
    ts[0].tv_nsec = st.st_mtim.tv_nsec;             // keep atime nanos same as mtime
    ts[1].tv_sec = UTIME_OMIT;                      // leave mtime secs unchanged
    ts[1].tv_nsec = UTIME_OMIT;                     // leave mtime nanos unchanged

    // Update the file times with nanosecond precision.
    if (futimens(fd, ts) == -1) {
	// If time update fails just carry on silently.
	// It's probably a system file we don't care about.
	// perror("futimens()");
    } else {
	// Temporary debug hack.
	char proc[4096] = {0};
	char path[4096] = {0};

	(void)snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	readlink(proc, path, sizeof(path) - 1);
	if (*path == '/' && strncmp(path, "/tmp/", 5)) {
	    fprintf(stderr, "=-= moved atime of %s behind mtime\n", path);
	}
    }
}

// Intercepted write() function.
ssize_t
write(int fd, const void *buf, size_t count)
{
    static ssize_t (*real_write)(int fd, const void *buf, size_t count) = NULL;
    ssize_t result;

    // Look up the original write() function and use it.
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

// Intercepted fwrite() function.
size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    static size_t (*real_fwrite)(const void *ptr, size_t size, size_t nmemb, FILE *stream) = NULL;
    size_t result;

    // Look up the original fwrite() function and use it.
    if (!real_fwrite) {
        real_fwrite = (size_t (*)(const void *, size_t, size_t, FILE *))dlsym(RTLD_NEXT, "fwrite");
    }
    result = real_fwrite(ptr, size, nmemb, stream);

    // If successful, update access time.
    if ((ssize_t)result != -1) {
	adjust_atime(fileno(stream));
    }

    return result;
}

// Intercepted fwrite_unlocked() function.
size_t
fwrite_unlocked(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    static size_t (*real_fwrite_unlocked)(const void *ptr, size_t size, size_t nmemb, FILE *stream) = NULL;
    size_t result;

    // Look up the original fwrite_unlocked() function and use it.
    if (!real_fwrite_unlocked) {
        real_fwrite_unlocked = (size_t (*)(const void *, size_t, size_t, FILE *))dlsym(RTLD_NEXT, "fwrite_unlocked");
    }
    result = real_fwrite_unlocked(ptr, size, nmemb, stream);

    // If successful, update access time.
    if ((ssize_t)result != -1) {
	adjust_atime(fileno(stream));
    }

    return result;
}
