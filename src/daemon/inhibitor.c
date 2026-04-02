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

#include "inhibitor.h"
#include "test.h"
#include "guard.h"
#include "control.h"

int  inhibitor_fd               = -1;
bool set_max_inhibit_delay      = false;
bool restart_logind_after_set   = false;
int  max_inhibit_delay          = DEFAULT_MAX_INHIBIT_DELAY;
int  current_logind_inhibit_delay = SYSTEMD_DEFAULT_INHIBIT_DELAY;

static bool test_mode_enabled = false;
static bool test_mode_shutdown_seen = false;

static int  control_listen_fd = -1;
static char selected_config_path_runtime[PATH_MAX] = {0};
static bool selected_config_is_custom = false;

script_entry_t scripts[MAX_SCRIPTS];
int script_count = 0;

static int handle_prepare_for_shutdown(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    int active = 0;
    int r;

    (void)userdata;
    (void)ret_error;

    r = sd_bus_message_read(m, "b", &active);
    if (r < 0) {
        syslog(LOG_ERR, "Failed to parse PrepareForShutdown signal: %s", strerror(-r));
        return r;
    }

    if (!active)
        return 0;

    if (test_mode_enabled) {
        test_mode_log(LOG_INFO, "[test-mode] PrepareForShutdown received (active=true)");
        log_test_mode_plan();
        test_mode_shutdown_seen = true;
        return 0;
    }

    if (control_consume_skip_shutdown_scripts_once()) {
        syslog(LOG_NOTICE, "Skipping script/priority execution for this shutdown due to runtime reboot --skip-scripts request");
        if (inhibitor_fd >= 0) {
            close(inhibitor_fd);
            inhibitor_fd = -1;
        }
        return 0;
    }

    run_all_scripts();

    // run_all_scripts() releases the fd itself on a critical abort.
    // Only close it here if it is still held (normal completion).
    if (inhibitor_fd >= 0) {
        syslog(LOG_INFO, "All scripts finished. Releasing inhibitor lock.");
        close(inhibitor_fd);
        inhibitor_fd = -1;
    }

    return 0;
}

static int daemonize(void) {
    pid_t pid;
    int null_fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(EXIT_SUCCESS);

    if (setsid() < 0)
        return -1;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(EXIT_SUCCESS);

    if (chdir("/") < 0)
        return -1;

    null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0)
        return -1;

    if (dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) {
        int saved = errno;
        close(null_fd);
        errno = saved;
        return -1;
    }

    if (null_fd > STDERR_FILENO)
        close(null_fd);

    return 0;
}

