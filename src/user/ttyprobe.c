/* ttyprobe - report winsize, then replicate slang's select+read loop.
 * Now also: raw-mode key delivery check. Press arrow keys; each byte is
 * printed as hex. 'q' quits. This tells us whether multi-byte escape
 * sequences reach a raw-mode reader intact (newt/slang needs exactly that). */
#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>

int main(void) {
    struct winsize ws;
    memset(&ws, 0xAA, sizeof(ws));
    int r = ioctl(0, TIOCGWINSZ, &ws);
    printf("winsize rc=%d rows=%d cols=%d x=%d y=%d\n",
           r, ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel);
    fflush(stdout);
    int fd = open("/dev/tty", O_RDWR);
    printf("open-tty=%d\n", fd);
    fflush(stdout);
    r = ioctl(fd >= 0 ? fd : 0, TIOCGWINSZ, &ws);
    printf("tty-winsize rc=%d rows=%d cols=%d\n", r, ws.ws_row, ws.ws_col);
    fflush(stdout);

    /* ── raw-mode key delivery check ─────────────────────────────────────── */
    printf("keys: press arrows, then q to quit\n");
    fflush(stdout);

    struct termios t, o;
    if (tcgetattr(0, &o) != 0) {
        printf("tcgetattr failed\n");
        return 1;
    }
    t = o;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) {
        printf("tcsetattr(raw) failed\n");
        return 1;
    }

    for (;;) {
        unsigned char b[16];
        int n = (int)read(0, b, sizeof b);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            printf("read rc=%d errno=%d\n", n, n < 0 ? errno : 0);
            fflush(stdout);
            continue;
        }
        for (int i = 0; i < n; i++) printf("0x%02x ", b[i]);
        printf("(%d byte%s)\n", n, n == 1 ? "" : "s");
        fflush(stdout);
        if (b[0] == 'q') break;
    }

    tcsetattr(0, TCSANOW, &o);
    printf("done\n");
    return 0;
}
