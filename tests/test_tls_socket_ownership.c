/* SPDX-License-Identifier: Apache-2.0 */
/* Real loopback sockets and exec: a spawned command must not keep a
 * foreground transport alive after its owning task closes it. */
#include <assert.h>
#include <spawn.h>
#include <sys/wait.h>
#include "../src/infra/vela_tls.c"
#include "../src/infra/agent_timing.c"

extern char **environ;

void mbedtls_net_close(mbedtls_net_context *ctx)
{
    if (ctx->fd >= 0) close(ctx->fd);
    ctx->fd = -1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--child")) {
        errno = 0;
        return fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF ? 0 : 1;
    }

    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0);
    struct sockaddr_in address = { .sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(address);
    assert(bind(listener, (struct sockaddr *)&address, len) == 0);
    assert(listen(listener, 1) == 0);
    assert(getsockname(listener, (struct sockaddr *)&address, &len) == 0);
    char port[8];
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    tls_ctx_t ctx = {0};
    ctx.net.fd = -1;
    vela_req_ctl_t ctl = { .deadline_ms = agent_mono_ms() + 2000 };
    assert(tls_connect_controlled(&ctx, "127.0.0.1", port, &ctl) == 0);
    assert((fcntl(ctx.net.fd, F_GETFD) & FD_CLOEXEC) != 0);

    char fd[16];
    snprintf(fd, sizeof(fd), "%d", ctx.net.fd);
    char *args[] = { "/proc/self/exe", "--child", fd, NULL };
    pid_t child;
    int status;
    assert(posix_spawn(&child, args[0], NULL, NULL, args, environ) == 0);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(ctx.net.fd);
    close(listener);
    puts("test_tls_socket_ownership: OK (real connection, descriptor closed across exec)");
    return 0;
}
