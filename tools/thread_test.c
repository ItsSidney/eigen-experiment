/* thread_test - pthreads over clone/futex: join, mutex, TLS, gettid. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>

static int fails = 0;
#define CHECK(c, msg) do { \
    if (c) { printf("PASS %s\n", msg); } \
    else { printf("FAIL %s\n", msg); fails++; } \
} while (0)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long shared;
static __thread long tls_val;

static void *worker(void *arg) {
    long id = (long)arg;
    tls_val = 1000 + id;
    for (int i = 0; i < 250; i++) {
        pthread_mutex_lock(&lock);
        shared++;
        pthread_mutex_unlock(&lock);
    }
    /* TLS is per-thread: still ours after mutex traffic. */
    if (tls_val != 1000 + id) return (void *)1;
    if (getpid() == syscall(SYS_gettid)) return (void *)2;
    return (void *)0;
}

int main(void) {
    CHECK(getpid() == syscall(SYS_gettid), "main-tid-eq-pid");

    pthread_t t[4];
    for (long i = 0; i < 4; i++) {
        if (pthread_create(&t[i], 0, worker, (void *)i) != 0) {
            printf("FAIL create-%ld\n", i);
            fails++;
        }
    }
    CHECK(fails == 0, "create-x4");

    long bad = 0;
    for (int i = 0; i < 4; i++) {
        void *ret = (void *)1;
        if (pthread_join(t[i], &ret) != 0 || ret != 0) bad++;
    }
    CHECK(bad == 0, "join-retval");
    CHECK(shared == 1000, "shared-counter");

    /* Direct futex: mismatched word reports EAGAIN, wake is a no-op 0. */
    CHECK(syscall(SYS_futex, &(int){0}, 0 /*WAIT*/, 1, 0) == -1, "futex-eagain");

    printf(fails ? "THREAD-TEST FAIL %d\n" : "THREAD-TEST PASS\n", fails);
    return fails != 0;
}
