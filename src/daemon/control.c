#include "inhibitor.h"
#include "guard.h"
#include "control.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

static bool skip_shutdown_scripts_once = false;
static bool control_test_mode = false;
static char control_socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)] = CONTROL_SOCKET_PATH;

static const char *get_control_socket_path(void) {
    /*
       TODO:

       The socket is always 0600 so there's no risk here.
       However, it should really be hard-coded for the test-mode,
       and should also make the path of the socket configurable
       via the config.

       For now, there's no actual security risk/exposure other
       than the socket not being where expected should someone
       set the env, but temrinusctl will also honor this env var.
    */
    const char *override = getenv("TERMINUSD_CONTROL_SOCKET");

    if (override && override[0] == '/' &&
        strlen(override) < sizeof(control_socket_path)) {
        snprintf(control_socket_path, sizeof(control_socket_path), "%s", override);
    } else {
        snprintf(control_socket_path, sizeof(control_socket_path), "%s", CONTROL_SOCKET_PATH);
    }

    return control_socket_path;
}

void control_set_test_mode(bool enabled) {
    // TODO:
    // Perhaps we move this to inhibitor.h to make it global?
    control_test_mode = enabled;
}

static void write_control_response(int fd, bool ok, const char *msg) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s %s\n", ok ? "OK" : "ERR", msg ? msg : (ok ? "done" : "failed"));
    if (n <= 0)
        return;
    if ((size_t)n > sizeof(buf))
        n = (int)sizeof(buf);
    (void)write(fd, buf, (size_t)n);
}

