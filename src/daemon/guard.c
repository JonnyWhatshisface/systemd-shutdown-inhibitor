/* Copyright (c) 2021 Jonathan D. Hall <jon@jonathandavidhall.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
   shutdown_guard implementation.
  
   Supports two modes:
  
     oneshot  — runs shutdown_guard_command every shutdown_guard_interval
                seconds.  After shutdown_guard_threshold consecutive non-zero
                exit codes the shutdown targets are runtime-masked.  They are
                unmasked the first time the command returns 0.
  
     persist  — forks shutdown_guard_command once and keeps it running
                forever.  If it exits for any reason it is restarted
                immediately.
                The
                process controls masking by writing control lines to stdout:
                    shutdown_guard_disable_shutdown 1   → disable shutdowns
                    shutdown_guard_disable_shutdown 0   → enable shutdowns
                These directives may be emitted as often as the process
                wishes; all other output is logged to syslog.
*/

#include "inhibitor.h"
#include "guard.h"

#include <signal.h>
#include <time.h>

extern char **environ;

struct shutdown_guard_config guard_config = {
    .type      = GUARD_TYPE_ONESHOT,
    .interval  = 30,
    .threshold = 1,
    .enabled   = false,
};

static bool services_masked = false;

static pid_t persist_pid     = -1;
static int   persist_fd      = -1;
static char  persist_linebuf[1024];
static int   persist_linelen = 0;

static pid_t  oneshot_pid     = -1;
static int    oneshot_fd      = -1;
static char   oneshot_linebuf[1024];
static int    oneshot_linelen = 0;
static int    fail_count      = 0;
static time_t next_run_at     = 0;

static const char *const SHUTDOWN_TARGETS[] = {
    "shutdown.target",
    "reboot.target",
    "poweroff.target",
    "kexec.target",
    "halt.target",
    NULL
};

