/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ntp.c - basis-ntpsync: SNTP client that disciplines CLOCK_REALTIME.
 *
 * Sends one client-mode request, waits under select() with a deadline, and
 * applies the server transmit timestamp through clock_settime. Exits 0 on
 * success; the manager parks a failing sync after its retry budget, so a
 * dead network never wedges boot.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NTP_EPOCH_DELTA 2208988800ULL /* seconds between 1900 and 1970 */
#define NTP_PORT        123
#define PKT_LEN         48
#define ATTEMPTS        3
#define TIMEOUT_SEC     3

/* Numeric input skips DNS: useful when the resolver is down but the gateway
 * still forwards NTP traffic. */
static int resolve(const char *host, struct in_addr *out) {
    if (inet_pton(AF_INET, host, out) == 1) return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, "ntp", &hints, &res) != 0 || !res) return -1;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* Server transmit timestamp: big-endian seconds at offset 40. */
static uint32_t ntp_seconds(const unsigned char *pkt) {
    return ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
           ((uint32_t)pkt[42] << 8)  |  (uint32_t)pkt[43];
}

int main(int argc, char **argv) {
    const char *server = argc > 1 ? argv[1] : "pool.ntp.org";

    struct in_addr addr;
    if (resolve(server, &addr) != 0) {
        fprintf(stderr, "basis-ntpsync: cannot resolve %s\n", server);
        return 1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("basis-ntpsync: socket");
        return 1;
    }

    unsigned char pkt[PKT_LEN];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B; /* LI=0, VN=4, Mode=3 (client) */

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(NTP_PORT);
    sa.sin_addr   = addr;

    time_t before = time(NULL);

    for (int attempt = 1; attempt <= ATTEMPTS; attempt++) {
        if (sendto(fd, pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            perror("basis-ntpsync: sendto");
            close(fd);
            return 1;
        }

        /* Plain select() deadline rather than SO_RCVTIMEO, so the wait is
         * correct wherever a socket layer implements poll but not timeouts. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { TIMEOUT_SEC, 0 };
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            fprintf(stderr, "basis-ntpsync: attempt %d timed out\n", attempt);
            continue;
        }

        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int r = (int)recvfrom(fd, pkt, sizeof(pkt), 0,
                              (struct sockaddr *)&from, &flen);
        if (r < PKT_LEN) continue;

        uint32_t secs = ntp_seconds(pkt);
        if (secs < NTP_EPOCH_DELTA) continue; /* server clock unset */

        time_t now = (time_t)(secs - NTP_EPOCH_DELTA);
        struct timespec ts = { now, 0 };
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            perror("basis-ntpsync: clock_settime");
            close(fd);
            return 1;
        }
        printf("basis-ntpsync: %s set the clock to %lld (drift %ld s)\n",
               server, (long long)now, (long)(now - before));
        close(fd);
        return 0;
    }

    close(fd);
    fprintf(stderr, "basis-ntpsync: no reply from %s\n", server);
    return 1;
}