static bool run_systemctl_reboot(void) {
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Failed to fork for reboot command: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        execlp("systemctl", "systemctl", "reboot", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    int wret;
    do {
        wret = waitpid(pid, &status, 0);
    } while (wret < 0 && errno == EINTR);

    if (wret < 0) {
        syslog(LOG_ERR, "Failed waiting for reboot command: %s", strerror(errno));
        return false;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;

    syslog(LOG_ERR, "Reboot command failed (status=%d)",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return false;
}

static bool run_systemctl_poweroff(void) {
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Failed to fork for poweroff command: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        execlp("systemctl", "systemctl", "poweroff", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    int wret;
    do {
        wret = waitpid(pid, &status, 0);
    } while (wret < 0 && errno == EINTR);

    if (wret < 0) {
        syslog(LOG_ERR, "Failed waiting for poweroff command: %s", strerror(errno));
        return false;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return true;

    syslog(LOG_ERR, "Poweroff command failed (status=%d)",
          WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return false;
}

static bool reload_runtime_configuration(const char *selected_config_path_runtime, bool selected_config_is_custom) {
    if (control_test_mode) {
        load_selected_config(selected_config_is_custom ? selected_config_path_runtime : NULL);
        return true;
    }

    // Stop any existing runtime guard process/timer before reloading.
    (void)guard_runtime_set_enabled(false);

    load_selected_config(selected_config_is_custom ? selected_config_path_runtime : NULL);
    ensure_logind_inhibit_delay();

    if (guard_config.enabled)
        guard_start();

    return true;
}

static int cmp_script_index(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return cmp_priority(&scripts[ia], &scripts[ib]);
}

static bool status_append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_list ap_copy;

    if (!*buf) {
        *cap = 2048;
        *buf = malloc(*cap);
        if (!*buf)
            return false;
        (*buf)[0] = '\0';
        *len = 0;
    }

    for (;;) {
        va_start(ap, fmt);
        va_copy(ap_copy, ap);
        int needed = vsnprintf(*buf + *len, *cap - *len, fmt, ap_copy);
        va_end(ap_copy);
        va_end(ap);

        if (needed < 0)
            return false;

        if ((size_t)needed < (*cap - *len)) {
            *len += (size_t)needed;
            return true;
        }

        size_t new_cap = *cap * 2;
        size_t min_cap = *len + (size_t)needed + 1;
        if (new_cap < min_cap)
            new_cap = min_cap;

        char *new_buf = realloc(*buf, new_cap);
        if (!new_buf)
            return false;

        *buf = new_buf;
        *cap = new_cap;
    }
}

static char *build_status_payload(void) {
    // Thank you, copilot - I can't format to save my soul.
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    if (!status_append(&buf, &len, &cap,
               "========================================\n"
               "            terminusd status\n"
               "========================================\n\n"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "Inhibitor Settings\n"
             "------------------\n"
             "%-22s : %ds\n",
             "Daemon Max Delay",
             max_inhibit_delay))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %ds\n",
             "Logind Max Delay",
             current_logind_inhibit_delay))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %s\n",
             "Delay Match",
             max_inhibit_delay == current_logind_inhibit_delay ?
             "yes" : "no"))
        goto oom;

    if (max_inhibit_delay != current_logind_inhibit_delay) {
        if (!status_append(&buf, &len, &cap,
                 "%-22s : daemon configured delay does not match logind\n",
                 "Warning"))
            goto oom;
    }

    if (!status_append(&buf, &len, &cap,
             "\nShutdown Controls\n"
             "-----------------\n"
             "%-22s : %s\n",
             "Shutdown Requests",
             guard_shutdowns_disabled() ? "disabled" : "enabled"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %s\n",
             "Shutdown Guard",
               guard_config.enabled ? "enabled" : "disabled"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %s\n",
             "Guard Type",
               guard_config.type == GUARD_TYPE_PERSIST ? "persist" : "oneshot"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %s\n",
             "Guard Command",
               guard_config.command[0] ? guard_config.command : "<unset>"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %s\n",
             "Guard User",
               guard_config.user[0] ? guard_config.user : "<daemon>"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %s\n",
             "Guard Group",
               guard_config.group[0] ? guard_config.group : "<daemon>"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %s\n",
             "Guard Env File",
               guard_config.env[0] ? guard_config.env : "<inherited>"))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %us\n",
             "Guard Interval",
               guard_config.interval))
        goto oom;

    if (!status_append(&buf, &len, &cap,
             "%-22s : %u\n\n",
             "Guard Threshold",
               guard_config.threshold))
        goto oom;

    if (!status_append(&buf, &len, &cap,
               "Shutdown Execution Plan\n"
               "---------------------\n"
               "Enabled Scripts  : %d\n\n",
               script_count))
        goto oom;

    if (script_count == 0) {
        if (!status_append(&buf, &len, &cap,
                   "<none configured>\n"))
            goto oom;
        return buf;
    }

    int order[MAX_SCRIPTS];
    for (int i = 0; i < script_count; i++)
        order[i] = i;

    qsort(order, (size_t)script_count, sizeof(order[0]), cmp_script_index);

    for (int i = 0; i < script_count; ) {
        unsigned int prio = scripts[order[i]].priority;
        int j = i;
        while (j < script_count && scripts[order[j]].priority == prio)
            j++;

        if (!status_append(&buf, &len, &cap,
                   "Priority %u (%d script%s)\n",
                   prio,
                   j - i,
                   (j - i) == 1 ? "" : "s"))
            goto oom;

        for (int k = i; k < j; k++) {
            const script_entry_t *e = &scripts[order[k]];
            if (!status_append(&buf, &len, &cap,
                       "  [%d] %s\n",
                       k + 1,
                       e->name))
                goto oom;

            if (!status_append(&buf, &len, &cap,
                       "      critical : %s\n",
                       e->critical ? "yes" : "no"))
                goto oom;

            if (!status_append(&buf, &len, &cap,
                       "      command  : %s\n",
                       e->command[0] ? e->command : "<unset>"))
                goto oom;

            if (!status_append(&buf, &len, &cap,
                       "      user     : %s\n"
                       "      group    : %s\n"
                       "      env      : %s\n",
                       e->user[0] ? e->user : "<daemon>",
                       e->group[0] ? e->group : "<daemon>",
                       e->env[0] ? e->env : "<inherited>"))
                goto oom;

            if (!status_append(&buf, &len, &cap, "\n"))
                goto oom;
        }

        i = j;
    }

    return buf;

oom:
    free(buf);
    return NULL;
}

static void write_control_payload_response(int fd, bool ok, const char *payload) {
    if (ok) {
        (void)write(fd, "OK\n", 3);
        if (payload && payload[0] != '\0') {
            size_t plen = strlen(payload);
            (void)write(fd, payload, plen);
            if (payload[plen - 1] != '\n')
                (void)write(fd, "\n", 1);
        }
        return;
    }

    if (!payload || payload[0] == '\0')
        payload = "failed";
    write_control_response(fd, false, payload);
}