static bool target_is_masked(const char *target) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) < 0) {
        syslog(LOG_ERR, "[shutdown_guard] pipe() for state probe failed: %s", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "[shutdown_guard] fork() for state probe failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        if (pipefd[1] > STDERR_FILENO)
            close(pipefd[1]);
        execlp("systemctl", "systemctl", "is-enabled", target, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    char outbuf[256];
    size_t outlen = 0;
    for (;;) {
        ssize_t n = read(pipefd[0], outbuf + outlen, sizeof(outbuf) - 1 - outlen);
        if (n > 0) {
            outlen += (size_t)n;
            if (outlen >= sizeof(outbuf) - 1)
                break;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        break;
    }
    outbuf[outlen] = '\0';
    close(pipefd[0]);

    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR);

    return strstr(outbuf, "masked") != NULL;
}

static void sync_mask_state_on_startup(void) {
    int masked_count = 0;
    int total = 0;

    while (SHUTDOWN_TARGETS[total]) {
        if (target_is_masked(SHUTDOWN_TARGETS[total]))
            masked_count++;
        total++;
    }

    services_masked = (masked_count > 0);
    if (masked_count == 0) {
        syslog(LOG_INFO, "[shutdown_guard] startup state sync: shutdown targets currently unmasked");
        return;
    }

    if (masked_count == total) {
        syslog(LOG_INFO, "[shutdown_guard] startup state sync: shutdown targets currently masked");
        return;
    }

    syslog(LOG_WARNING, "[shutdown_guard] startup state sync: %d/%d shutdown targets are masked; treating guard state as masked", masked_count, total);
}

static bool apply_mask(bool mask) {
    if (services_masked == mask)
        return true;

    int ntargets = 0;
    while (SHUTDOWN_TARGETS[ntargets])
        ntargets++;

    // argv: systemctl mask/unmask --runtime <targets...> NULL
    char **argv = malloc((size_t)(3 + ntargets + 1) * sizeof(char *));
    if (!argv) {
        syslog(LOG_ERR, "[shutdown_guard] OOM building systemctl argv");
        return false;
    }
    argv[0] = "systemctl";
    argv[1] = mask ? "mask" : "unmask";
    argv[2] = "--runtime";
    for (int i = 0; i < ntargets; i++)
        argv[3 + i] = (char *)SHUTDOWN_TARGETS[i];
    argv[3 + ntargets] = NULL;

    int devnull = open("/dev/null", O_RDWR);

    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "[shutdown_guard] fork() for systemctl: %s", strerror(errno));
        free(argv);
        if (devnull >= 0) close(devnull);
        return false;
    }
    if (pid == 0) {
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execvp("systemctl", argv);
        _exit(127);
    }
    free(argv);
    if (devnull >= 0) close(devnull);

    int status = 0;
    int wret;
    do {
        wret = waitpid(pid, &status, 0);
    } while (wret < 0 && errno == EINTR);

    if (wret < 0) {
        syslog(LOG_ERR, "[shutdown_guard] waitpid(systemctl): %s", strerror(errno));
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        syslog(LOG_NOTICE, "[shutdown_guard] %s system shutdown and reboot", mask ? "DISABLED" : "ENABLED");
        services_masked = mask;
        return true;
    } else {
        syslog(LOG_ERR, "[shutdown_guard] systemctl %s --runtime failed (exit %d)", mask ? "mask" : "unmask", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
}

static pid_t launch_guard_process(int *fd_out) {
    int pipefd[2] = {-1, -1};
    if (pipe(pipefd) < 0) {
        syslog(LOG_ERR, "[shutdown_guard] pipe(): %s", strerror(errno));
        if (fd_out) *fd_out = -1;
        return -1;
    }

    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) {
        syslog(LOG_ERR, "[shutdown_guard] open(/dev/null): %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        if (fd_out) *fd_out = -1;
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "[shutdown_guard] fork(): %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        close(devnull);
        if (fd_out) *fd_out = -1;
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(126);
        if (pipefd[1] > STDERR_FILENO)
            close(pipefd[1]);
        if (dup2(devnull, STDIN_FILENO) < 0)
            _exit(126);
        if (devnull > STDIN_FILENO)
            close(devnull);

        // apply credentials
        script_entry_t dummy = {0};
        snprintf(dummy.name, sizeof(dummy.name), "shutdown_guard");
        snprintf(dummy.user, sizeof(dummy.user), "%s", guard_config.user);
        snprintf(dummy.group, sizeof(dummy.group), "%s", guard_config.group);
        if (apply_entry_credentials(&dummy) < 0)
            _exit(126);

        // load environment
        script_entry_t env_entry = {0};
        snprintf(env_entry.env, sizeof(env_entry.env), "%s", guard_config.env);
        char **env = load_entry_env(&env_entry);
        bool env_is_custom = (env != NULL);
        if (!env)
            env = environ;

        int argc = 0;
        char **argv = parse_command_argv(guard_config.command, &argc);
        if (!argv || argc == 0) {
            if (env_is_custom)
                free_env_array(env);
            _exit(127);
        }
        execve(argv[0], argv, env);
        int saved = errno;
        if (env_is_custom)
            free_env_array(env);
        fprintf(stderr, "[shutdown_guard] execve(\"%s\"): %s\n", argv[0], strerror(saved));
        free_argv(argv);
        _exit(127);
    }

    // parent
    close(pipefd[1]);
    close(devnull);

    int flags = fcntl(pipefd[0], F_GETFL);
    if (flags >= 0)
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    if (fd_out) *fd_out = pipefd[0];
    return pid;
}

static void handle_persist_line(const char *line) {
    if (strcmp(line, "shutdown_guard_disable_shutdown 1") == 0) {
        syslog(LOG_NOTICE, "[shutdown_guard] disabling shutdowns/reboots (masking shutdown targets)");
        apply_mask(true);
    } else if (strcmp(line, "shutdown_guard_disable_shutdown 0") == 0) {
        syslog(LOG_NOTICE, "[shutdown_guard] enabling shutdowns/reboots (unmasking shutdown targets)");
        apply_mask(false);
    } else {
        syslog(LOG_INFO, "[shutdown_guard] %s", line);
    }
}

static void persist_drain_and_restart(void) {
    char tmp[512];

    for (;;) {
        ssize_t n = read(persist_fd, tmp, sizeof(tmp));

        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = tmp[i];
                if (c == '\n' || c == '\r') {
                    if (persist_linelen > 0) {
                        persist_linebuf[persist_linelen] = '\0';
                        handle_persist_line(persist_linebuf);
                        persist_linelen = 0;
                    }
                } else if (persist_linelen < (int)sizeof(persist_linebuf) - 1) {
                    persist_linebuf[persist_linelen++] = c;
                }
            }
            continue;
        }

        if (n == 0) {
            // write end closed — process has exited; flush and restart
            if (persist_linelen > 0) {
                persist_linebuf[persist_linelen] = '\0';
                handle_persist_line(persist_linebuf);
                persist_linelen = 0;
            }
            close(persist_fd);
            persist_fd = -1;

            if (persist_pid > 0) {
                int status = 0;
                int wret;
                do {
                    wret = waitpid(persist_pid, &status, 0);
                } while (wret < 0 && errno == EINTR);
                int code = (wret > 0 && WIFEXITED(status))
                               ? WEXITSTATUS(status) : -1;
                syslog(LOG_WARNING, "[shutdown_guard] persist process (pid %d) exited (status %d); restarting", (int)persist_pid, code);
                persist_pid = -1;
            }

            persist_pid = launch_guard_process(&persist_fd);
            if (persist_pid > 0)
                syslog(LOG_INFO, "[shutdown_guard] persist restarted (pid %d)", (int)persist_pid);
            else
                syslog(LOG_ERR, "[shutdown_guard] failed to restart persist process");
            return;
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;

        syslog(LOG_ERR, "[shutdown_guard] persist pipe read error: %s",
               strerror(errno));
        close(persist_fd);
        persist_fd = -1;
        if (persist_pid > 0) {
            while (waitpid(persist_pid, NULL, 0) < 0 && errno == EINTR);
            persist_pid = -1;
        }
        persist_pid = launch_guard_process(&persist_fd);
        if (persist_pid > 0)
            syslog(LOG_INFO, "[shutdown_guard] persist restarted (pid %d)", (int)persist_pid);
        return;
    }
}

static void oneshot_drain(void) {
    char tmp[512];

    for (;;) {
        ssize_t n = read(oneshot_fd, tmp, sizeof(tmp));

        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = tmp[i];
                if (c == '\n' || c == '\r') {
                    if (oneshot_linelen > 0) {
                        oneshot_linebuf[oneshot_linelen] = '\0';
                        syslog(LOG_INFO, "[shutdown_guard] %s", oneshot_linebuf);
                        oneshot_linelen = 0;
                    }
                } else if (oneshot_linelen < (int)sizeof(oneshot_linebuf) - 1) {
                    oneshot_linebuf[oneshot_linelen++] = c;
                }
            }
            continue;
        }

        if (n == 0) {
            // process exited — flush any partial line
            if (oneshot_linelen > 0) {
                oneshot_linebuf[oneshot_linelen] = '\0';
                syslog(LOG_INFO, "[shutdown_guard] %s", oneshot_linebuf);
                oneshot_linelen = 0;
            }
            close(oneshot_fd);
            oneshot_fd = -1;

            if (oneshot_pid > 0) {
                int status = 0;
                int wret;
                do {
                    wret = waitpid(oneshot_pid, &status, 0);
                } while (wret < 0 && errno == EINTR);

                if (wret < 0) {
                    syslog(LOG_ERR, "[shutdown_guard] oneshot waitpid: %s", strerror(errno));
                    fail_count++;
                } else if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    if (code == 0) {
                        syslog(LOG_INFO, "[shutdown_guard] oneshot OK (exit 0); consecutive_failures=0");
                        fail_count = 0;
                        if (services_masked) {
                            syslog(LOG_NOTICE, "[shutdown_guard] condition cleared; unmasking shutdown targets");
                            apply_mask(false);
                        }
                    } else {
                        fail_count++;
                        syslog(LOG_WARNING,
                               "[shutdown_guard] oneshot exit %d (consecutive_failures=%d/%u)", code, fail_count, guard_config.threshold);
                        if ((unsigned int)fail_count >=
                            guard_config.threshold && !services_masked) {
                            syslog(LOG_NOTICE, "[shutdown_guard] threshold reached; masking shutdown targets");
                            apply_mask(true);
                        }
                    }
                } else if (WIFSIGNALED(status)) {
                    fail_count++;
                    syslog(LOG_WARNING, "[shutdown_guard] oneshot killed by signal %d (consecutive_failures=%d/%u)", WTERMSIG(status), fail_count, guard_config.threshold);
                    if ((unsigned int)fail_count >=
                        guard_config.threshold && !services_masked)
                        apply_mask(true);
                }
                oneshot_pid = -1;
            }
            next_run_at = time(NULL) + (time_t)guard_config.interval;
            return;
        }

        // n < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;

        syslog(LOG_ERR, "[shutdown_guard] oneshot pipe read error: %s", strerror(errno));
        close(oneshot_fd);
        oneshot_fd = -1;
        if (oneshot_pid > 0) {
            while (waitpid(oneshot_pid, NULL, 0) < 0 && errno == EINTR);
            oneshot_pid = -1;
        }
        next_run_at = time(NULL) + (time_t)guard_config.interval;
        return;
    }
}

