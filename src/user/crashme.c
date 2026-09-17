/* crashme - deterministic Ring-3 fault injector for recovery testing.
 * Usage: crashme de|pf|ud   (divide error, page fault, invalid opcode) */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: crashme de|pf|ud\n");
        return 2;
    }
    if (strcmp(argv[1], "de") == 0) {
        volatile int z = 0;
        volatile int x = 1 / z;
        printf("%d\n", x);
    } else if (strcmp(argv[1], "pf") == 0) {
        *(volatile char *)0 = 0;
    } else if (strcmp(argv[1], "ud") == 0) {
        __asm__ volatile("ud2");
    } else {
        printf("usage: crashme de|pf|ud\n");
        return 2;
    }
    return 0;
}
