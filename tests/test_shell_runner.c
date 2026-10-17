/* SPDX-License-Identifier: Apache-2.0 */
#include "../src/tools/tool_shell_runner.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

int main(void)
{
    char out[64];
    assert(tool_shell_run("printf ready", out, sizeof(out), 500) == 0);
    assert(!strcmp(out, "ready"));
    assert(tool_shell_run("sleep 5", out, sizeof(out), 50) == -ETIMEDOUT);
    assert(tool_shell_run("exec 1>&-; sleep 5", out, sizeof(out), 50) == -ETIMEDOUT);
    assert(tool_shell_run("printf prefix; exit 1", out, sizeof(out), 500) == -EIO);
    assert(!strcmp(out, "prefix"));
    assert(tool_shell_run("i=0; while [ $i -lt 200 ]; do printf x; i=$((i+1)); done",
                          out, sizeof(out), 1000) == -EOVERFLOW);
    assert(strlen(out) == sizeof(out) - 1);
    assert(tool_shell_run("printf recovered", out, sizeof(out), 500) == 0);
    assert(!strcmp(out, "recovered"));
    assert(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
    assert(tool_shell_run("x", out, 0, 1) == -EINVAL);
    puts("test_shell_runner: OK (timeout, closed pipe, overflow, recovery, reap)");
    return 0;
}