int main(int argc, char *argv[]) {
    const char *config_path = NULL;
    const char *selected_config_path = NULL;
    char config_path_buf[PATH_MAX] = {0};
    bool test_mode = false;
    bool foreground = false;
    sd_bus        *bus    = NULL;
    sd_bus_slot   *slot   = NULL;
    sd_bus_message *reply = NULL;
    sd_bus_error   error  = SD_BUS_ERROR_NULL;
    int raw_fd = -1;
    int r = 0;
    int opt;
    int option_index = 0;
    static const struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"test-mode", no_argument, 0, 't'},
        {"foreground", no_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:tf", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 't':
            test_mode = true;
            break;
        case 'f':
            foreground = true;
            break;
        default:
            fprintf(stderr, "Usage: %s [-c PATH] [--config PATH] [--test-mode] [--foreground]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Usage: %s [-c PATH] [--config PATH] [--test-mode] [--foreground]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (config_path && config_path[0] != '/') {
        char cwd[PATH_MAX] = {0};

        if (!getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "terminusd: getcwd() failed: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }

        int n = snprintf(config_path_buf, sizeof(config_path_buf), "%s/%s", cwd, config_path);
        if (n < 0 || (size_t)n >= sizeof(config_path_buf)) {
            fprintf(stderr, "terminusd: config path is too long: %s/%s\n", cwd, config_path);
            return EXIT_FAILURE;
        }

        selected_config_path = config_path_buf;
    } else {
        selected_config_path = config_path;
    }

    if (selected_config_path) {
        snprintf(selected_config_path_runtime, sizeof(selected_config_path_runtime), "%s", selected_config_path);
        selected_config_is_custom = true;
    } else {
        selected_config_is_custom = false;
        selected_config_path_runtime[0] = '\0';
    }

    if (!test_mode && !foreground) {
        if (daemonize() < 0) {
            fprintf(stderr, "terminusd: daemonize() failed: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
    }

    openlog("terminusd", LOG_PID, LOG_DAEMON);

    test_mode_enabled = test_mode;
    control_set_test_mode(test_mode_enabled);
    if (test_mode_enabled)
        test_mode_log(LOG_INFO, "[test-mode] Running in test mode: script and shutdown_guard execution are disabled");

    load_selected_config(selected_config_path);
    ensure_logind_inhibit_delay();

    if (!test_mode_enabled) {
        guard_start();
    } else if (guard_config.enabled && guard_config.command[0] != '\0') {
        test_mode_log(LOG_INFO, "[test-mode] shutdown_guard configured but disabled in test mode: command=\"%s\" type=%s", guard_config.command, guard_config.type == GUARD_TYPE_PERSIST ? "persist" : "oneshot");
    }

    control_listen_fd = control_setup_socket();
    if (control_listen_fd < 0) {
        if (!test_mode_enabled)
            goto finish;

        test_mode_log(LOG_WARNING, "[test-mode] control socket setup failed; continuing without control socket");
    }

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        syslog(LOG_ERR, "Failed to connect to system bus: %s", strerror(-r));
        goto finish;
    }

    if (!test_mode_enabled) {
        r = sd_bus_call_method(
            bus,
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            "Inhibit",
            &error,
            &reply,
            "ssss",
            "shutdown",
            "terminusd",
            "Execute shutdown actions",
            "delay");
        if (r < 0) {
            syslog(LOG_ERR, "Failed to acquire inhibitor lock: %s", error.message ? error.message : strerror(-r));
            goto finish;
        }

        r = sd_bus_message_read(reply, "h", &raw_fd);
        if (r < 0) {
            syslog(LOG_ERR, "Failed to read inhibitor fd: %s", strerror(-r));
            goto finish;
        }

        inhibitor_fd = dup(raw_fd);
        if (inhibitor_fd < 0) {
            r = -errno;
            syslog(LOG_ERR, "Failed to dup inhibitor fd: %s", strerror(errno));
            goto finish;
        }

        sd_bus_message_unref(reply);
        reply = NULL;
    }

    r = sd_bus_match_signal(
        bus,
        &slot,
        test_mode_enabled ? NULL : "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "PrepareForShutdown",
        handle_prepare_for_shutdown,
        NULL);

    if (r < 0) {
        syslog(LOG_ERR, "Failed to subscribe to PrepareForShutdown: %s", strerror(-r));
        goto finish;
    }

    r = sd_bus_flush(bus);
    if (r < 0) {
        syslog(LOG_ERR, "Failed to flush bus after subscription: %s", strerror(-r));
        goto finish;
    }

    if (test_mode_enabled) {
        test_mode_log(LOG_INFO, "Inhibitor running, waiting for PrepareForShutdown signal");
    } else {
        syslog(LOG_INFO, "Inhibitor running, waiting for PrepareForShutdown signal");
    }

    for (;;) {
        for (;;) {
            r = sd_bus_process(bus, NULL);
            if (r < 0) {
                syslog(LOG_ERR, "Bus processing error: %s", strerror(-r));
                break;
            }
            if (r == 0)
                break;
        }
        if (r < 0)
            break;
        if (test_mode_enabled && test_mode_shutdown_seen)
            break;

        int bus_fd = sd_bus_get_fd(bus);
        if (bus_fd < 0) {
            r = bus_fd;
            syslog(LOG_ERR, "Failed to get bus fd: %s", strerror(-r));
            break;
        }

        int bus_events = sd_bus_get_events(bus);
        if (bus_events < 0) {
            r = bus_events;
            syslog(LOG_ERR, "Failed to get bus events: %s", strerror(-r));
            break;
        }

        uint64_t bus_timeout_us = UINT64_MAX;
        r = sd_bus_get_timeout(bus, &bus_timeout_us);
        if (r < 0) {
            syslog(LOG_ERR, "Failed to get bus timeout: %s", strerror(-r));
            break;
        }

        int timeout_ms = -1;
        if (bus_timeout_us != UINT64_MAX) {
            timeout_ms = (int)(bus_timeout_us / 1000);
            if (timeout_ms < 0)
                timeout_ms = 0;
        }

        struct pollfd pfds[3];
        int nfds = 0;
        int guard_idx = -1;
        int control_idx = -1;

        pfds[nfds].fd = bus_fd;
        pfds[nfds].events = POLLIN;
        if (bus_events & EPOLLOUT)
            pfds[nfds].events |= POLLOUT;
        pfds[nfds].revents = 0;
        nfds++;

        {
            int guard_fd = guard_get_fd();
            if (guard_fd >= 0) {
                guard_idx = nfds;
                pfds[nfds].fd = guard_fd;
                pfds[nfds].events = POLLIN | POLLHUP;
                pfds[nfds].revents = 0;
                nfds++;
            }
        }

        if (control_listen_fd >= 0) {
            control_idx = nfds;
            pfds[nfds].fd = control_listen_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        {
            int guard_timeout = guard_poll_timeout_ms();
            if (guard_timeout >= 0 && (timeout_ms < 0 || guard_timeout < timeout_ms))
                timeout_ms = guard_timeout;
        }

        int poll_r = poll(pfds, (nfds_t)nfds, timeout_ms);
        if (poll_r < 0) {
            if (errno == EINTR)
                continue;
            r = -errno;
            syslog(LOG_ERR, "poll() failed: %s", strerror(errno));
            break;
        }

        if (guard_idx >= 0 &&
            (pfds[guard_idx].revents & (POLLIN | POLLHUP | POLLERR)))
            guard_on_readable();

        if (control_idx >= 0 &&
            (pfds[control_idx].revents & (POLLIN | POLLHUP | POLLERR)))
            control_handle_socket_ready(control_listen_fd, selected_config_path_runtime, selected_config_is_custom);

        guard_tick();
    }

finish:
    sd_bus_error_free(&error);
    if (reply)
        sd_bus_message_unref(reply);
    if (slot)
        sd_bus_slot_unref(slot);
    if (inhibitor_fd >= 0)
        close(inhibitor_fd);
    if (control_listen_fd >= 0)
        close(control_listen_fd);
    if (control_listen_fd >= 0)
        control_cleanup_socket();
    if (bus)
        sd_bus_unref(bus);

    closelog();
    return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
