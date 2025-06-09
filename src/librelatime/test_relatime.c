#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

int main() {
    const char *testfile = "tst_relatime.txt";
    int fd;
    struct stat st;
    const char *data = "Hello, World!\n";

    // Create or open the test file.
    fd = open(testfile, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd == -1) {
        perror(testfile);
        return 1;
    }

    // Write data (this will be intercepted if LD_PRELOAD is used)
    if (write(fd, data, strlen(data)) == -1) {
        perror(testfile);
        close(fd);
        return 1;
    }

    close(fd);

    if (stat(testfile, &st) == 0) {
        printf("File: %s\n", testfile);
        printf("  Access time:  %s", ctime(&st.st_atime));
        printf("  Modify time:  %s", ctime(&st.st_mtime));
        printf("  Change time:  %s", ctime(&st.st_ctime));
    }

    return 0;
}
