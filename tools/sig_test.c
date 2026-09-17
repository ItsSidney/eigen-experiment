/* sig_test - caught signals run and sigreturn restores context. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static int fails = 0;
#define CHECK(c, msg) do { \
    if (c) { printf("PASS %s\n", msg); } \
    else { printf("FAIL %s\n", msg); fails++; } \
} while (0)

static volatile int usr1_count;
static volatile unsigned long handler_rdi;

static void on_usr1(int sig) {
    usr1_count++;
    handler_rdi = (unsigned long)sig;
}

static volatile int usr2_count;

static void on_usr2(int sig) {
    (void)sig;
    usr2_count++;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR1, &sa, 0) == 0, "sigaction-usr1");

    CHECK(kill(getpid(), SIGUSR1) == 0, "kill-self");
    CHECK(usr1_count == 1, "handler-ran");
    CHECK(handler_rdi == SIGUSR1, "handler-arg");

    /* Register state survives the round trip. */
    volatile unsigned long x = 0x123456789ABCDEF0UL;
    kill(getpid(), SIGUSR1);
    CHECK(usr1_count == 2 && x == 0x123456789ABCDEF0UL, "regs-preserved");

    /* Blocked signals stay pending until unblocked. */
    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = on_usr2;
    sigemptyset(&sa2.sa_mask);
    CHECK(sigaction(SIGUSR2, &sa2, 0) == 0, "sigaction-usr2");

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    CHECK(sigprocmask(SIG_BLOCK, &block, &old) == 0, "sigblock");
    kill(getpid(), SIGUSR2);
    CHECK(usr2_count == 0, "handler-blocked");
    sigset_t pend;
    sigpending(&pend);
    CHECK(sigismember(&pend, SIGUSR2), "sigpending");
    CHECK(sigprocmask(SIG_SETMASK, &old, 0) == 0, "sigunblock");
    CHECK(usr2_count == 1, "handler-after-unblock");

    /* Ignored dispositions never run. */
    signal(SIGPIPE, SIG_IGN);
    CHECK(kill(getpid(), SIGPIPE) == 0, "kill-ignored");

    printf(fails ? "SIG-TEST FAIL %d\n" : "SIG-TEST PASS\n", fails);
    return fails != 0;
}