void guard_start(void) {
    if (!guard_config.enabled)
        return;
    if (guard_config.command[0] == '\0') {
        syslog(LOG_WARNING, "[shutdown_guard] enabled but shutdown_guard_command not set; guard will not run");
        return;
    }

    // Sync cached state with systemd once at daemon start so restarts
    // don't lose whether shutdown targets are currently masked.
    sync_mask_state_on_startup();

    if (guard_config.type == GUARD_TYPE_PERSIST) {
        syslog(LOG_INFO, "[shutdown_guard] persist mode: command=\"%s\" user=%s group=%s", guard_config.command, guard_config.user[0] ? guard_config.user  : "<daemon>", guard_config.group[0] ? guard_config.group : "<daemon>");
        
        persist_pid = launch_guard_process(&persist_fd);
        if (persist_pid > 0)
            syslog(LOG_INFO, "[shutdown_guard] persistent user-defined shutdown guard started (pid %d)", (int)persist_pid);
        else
            syslog(LOG_ERR, "[shutdown_guard] failed to start persist process");
    } else {
        syslog(LOG_INFO, "[shutdown_guard] oneshot mode: command=\"%s\" interval=%us threshold=%u user=%s group=%s", guard_config.command, guard_config.interval, guard_config.threshold, guard_config.user[0]  ? guard_config.user  : "<daemon>", guard_config.group[0] ? guard_config.group : "<daemon>");
        next_run_at = time(NULL);   // run immediately on first tick
    }
}

