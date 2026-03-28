#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(content, f) >= 0);
    assert(fclose(f) == 0);
}

static pid_t start_daemon_test_mode(const char *config_path, int *output_fd)
{
    int out_pipe[2];
    assert(pipe(out_pipe) == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(out_pipe[0]);
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)dup2(out_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO)
            close(out_pipe[1]);

        execl("./terminusd",
             "./terminusd",
             "--test-mode",
             "-c",
             config_path,
             (char *)NULL);
        _exit(127);
    }

    close(out_pipe[1]);
    *output_fd = out_pipe[0];
    return pid;
}

static bool read_until_contains(int fd,
                char *buffer,
                size_t cap,
                const char *needle,
                int timeout_sec)
{
    size_t len = strlen(buffer);
    time_t deadline = time(NULL) + timeout_sec;

    while (time(NULL) < deadline) {
        if (strstr(buffer, needle))
            return true;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (sel == 0)
            continue;

        char tmp[512];
        ssize_t n = read(fd, tmp, sizeof(tmp) - 1);
        if (n <= 0)
            return strstr(buffer, needle) != NULL;

        if (len + (size_t)n >= cap - 1)
            n = (ssize_t)(cap - 1 - len);
        if (n <= 0)
            return strstr(buffer, needle) != NULL;

        memcpy(buffer + len, tmp, (size_t)n);
        len += (size_t)n;
        buffer[len] = '\0';
    }

    return strstr(buffer, needle) != NULL;
}

static void drain_fd_to_buffer(int fd, char *buffer, size_t cap)
{
    size_t len = strlen(buffer);
    for (;;) {
        char tmp[512];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0)
            break;
        if (len + (size_t)n >= cap - 1)
            n = (ssize_t)(cap - 1 - len);
        if (n <= 0)
            break;
        memcpy(buffer + len, tmp, (size_t)n);
        len += (size_t)n;
        buffer[len] = '\0';
    }
}

static int run_argv(char *const argv[])
{
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
}

static bool emit_prepare_for_shutdown(void)
{
    char *const cmd[] = {
        "sudo",
        "-n",
        "busctl",
        "--system",
        "emit",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "PrepareForShutdown",
        "b",
        "true",
        NULL
    };

    int rc = run_argv(cmd);
    return rc == 0;
}

static bool has_passwordless_sudo(void)
{
    char *const cmd[] = {"sudo", "-n", "true", NULL};
    return run_argv(cmd) == 0;
}

static void terminate_and_reap(pid_t pid)
{
    kill(pid, SIGTERM);
    (void)waitpid(pid, NULL, 0);
}

