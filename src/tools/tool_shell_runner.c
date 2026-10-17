/* SPDX-License-Identifier: Apache-2.0 */
#ifdef __NuttX__
#include <nuttx/config.h>
#include <nuttx/lib/builtin.h>
#if defined(CONFIG_SYSTEM_POPEN) && !defined(CONFIG_SCHED_CHILD_STATUS)
#error "Bounded shell polling requires retained child exit status"
#endif
#if defined(CONFIG_SYSTEM_POPEN) && !defined(CONFIG_SYSTEM_POPEN_SHPATH)
#include <nshlib/nshlib.h>
#endif
#endif
#include "tool_shell_runner.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(__NuttX__) || defined(CONFIG_SYSTEM_POPEN)

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int start_command(pid_t *pid, const char *cmd,
                         posix_spawn_file_actions_t *actions,
                         posix_spawnattr_t *attr)
{
#ifdef __NuttX__
#ifdef CONFIG_VG_HMI
    {
    /* Run linked applications directly: killing only an NSH wrapper would
     * leave its builtin child running and its pipe/locks alive. Product
     * commands have already passed the allowlist and metacharacter guard. */
    char *copy = strdup(cmd);
    char *args[40];
    char *save = NULL;
    int n = 0;
    int rc;
    if (!copy) return ENOMEM;
    for (char *p = strtok_r(copy, " \t", &save); p;
         p = strtok_r(NULL, " \t", &save)) {
        if (n == 39) { free(copy); return E2BIG; }
        args[n++] = p;
    }
    args[n] = NULL;
    const char *base = n ? strrchr(args[0], '/') : NULL;
    int index = n ? builtin_isavail(base ? base + 1 : args[0]) : -1;
    const struct builtin_s *app = index < 0 ? NULL : builtin_for_index(index);
    if (app) {
        rc = posix_spawnattr_setstacksize(attr, app->stacksize);
        if (!rc) {
            *pid = task_spawn(app->name, app->main, actions, attr,
                              args + 1, NULL);
            rc = *pid < 0 ? -*pid : 0;
        }
        free(copy);
        return rc;
    }
    free(copy);
    }
#endif
#ifdef CONFIG_SYSTEM_POPEN_SHPATH
    char *argv[] = {CONFIG_SYSTEM_POPEN_SHPATH, "-c", (char *)cmd, NULL};
    return posix_spawn(pid, argv[0], actions, attr, argv, NULL);
#else
    char *argv[] = {"-c", (char *)cmd, NULL};
    int rc = posix_spawnattr_setstacksize(attr, CONFIG_SYSTEM_POPEN_STACKSIZE);
    if (rc) return rc;
    *pid = task_spawn("agent_shell", nsh_system, actions, attr, argv, NULL);
    return *pid < 0 ? -*pid : 0;
#endif
#else
    extern char **environ;
    char *argv[] = {"sh", "-c", (char *)cmd, NULL};
    int rc = posix_spawnattr_setflags(attr, POSIX_SPAWN_SETPGROUP);
    if (!rc) rc = posix_spawnattr_setpgroup(attr, 0);
    return rc ? rc : posix_spawn(pid, "/bin/sh", actions, attr, argv, environ);
#endif
}

int tool_shell_run(const char *command, char *out, size_t cap,
                   uint32_t timeout_ms)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int pipefd[2];
    pid_t pid = -1;
    int rc, status = 0;
    size_t used = 0;
    int overflow = 0, eof = 0, exited = 0;
    uint32_t started = now_ms();
    if (!command || !out || !cap || !timeout_ms) return -EINVAL;
    out[0] = 0;
    if (pipe(pipefd)) return -errno;
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0) {
        rc = errno;
        goto close_pipe;
    }
    rc = posix_spawn_file_actions_init(&actions);
    if (rc) goto close_pipe;
    rc = posix_spawnattr_init(&attr);
    if (rc) goto destroy_actions;
    if (!(rc = posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO)) &&
        !(rc = posix_spawn_file_actions_addclose(&actions, pipefd[0])) &&
        !(rc = posix_spawn_file_actions_addclose(&actions, pipefd[1])))
        rc = start_command(&pid, command, &actions, &attr);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    if (rc) { close(pipefd[0]); return -rc; }
    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        rc = -errno;
        goto stop_child;
    }
    while (!exited || !eof) {
        if (now_ms() - started >= timeout_ms) { rc = -ETIMEDOUT; goto stop_child; }
        struct pollfd pfd = {pipefd[0], POLLIN, 0};
        if (!eof && poll(&pfd, 1, 10) < 0 && errno != EINTR) {
            rc = -errno;
            goto stop_child;
        }
        if (eof) usleep(1000);
        char chunk[256];
        ssize_t got = eof ? -1 : read(pipefd[0], chunk, sizeof(chunk));
        if (got > 0) {
            size_t n = (size_t)got;
            size_t room = cap - used - 1;
            if (n > room) { n = room; overflow = 1; }
            memcpy(out + used, chunk, n);
            used += n;
            out[used] = 0;
        } else if (got == 0) eof = 1;
        else if (!eof && errno != EAGAIN && errno != EINTR) {
            rc = -errno;
            goto stop_child;
        }
        if (!exited) {
            pid_t done = waitpid(pid, &status, WNOHANG);
            if (done == pid) exited = 1;
            else if (done < 0 && errno != EINTR) { rc = -errno; goto stop_child; }
        }
    }
    close(pipefd[0]);
    return overflow ? -EOVERFLOW :
        (WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO);

stop_child:
#ifndef __NuttX__
    kill(-pid, SIGKILL);
#else
    if (!exited) kill(pid, SIGKILL);
#endif
    /* SIGKILL termination is asynchronous; reap with a bounded wait. */
    uint32_t stop = now_ms();
    while (!exited && now_ms() - stop < 100) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid || (done < 0 && errno == ECHILD)) exited = 1;
        else usleep(1000);
    }
    close(pipefd[0]);
    return rc;
destroy_actions:
    posix_spawn_file_actions_destroy(&actions);
close_pipe:
    close(pipefd[0]);
    close(pipefd[1]);
    return -rc;
}

#else
int tool_shell_run(const char *cmd, char *out, size_t cap, uint32_t ms)
{
    (void)cmd; (void)ms;
    if (out && cap) out[0] = 0;
    return -ENOSYS;
}
#endif