static void guard_stop_runtime_processes(void) {
    if (persist_fd >= 0) {
        close(persist_fd);
        persist_fd = -1;
    }
    if (persist_pid > 0) {
        kill(persist_pid, SIGTERM);
        while (waitpid(persist_pid, NULL, 0) < 0 && errno == EINTR);
        persist_pid = -1;
    }

    if (oneshot_fd >= 0) {
        close(oneshot_fd);
        oneshot_fd = -1;
    }
    if (oneshot_pid > 0) {
        kill(oneshot_pid, SIGTERM);
        while (waitpid(oneshot_pid, NULL, 0) < 0 && errno == EINTR);
        oneshot_pid = -1;
    }

    oneshot_linelen = 0;
    persist_linelen = 0;
}

bool guard_runtime_set_enabled(bool enabled) {
    if (enabled == guard_config.enabled)
        return true;

    guard_config.enabled = enabled;

    if (!enabled) {
        guard_stop_runtime_processes();
        syslog(LOG_NOTICE, "[shutdown_guard] terminusctl command: guard disabled");
        return true;
    }

    if (guard_config.command[0] == '\0') {
        syslog(LOG_WARNING, "[shutdown_guard] terminusctl command requested guard enable, but shutdown_guard_command is empty");
        guard_config.enabled = false;
        return false;
    }

    fail_count = 0;
    guard_start();
    syslog(LOG_NOTICE, "[shutdown_guard] terminusctl command: guard enabled");
    return true;
}

bool guard_runtime_set_shutdown_disabled(bool disabled) {
    return apply_mask(disabled);
}

bool guard_runtime_unmask_forced(void) {
    return apply_mask(false);
}

bool guard_shutdowns_disabled(void) {
    return services_masked;
}

int guard_get_fd(void) {
    // Note: the returned fd may change between calls in persist mode (after a
    // restart).  Always call guard_get_fd() fresh at the start of each poll
    // iteration.
    if (!guard_config.enabled)
        return -1;
    if (guard_config.type == GUARD_TYPE_PERSIST)
        return persist_fd;
    return oneshot_fd;   // -1 when no child is running
}

void guard_on_readable(void) {
    if (!guard_config.enabled)
        return;
    if (guard_config.type == GUARD_TYPE_PERSIST)
        persist_drain_and_restart();
    else
        oneshot_drain();
}

void guard_tick(void) {
    // Called once per poll iteration.  In oneshot mode launches the next check
    // when the interval has elapsed and no child is currently running.
    // No-op in persist mode.
    if (!guard_config.enabled)
        return;
    if (guard_config.type != GUARD_TYPE_ONESHOT)
        return;
    if (oneshot_pid >= 0 || oneshot_fd >= 0)
        return;   /* child still running, poll on its fd */
    if (time(NULL) < next_run_at)
        return;

    syslog(LOG_DEBUG, "[shutdown_guard] launching oneshot check");
    oneshot_pid = launch_guard_process(&oneshot_fd);
    if (oneshot_pid < 0) {
        syslog(LOG_ERR, "[shutdown_guard] failed to launch oneshot check");
        next_run_at = time(NULL) + (time_t)guard_config.interval;
    }
}

int guard_poll_timeout_ms(void) {
    if (!guard_config.enabled)
        return -1;
    if (guard_config.type != GUARD_TYPE_ONESHOT)
        return -1;
    if (oneshot_pid >= 0 || oneshot_fd >= 0)
        return -1;   // waiting on fd, not timer
    time_t diff = next_run_at - time(NULL);
    if (diff <= 0)
        return 0;
    if (diff > 60) diff = 60;   // cap to avoid stalling indefinitely
    return (int)(diff * 1000);
}
