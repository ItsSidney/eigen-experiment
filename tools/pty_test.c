/* pty_test - exercise /dev/ptmx + /dev/pts/N (open, ioctl, rw, poll). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>

static int fails = 0;
#define CHECK(c, msg) do { \
    if (c) { printf("PASS %s\n", msg); } \
    else { printf("FAIL %s\n", msg); fails++; } \
} while (0)

int main(void) {
    int mfd = open("/dev/ptmx", O_RDWR);
    CHECK(mfd >= 0, "open-ptmx");

    int n = -1;
    CHECK(ioctl(mfd, TIOCGPTN, &n) == 0 && n >= 0, "tiocgptn");

    int lock = 0;
    CHECK(ioctl(mfd, TIOCSPTLCK, &lock) == 0, "tiocsptlck-unlock");

    char path[32];
    snprintf(path, sizeof(path), "/dev/pts/%d", n);
    int sfd = open(path, O_RDWR);
    CHECK(sfd >= 0, "open-slave");

    struct termios tio;
    CHECK(ioctl(sfd, TCGETS, &tio) == 0, "tcgets-slave");
    CHECK(ioctl(mfd, TCGETS, &tio) == 0, "tcgets-master");

    struct winsize ws;
    CHECK(ioctl(sfd, TIOCGWINSZ, &ws) == 0 && ws.ws_col == 80, "tiocgwinsz-default");
    ws.ws_row = 30;
    ws.ws_col = 100;
    CHECK(ioctl(sfd, TIOCSWINSZ, &ws) == 0, "tiocswinsz-set");
    memset(&ws, 0, sizeof(ws));
    CHECK(ioctl(mfd, TIOCGWINSZ, &ws) == 0 && ws.ws_row == 30 && ws.ws_col == 100,
          "tiocgwinsz-back");

    CHECK(write(mfd, "hello-pty", 9) == 9, "write-m2s");
    char buf[16];
    memset(buf, 0, sizeof(buf));
    CHECK(read(sfd, buf, 9) == 9 && memcmp(buf, "hello-pty", 9) == 0, "read-m2s");

    CHECK(write(sfd, "world-pty", 9) == 9, "write-s2m");
    memset(buf, 0, sizeof(buf));
    CHECK(read(mfd, buf, 9) == 9 && memcmp(buf, "world-pty", 9) == 0, "read-s2m");

    struct pollfd pf;
    pf.fd = mfd;
    pf.events = POLLIN;
    CHECK(poll(&pf, 1, 0) == 0, "poll-empty");
    CHECK(write(sfd, "x", 1) == 1, "write-poll");
    CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "poll-ready");
    memset(buf, 0, sizeof(buf));
    read(mfd, buf, 1);

    pf.fd = sfd;
    pf.events = POLLIN;
    CHECK(poll(&pf, 1, 0) == 0, "poll-slave-empty");

    CHECK(isatty(mfd) && isatty(sfd), "isatty");

    close(sfd);
    close(mfd);

    printf(fails ? "PTY-TEST FAIL %d\n" : "PTY-TEST PASS\n", fails);
    return fails != 0;
}
