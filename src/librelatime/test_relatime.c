#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

int main() {
    const char *path = "tst_relatime.txt";
    int fd;
    struct timespec ts[2] = {UTIME_NOW, UTIME_NOW, UTIME_OMIT, UTIME_OMIT};
    struct stat st;
    const char *data = "Hello, World!\n";

    // Create or open the test file.
    fd = creat(path, 0644);
    if (fd == -1) {
        perror(path);
        return 1;
    }

    if (clock_gettime(CLOCK_REALTIME, &ts[0]) == -1) {
        perror(path);
        return 1;
    } else{
	ts[1].tv_sec = ts[0].tv_sec;
	ts[1].tv_nsec = ts[0].tv_nsec;
    }
    if (futimens(fd, ts) == -1) {
        perror(path);
        return 1;
    }
    if (fsync(fd) == -1) {
	perror(path);
    }
    (void)close(fd);

    if (stat(path, &st) == 0) {
        printf("Pre: %s\n", path);
        printf("  Access time:  %s", ctime(&st.st_atime));
        printf("  Modify time:  %s", ctime(&st.st_mtime));
        printf("  Change time:  %s", ctime(&st.st_ctime));
    }

    // Open the test file again.
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        perror(path);
        return 1;
    }

    // Write data (this will be intercepted if LD_PRELOAD is used)
    if (write(fd, data, strlen(data)) == -1) {
        perror(path);
        close(fd);
        return 1;
    }

    close(fd);

    if (stat(path, &st) == 0) {
        printf("Post: %s\n", path);
        printf("  Access time:  %s", ctime(&st.st_atime));
        printf("  Modify time:  %s", ctime(&st.st_mtime));
        printf("  Change time:  %s", ctime(&st.st_ctime));
    }

    return 0;
}
