/* test_pty - opens /dev/ptmx, writes to master, reads from slave */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <errno.h>

int main(void) {
    int master_fd;
    char slave_name[64];
    int slave_fd;

    printf("=== PTY Test ===\n");

    /* Open the PTY master */
    master_fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master_fd < 0) {
        perror("open /dev/ptmx");
        return 1;
    }
    printf("master fd: %d\n", master_fd);

    /* Grant and unlock the slave */
    if (grantpt(master_fd) < 0) {
        perror("grantpt");
        close(master_fd);
        return 1;
    }
    printf("grantpt OK\n");

    if (unlockpt(master_fd) < 0) {
        perror("unlockpt");
        close(master_fd);
        return 1;
    }
    printf("unlockpt OK\n");

    /* Get the slave name */
    if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) < 0) {
        perror("ptsname_r");
        close(master_fd);
        return 1;
    }
    printf("slave name: %s\n", slave_name);

    /* Open the slave */
    slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        perror("open slave");
        /* Try to create it if it doesn't exist */
        printf("slave not found, trying to create...\n");
        close(master_fd);
        return 1;
    }
    printf("slave fd: %d\n", slave_fd);

    /* Write to master, read from slave */
    const char *msg = "hello from master\n";
    if (write(master_fd, msg, strlen(msg)) < 0) {
        perror("write master");
    } else {
        printf("wrote to master: %s", msg);
    }

    char buf[256];
    memset(buf, 0, sizeof(buf));
    if (read(slave_fd, buf, sizeof(buf)) < 0) {
        perror("read slave");
    } else {
        printf("read from slave: %s", buf);
    }

    /* Write to slave, read from master */
    const char *reply = "hello from slave\n";
    if (write(slave_fd, reply, strlen(reply)) < 0) {
        perror("write slave");
    } else {
        printf("wrote to slave: %s", reply);
    }

    memset(buf, 0, sizeof(buf));
    if (read(master_fd, buf, sizeof(buf)) < 0) {
        perror("read master");
    } else {
        printf("read from master: %s", buf);
    }

    close(slave_fd);
    close(master_fd);

    printf("=== PTY Test Done ===\n");
    return 0;
}
