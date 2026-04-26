#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

int _close(int fd) {
    (void)fd;
    errno = EBADF;
    return -1;
}

int _fstat(int fd, struct stat *st) {
    (void)fd;
    if (st) {
        st->st_mode = S_IFCHR;
    }
    return 0;
}

int _isatty(int fd) {
    (void)fd;
    return 1;
}

int _lseek(int fd, int ptr, int dir) {
    (void)fd;
    (void)ptr;
    (void)dir;
    return 0;
}

int _read(int fd, char *ptr, int len) {
    (void)fd;
    (void)ptr;
    (void)len;
    return 0;
}

int _write(int fd, const char *ptr, int len) {
    (void)fd;
    (void)ptr;
    return len;
}