static void handle_control_client_request(int client_fd, const char *selected_config_path_runtime, bool selected_config_is_custom) {
    char req[256] = {0};
    ssize_t nr = read(client_fd, req, sizeof(req) - 1);
    if (nr <= 0) {
        write_control_response(client_fd, false, "empty request");
        return;
    }
    req[nr] = '\0';

    char *nl = strpbrk(req, "\r\n");
    if (nl)
        *nl = '\0';

    char *saveptr = NULL;
    char *cmd = strtok_r(req, " \t", &saveptr);
    if (!cmd) {
        write_control_response(client_fd, false, "invalid request");
        return;
    }

    if (strcmp(cmd, "RELOAD") == 0) {
        if (reload_runtime_configuration(selected_config_path_runtime, selected_config_is_custom))
            write_control_response(client_fd, true, "configuration reloaded");
        else
            write_control_response(client_fd, false, "reload failed");
        return;
    }

    if (strcmp(cmd, "GUARD") == 0) {
        char *arg = strtok_r(NULL, " \t", &saveptr);
        if (!arg) {
            write_control_response(client_fd, false, "missing GUARD action");
            return;
        }

        if (strcmp(arg, "ENABLE") == 0) {
            if (control_test_mode) {
                write_control_response(client_fd, true, "guard enable simulated (test mode)");
                return;
            }
            bool ok = guard_runtime_set_enabled(true);
            write_control_response(client_fd, ok, ok ? "guard enabled" : "guard enable failed");
            return;
        }

        if (strcmp(arg, "DISABLE") == 0) {
            if (control_test_mode) {
                write_control_response(client_fd, true, "guard disable simulated (test mode)");
                return;
            }
            bool ok = guard_runtime_set_enabled(false);
            write_control_response(client_fd, ok, ok ? "guard disabled" : "guard disable failed");
            return;
        }

        write_control_response(client_fd, false, "unknown GUARD action");
        return;
    }

    if (strcmp(cmd, "SHUTDOWN") == 0) {
        char *arg = strtok_r(NULL, " \t", &saveptr);
        if (!arg) {
            write_control_response(client_fd, false, "missing SHUTDOWN action");
            return;
        }

        if (strcmp(arg, "DISABLE") == 0) {
            if (control_test_mode) {
                write_control_response(client_fd, true, "shutdown disable simulated (test mode)");
                return;
            }
            bool ok = guard_runtime_set_shutdown_disabled(true);
            write_control_response(client_fd, ok, ok ? "shutdown disabled" : "shutdown disable failed");
            return;
        }

        if (strcmp(arg, "ENABLE") == 0) {
            if (control_test_mode) {
                write_control_response(client_fd, true, "shutdown enable simulated (test mode)");
                return;
            }
            bool ok = guard_runtime_set_shutdown_disabled(false);
            write_control_response(client_fd, ok, ok ? "shutdown enabled" : "shutdown enable failed");
            return;
        }

        write_control_response(client_fd, false, "unknown SHUTDOWN action");
        return;
    }

    if (strcmp(cmd, "REBOOT") == 0) {
        bool skip = false;
        char *arg = strtok_r(NULL, " \t", &saveptr);
        if (arg) {
            if (strcmp(arg, "SKIP") == 0)
                skip = true;
            else {
                write_control_response(client_fd, false, "unknown REBOOT option");
                return;
            }
        }

        if (control_test_mode) {
            write_control_response(client_fd, true, skip ? "reboot simulated (skip-scripts, test mode)" : "reboot simulated (test mode)");
            return;
        }

        if (!guard_runtime_unmask_forced()) {
            write_control_response(client_fd, false, "failed to unmask shutdown targets");
            return;
        }

        if (skip)
            skip_shutdown_scripts_once = true;

        if (!run_systemctl_reboot()) {
            if (skip)
                skip_shutdown_scripts_once = false;
            write_control_response(client_fd, false, "failed to trigger reboot");
            return;
        }

        write_control_response(client_fd, true, skip ? "reboot requested (skip-scripts)" : "reboot requested");
        return;
    }

    if (strcmp(cmd, "POWEROFF") == 0) {
        bool skip = false;
        char *arg = strtok_r(NULL, " \t", &saveptr);
        if (arg) {
            if (strcmp(arg, "SKIP") == 0)
                skip = true;
            else {
                write_control_response(client_fd, false, "unknown POWEROFF option");
                return;
            }
        }

        if (control_test_mode) {
            write_control_response(client_fd, true, skip ? "shutdown simulated (skip-scripts, test mode)" : "shutdown simulated (test mode)");
            return;
        }

        if (!guard_runtime_unmask_forced()) {
            write_control_response(client_fd, false, "failed to unmask shutdown targets");
            return;
        }

        if (skip)
            skip_shutdown_scripts_once = true;

        if (!run_systemctl_poweroff()) {
            if (skip)
                skip_shutdown_scripts_once = false;
            write_control_response(client_fd, false, "failed to trigger shutdown");
            return;
        }

        write_control_response(client_fd, true, skip ? "shutdown requested (skip-scripts)" : "shutdown requested");
        return;
    }

    if (strcmp(cmd, "SET_LOGIND_INHIBITOR_DELAY") == 0) {
        char *extra = strtok_r(NULL, " \t", &saveptr);
        char msg[128];
        bool original_set_max_inhibit_delay;

        if (extra) {
            write_control_response(client_fd, false, "unexpected SET_LOGIND_INHIBITOR_DELAY arguments");
            return;
        }

        if (control_test_mode) {
            write_control_response(client_fd, true, "logind inhibit delay set simulated (test mode)");
            return;
        }

        original_set_max_inhibit_delay = set_max_inhibit_delay;
        set_max_inhibit_delay = true;
        ensure_logind_inhibit_delay();
        set_max_inhibit_delay = original_set_max_inhibit_delay;

        if (current_logind_inhibit_delay != max_inhibit_delay) {
            write_control_response(client_fd, false, "failed to set logind inhibit delay");
            return;
        }

        snprintf(msg, sizeof(msg), "logind inhibit delay set to %d", max_inhibit_delay);
        write_control_response(client_fd, true, msg);
        return;
    }

    if (strcmp(cmd, "STATUS") == 0) {
        char *extra = strtok_r(NULL, " \t", &saveptr);
        if (extra) {
            write_control_response(client_fd, false, "unexpected STATUS arguments");
            return;
        }

        char *status_payload = build_status_payload();
        if (!status_payload) {
            write_control_response(client_fd, false, "failed to build status payload");
            return;
        }

        write_control_payload_response(client_fd, true, status_payload);
        free(status_payload);
        return;
    }

    write_control_response(client_fd, false, "unknown command");
}