int main(void)
{
    if (!has_passwordless_sudo()) {
        puts("[SKIP] test_daemon_test_mode_prepare_for_shutdown (passwordless sudo unavailable)");
        return 0;
    }

    char tmpdir[] = "/tmp/inhibitor-test-mode-XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/test.conf", tmpdir);

    char second_env_path[PATH_MAX];
    snprintf(second_env_path, sizeof(second_env_path), "%s/second.env", tmpdir);
    write_file(second_env_path,
            "TEST_FLAG=1\n"
            "TEST_NAME=second-script\n");

    char config_ini[8192];
    int written = snprintf(
        config_ini,
        sizeof(config_ini),
        "[main]\n"
        "shutdown_guard_enabled = true\n"
        "shutdown_guard_command = /bin/false --check\n"
        "shutdown_guard_type = oneshot\n"
        "shutdown_guard_interval = 60\n"
        "shutdown_guard_threshold = 3\n"
        "\n"
        "[third]\n"
        "command = /bin/echo\n"
        "priority = 300\n"
        "user = daemon\n"
        "group = daemon\n"
        "\n"
        "[first]\n"
        "command = /bin/true\n"
        "priority = 100\n"
        "user = root\n"
        "group = root\n"
        "\n"
        "[second]\n"
        "command = /bin/sleep 2\n"
        "priority = 200\n"
        "user = nobody\n"
        "group = nogroup\n"
        "env = %s\n"
        "\n"
        "[secondb]\n"
        "command = /bin/date\n"
        "priority = 200\n"
        "user = root\n"
        "group = root\n"
        "\n"
        "[disabled_example]\n"
        "command = /bin/echo\n"
        "priority = 150\n"
        "enabled = false\n"
        "\n"
        "[critical_step]\n"
        "command = /bin/false\n"
        "priority = 400\n"
        "critical = true\n"
        "simulate_exit_code = 1\n"
        "\n"
        "[also_disabled]\n"
        "command = /bin/echo\n"
        "priority = 450\n"
        "enabled = false\n"
        "\n"
        "[after_critical]\n"
        "command = /bin/true\n"
        "priority = 500\n",
        second_env_path);
    assert(written > 0 && (size_t)written < sizeof(config_ini));

    printf("--- daemon test-mode config begin ---\n%s", config_ini);
    if (config_ini[0] != '\0' && config_ini[strlen(config_ini) - 1] != '\n')
        printf("\n");
    printf("--- daemon test-mode config end ---\n");
    fflush(stdout);

    /* Verify the config print includes the disabled scripts */
    assert(strstr(config_ini, "[disabled_example]") != NULL);
    assert(strstr(config_ini, "enabled = false") != NULL);
    assert(strstr(config_ini, "[also_disabled]") != NULL);
    assert(strstr(config_ini, "command = /bin/sleep 2") != NULL);

    write_file(config_path, config_ini);

    int out_fd = -1;
    pid_t daemon_pid = start_daemon_test_mode(config_path, &out_fd);

    char output[65536] = {0};
    assert(read_until_contains(out_fd,
                    output,
                    sizeof(output),
                    "Inhibitor running, waiting for PrepareForShutdown signal",
                    10));

    bool got_shutdown = false;
    bool got_plan = false;
    bool emit_ok = false;
    time_t deadline = time(NULL) + 10;

    while (time(NULL) < deadline && (!got_shutdown || !got_plan)) {
        emit_ok = emit_prepare_for_shutdown();
        if (!emit_ok)
            break;

        if (!got_shutdown) {
            got_shutdown = read_until_contains(
                out_fd,
                output,
                sizeof(output),
                "[test-mode] PrepareForShutdown received (active=true)",
                1);
        }

        if (!got_plan) {
            got_plan = read_until_contains(
                out_fd,
                output,
                sizeof(output),
                "[test-mode] critical failure in priority group 400",
                1);
        }

        if (!got_shutdown || !got_plan)
            usleep(200000);
    }

    terminate_and_reap(daemon_pid);
    drain_fd_to_buffer(out_fd, output, sizeof(output));
    close(out_fd);

    printf("--- daemon test-mode output begin ---\n%s", output);
    if (output[0] != '\0' && output[strlen(output) - 1] != '\n')
        printf("\n");
    printf("--- daemon test-mode output end ---\n");
    fflush(stdout);

    if (!got_shutdown || !got_plan)
        fprintf(stderr, "Captured daemon output:\n%s\n", output);

    if (!emit_ok)
        fprintf(stderr,
            "Failed to emit PrepareForShutdown with sudo busctl. Ensure passwordless sudo for busctl in the test container.\n");

    assert(emit_ok);
    assert(got_shutdown);
    assert(got_plan);

    assert(strstr(output,
             "[test-mode] Running in test mode: script and shutdown_guard execution are disabled") != NULL);
    assert(strstr(output,
             "[test-mode] shutdown_guard configured but disabled in test mode: "
             "command=\"/bin/false --check\" type=oneshot") != NULL);
    assert(strstr(output,
             "[test-mode] Would execute 6 script section(s) in this order:") != NULL);
    assert(strstr(output,
             "[test-mode] parallel-group priority=100 count=1") != NULL);
    assert(strstr(output,
             "[test-mode] parallel-group priority=200 count=2") != NULL);
    assert(strstr(output,
             "[test-mode] parallel-group priority=300 count=1") != NULL);
    assert(strstr(output,
             "[test-mode] parallel-group priority=400 count=1") != NULL);
    assert(strstr(output,
             "[test-mode] #1 section=first priority=100 command=\"/bin/true\" user=root group=root env=<inherited>") != NULL);
    char expected_second_line[PATH_MAX + 256];
    int expected_written = snprintf(
        expected_second_line,
        sizeof(expected_second_line),
        "[test-mode] #2 section=second priority=200 command=\"/bin/sleep 2\" user=nobody group=nogroup env=%s",
        second_env_path);
    assert(expected_written > 0 &&
          (size_t)expected_written < sizeof(expected_second_line));
    assert(strstr(output, expected_second_line) != NULL);
    assert(strstr(output,
             "[test-mode] #3 section=secondb priority=200 command=\"/bin/date\" user=root group=root env=<inherited>") != NULL);
    assert(strstr(output,
             "[test-mode] #4 section=third priority=300 command=\"/bin/echo\" user=daemon group=daemon env=<inherited>") != NULL);
    assert(strstr(output,
             "[test-mode] #5 section=critical_step priority=400 command=\"/bin/false\" user=<daemon> group=<daemon> env=<inherited>") != NULL);
    assert(strstr(output,
             "[test-mode] critical script 'critical_step' simulate_exit_code=1: FAILED") != NULL);
    assert(strstr(output,
             "[test-mode] critical failure in priority group 400 -- aborting 1 remaining group(s); inhibitor would release") != NULL);

    /* Disabled scripts must NOT appear in the execution output */
    assert(strstr(output, "section=disabled_example") == NULL);
    assert(strstr(output, "section=also_disabled") == NULL);

    /* after_critical and its priority group must NOT appear — aborted by critical failure */
    assert(strstr(output, "parallel-group priority=500") == NULL);
    assert(strstr(output, "section=after_critical") == NULL);

    const char *l1   = strstr(output, "[test-mode] #1 section=first priority=100");
    const char *l2   = strstr(output, "[test-mode] #2 section=second priority=200");
    const char *l2b  = strstr(output, "[test-mode] #3 section=secondb priority=200");
    const char *l3   = strstr(output, "[test-mode] #4 section=third priority=300");
    const char *l4   = strstr(output, "[test-mode] #5 section=critical_step priority=400");
    const char *labs = strstr(output, "[test-mode] critical failure in priority group 400");
    assert(l1 != NULL && l2 != NULL && l2b != NULL && l3 != NULL && l4 != NULL && labs != NULL);
    assert(l1 < l2 && l2 < l2b && l2b < l3 && l3 < l4 && l4 < labs);

    (void)unlink(second_env_path);
    (void)unlink(config_path);
    (void)rmdir(tmpdir);

    puts("[PASS] test_daemon_test_mode_prepare_for_shutdown");
    return 0;
}