#define _GNU_SOURCE
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "synth_xz.h"

int main(int argc, char **argv, char **envp) {
    int pin[2], pout[2];
    pipe(pin);
    pipe(pout);

    if (!fork()) {
        close(pin[1]);
        close(pout[0]);
        dup2(pin[0], 0);
        dup2(pout[1], 1);
        execlp("xz", "xz", "-dc", NULL);
        _exit(1);
    }

    close(pin[0]);
    close(pout[1]);

    write(pin[1], synth_xz, synth_xz_len);
    close(pin[1]);

    int fd = syscall(SYS_memfd_create, "s", 0);
    char b[8192];
    int n;
    while ((n = read(pout[0], b, 8192)) > 0)
        write(fd, b, n);
    wait(0);

    syscall(SYS_execveat, fd, "", argv, envp, 0x1000);
    return 1;
}
