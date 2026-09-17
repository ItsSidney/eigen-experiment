/* unix_test - AF_UNIX stream bind/listen/accept/connect + dgram. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

static int fails = 0;
#define CHECK(c, msg) do { \
    if (c) { printf("PASS %s\n", msg); } \
    else { printf("FAIL %s (errno=%d)\n", msg, errno); fails++; } \
} while (0)

int main(void) {
    unlink("/tmp/unix_test.sock");
    unlink("/tmp/unix_dg.sock");

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(srv >= 0, "socket-stream");

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/tmp/unix_test.sock");
    CHECK(bind(srv, (struct sockaddr *)&sa, sizeof(sa.sun_family) + strlen(sa.sun_path) + 1) == 0, "bind");

    int dup = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(bind(dup, (struct sockaddr *)&sa, sizeof(sa.sun_family) + strlen(sa.sun_path) + 1) < 0, "bind-inuse");
    close(dup);

    struct stat st;
    CHECK(stat("/tmp/unix_test.sock", &st) == 0 && S_ISSOCK(st.st_mode), "stat-ifsock");

    CHECK(listen(srv, 4) == 0, "listen");

    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(cli >= 0, "socket-client");
    CHECK(connect(cli, (struct sockaddr *)&sa, sizeof(sa.sun_family) + strlen(sa.sun_path) + 1) == 0, "connect");

    struct sockaddr_un peer;
    socklen_t plen = sizeof(peer);
    int acc = accept(srv, (struct sockaddr *)&peer, &plen);
    CHECK(acc >= 0, "accept");

    struct sockaddr_un name;
    socklen_t nlen = sizeof(name);
    CHECK(getsockname(acc, (struct sockaddr *)&name, &nlen) == 0 &&
          strcmp(name.sun_path, "/tmp/unix_test.sock") == 0, "getsockname-acc");

    nlen = sizeof(name);
    memset(&name, 0, sizeof(name));
    CHECK(getpeername(cli, (struct sockaddr *)&name, &nlen) == 0 &&
          strcmp(name.sun_path, "/tmp/unix_test.sock") == 0, "getpeername-cli");

    CHECK(write(cli, "hello-unix", 10) == 10, "write-c2s");
    char buf[16];
    memset(buf, 0, sizeof(buf));
    CHECK(read(acc, buf, 10) == 10 && memcmp(buf, "hello-unix", 10) == 0, "read-c2s");

    CHECK(write(acc, "world-unix", 10) == 10, "write-s2c");
    memset(buf, 0, sizeof(buf));
    CHECK(read(cli, buf, 10) == 10 && memcmp(buf, "world-unix", 10) == 0, "read-s2c");

    struct pollfd pf;
    pf.fd = acc;
    pf.events = POLLIN;
    CHECK(poll(&pf, 1, 0) == 0, "poll-empty");

    int bad = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un no;
    memset(&no, 0, sizeof(no));
    no.sun_family = AF_UNIX;
    strcpy(no.sun_path, "/tmp/no-such.sock");
    CHECK(connect(bad, (struct sockaddr *)&no, sizeof(no.sun_family) + strlen(no.sun_path) + 1) < 0, "connect-refused");
    close(bad);

    /* Datagram pair over a bound name. */
    int dg1 = socket(AF_UNIX, SOCK_DGRAM, 0);
    int dg2 = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un dg;
    memset(&dg, 0, sizeof(dg));
    dg.sun_family = AF_UNIX;
    strcpy(dg.sun_path, "/tmp/unix_dg.sock");
    CHECK(bind(dg1, (struct sockaddr *)&dg, sizeof(dg.sun_family) + strlen(dg.sun_path) + 1) == 0, "dgram-bind");
    CHECK(sendto(dg2, "dg-hello", 8, 0, (struct sockaddr *)&dg, sizeof(dg.sun_family) + strlen(dg.sun_path) + 1) == 8, "dgram-sendto");
    struct sockaddr_un from;
    socklen_t flen = sizeof(from);
    memset(buf, 0, sizeof(buf));
    memset(&from, 0, sizeof(from));
    int r = recvfrom(dg1, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
    CHECK(r == 8 && memcmp(buf, "dg-hello", 8) == 0, "dgram-recvfrom");
    close(dg1);
    close(dg2);

    close(acc);
    close(cli);
    close(srv);
    unlink("/tmp/unix_test.sock");
    unlink("/tmp/unix_dg.sock");

    printf(fails ? "UNIX-TEST FAIL %d\n" : "UNIX-TEST PASS\n", fails);
    return fails != 0;
}
