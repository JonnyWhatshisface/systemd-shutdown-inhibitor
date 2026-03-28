#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
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

static pid_t start_daemon_test_mode(const char *config_path, const char *socket_path)
{
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }

        assert(setenv("TERMINUSD_CONTROL_SOCKET", socket_path, 1) == 0);
        execl("./terminusd",
              "./terminusd",
              "--test-mode",
              "--foreground",
              "-c",
              config_path,
              (char *)NULL);
        _exit(127);
    }

    return pid;
}

static void stop_daemon(pid_t pid)
{
    if (pid <= 0)
        return;

    (void)kill(pid, SIGTERM);
    (void)waitpid(pid, NULL, 0);
}

static bool wait_for_socket_ready(const char *socket_path, int timeout_sec)
{
    time_t deadline = time(NULL) + timeout_sec;

    while (time(NULL) < deadline) {
        struct stat st;
        if (stat(socket_path, &st) == 0 && S_ISSOCK(st.st_mode))
            return true;

        usleep(100 * 1000);
    }

    return false;
}

static int run_terminusctl(const char *socket_path,
               const char *const *args,
               int arg_count,
               char *output,
               size_t output_cap)
{
    int pipefd[2];
    assert(pipe(pipefd) == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        if (pipefd[1] > STDERR_FILENO)
            close(pipefd[1]);

        assert(setenv("TERMINUSD_CONTROL_SOCKET", socket_path, 1) == 0);

        char *argv[32];
        int i;
        assert(arg_count >= 1);
        assert(arg_count < (int)(sizeof(argv) / sizeof(argv[0])) - 1);
        for (i = 0; i < arg_count; i++)
            argv[i] = (char *)args[i];
        argv[arg_count] = NULL;

        execv("./terminusctl", argv);
        _exit(127);
    }

    close(pipefd[1]);

    size_t len = 0;
    while (len + 1 < output_cap) {
        ssize_t n = read(pipefd[0], output + len, output_cap - 1 - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    output[len] = '\0';
    close(pipefd[0]);

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
}

static bool run_and_report(const char *name,
               const char *socket_path,
               const char *const *argv,
               int argc,
               int expected_exit,
               const char *expected_substring)
{
    char output[8192];
    int rc = run_terminusctl(socket_path, argv, argc, output, sizeof(output));

    bool ok = (rc == expected_exit);
    if (ok && expected_substring && expected_substring[0] != '\0')
        ok = strstr(output, expected_substring) != NULL;

    if (ok) {
        printf("[PASS] %s\n", name);
        return true;
    }

    printf("[FAIL] %s\n", name);
    printf("       expected exit=%d substring=%s\n",
           expected_exit,
           expected_substring ? expected_substring : "<none>");
    printf("       actual exit=%d output=%s\n", rc, output);
    return false;
}

int main(void)
{
    char tmpdir[] = "/tmp/terminusctl-cli-test-XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    char config_path[PATH_MAX];
    char socket_path[PATH_MAX];
    int written;

    written = snprintf(config_path, sizeof(config_path), "%s/test.conf", tmpdir);
    assert(written > 0 && (size_t)written < sizeof(config_path));
    written = snprintf(socket_path, sizeof(socket_path), "%s/terminusd.sock", tmpdir);
    assert(written > 0 && (size_t)written < sizeof(socket_path));

    write_file(config_path,
           "[main]\n"
           "set_max_inhibit_delay = false\n"
           "restart_logind_after_set = false\n"
           "max_inhibit_delay = 1800\n"
           "shutdown_guard_enabled = false\n"
           "\n"
           "[sample]\n"
           "command = /bin/true\n"
           "priority = 100\n");

    pid_t daemon_pid = start_daemon_test_mode(config_path, socket_path);

    if (!wait_for_socket_ready(socket_path, 8)) {
        stop_daemon(daemon_pid);
        fprintf(stderr, "[FAIL] daemon startup: control socket not ready\n");
        return 1;
    }

    int total = 0;
    int passed = 0;

    {
        const char *cmd[] = {"./terminusctl", "status"};
        total++;
        if (run_and_report("status", socket_path, cmd, 2, 0, "terminusd status"))
            passed++;
    }

    {
        const char *cmd[] = {"./terminusctl", "reload-config"};
        total++;
        if (run_and_report("reload-config", socket_path, cmd, 2, 0,
                   "configuration reloaded"))
            passed++;
    }

    {
        const char *cmd[] = {"./terminusctl", "shutdown-guard", "enable"};
        total++;
        if (run_and_report("shutdown-guard enable", socket_path, cmd, 3, 0,
                   "guard enable simulated (test mode)"))
            passed++;
    }

    {
        const char *cmd[] = {"./terminusctl", "shutdown-guard", "disable"};
        total++;
        if (run_and_report("shutdown-guard disable", socket_path, cmd, 3, 0,
                   "guard disable simulated (test mode)"))
            passed++;
    }

    {
        const char *cmd[] = {"./terminusctl", "shutdown-commands", "disable"};
        total++;
        if (run_and_report("shutdown-commands disable", socket_path, cmd, 3, 0,
                   "shutdown disable simulated (test mode)"))
            passed++;
    }

    {
        const char *cmd[] = {"./terminusctl", "shutdown-commands", "enable"};
        total++;
        if (run_and_report("shutdown-commands enable", socket_path, cmd, 3, 0,
                   "shutdown enable simulated (test mode)"))
            passed++;
    }

    {
        const char *cmd[] = {
            "./terminusctl",
            "--set-logind-inhibitor-delay"
        };
        total++;
        if (run_and_report("--set-logind-inhibitor-delay", socket_path, cmd, 2, 0,
                   "logind inhibit delay set simulated (test mode)"))
            passed++;
    }

    {
        const char *cmd[] = {"./terminusctl", "-f", "--skip-scripts", "system-reboot"};
        total++;
        if (run_and_report("system-reboot --force --skip-scripts", socket_path, cmd, 4, 0,
                   "reboot simulated (skip-scripts, test mode)"))
            passed++;
    }

    {
        const char *cmd[] = {"./terminusctl", "-f", "system-shutdown"};
        total++;
        if (run_and_report("system-shutdown --force", socket_path, cmd, 3, 0,
                   "shutdown simulated (test mode)"))
            passed++;
    }

    stop_daemon(daemon_pid);

    if (passed == total) {
        printf("[PASS] terminusctl CLI integration tests (%d/%d passed)\n", passed, total);
        return 0;
    }

    printf("[FAIL] terminusctl CLI integration tests (%d/%d passed)\n", passed, total);
    return 1;
}