int control_setup_socket(void) {
    const char *socket_path = get_control_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int flags;
    if (fd < 0) {
        syslog(LOG_ERR, "Failed to create control socket: %s", strerror(errno));
        return -1;
    }

    flags = fcntl(fd, F_GETFD);
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        syslog(LOG_WARNING, "Failed to set FD_CLOEXEC (fd=%d): %s", fd, strerror(errno));
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    (void)unlink(socket_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "Failed to bind control socket %s: %s", socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (chmod(socket_path, S_IRUSR | S_IWUSR) < 0) {
        syslog(LOG_ERR, "Failed to chmod control socket %s: %s", socket_path, strerror(errno));
        close(fd);
        (void)unlink(socket_path);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        syslog(LOG_ERR, "Failed to listen on control socket %s: %s", socket_path, strerror(errno));
        close(fd);
        (void)unlink(socket_path);
        return -1;
    }

    if (control_test_mode)
        syslog(LOG_INFO, "Control socket ready at %s (test mode)", socket_path);
    else
        syslog(LOG_INFO, "Control socket ready at %s (root-only)", socket_path);
    return fd;
}

void control_handle_socket_ready(int control_listen_fd, const char *selected_config_path_runtime, bool selected_config_is_custom) {
    int client_fd = accept(control_listen_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            syslog(LOG_ERR, "accept() on control socket failed: %s", strerror(errno));
        return;
    }

    int flags = fcntl(client_fd, F_GETFD);
    if (fcntl(client_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        syslog(LOG_WARNING, "Failed to set FD_CLOEXEC (fd=%d): %s", client_fd, strerror(errno));
    }

    handle_control_client_request(client_fd, selected_config_path_runtime, selected_config_is_custom);
    close(client_fd);
}

void control_cleanup_socket(void) {
    (void)unlink(get_control_socket_path());
}

bool control_consume_skip_shutdown_scripts_once(void) {
    bool skip = skip_shutdown_scripts_once;
    skip_shutdown_scripts_once = false;
    return skip;
}
