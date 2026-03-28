/* Copyright (c) 2021 Jonathan D. Hall <jon@jonathandavidhall.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "inhibitor.h"

typedef enum {
    ACTION_NONE = 0,
    ACTION_START,
    ACTION_STOP,
    ACTION_STATUS,
    ACTION_RELOAD,
    ACTION_SET_LOGIND_INHIBITOR_DELAY,
    ACTION_GUARD_ENABLE,
    ACTION_GUARD_DISABLE,
    ACTION_SHUTDOWN_ENABLE,
    ACTION_SHUTDOWN_DISABLE,
    ACTION_REBOOT,
    ACTION_SYSTEM_SHUTDOWN
} action_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [options] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  start                 Start the terminusd daemon\n"
        "  stop                  Stop the terminusd daemon\n"
        "  status                Show current daemon runtime status\n"
        "  reload-config         Reload scripts/config while daemon is running\n"
        "  shutdown-guard        enable|disable shutdown guard\n"
        "  shutdown-commands     enable|disable shutdown commands\n"
        "  system-reboot         Request reboot through daemon control path\n"
        "  system-shutdown       Request shutdown through daemon control path\n"
        "\n"
        "Options:\n"
        "  -f, --force           Skip confirmation prompt for reboot/shutdown\n"
        "  --set-logind-inhibitor-delay\n"
        "                        Write daemon's configured max inhibit delay to logind.conf\n"
        "  --skip-scripts        With system-reboot/system-shutdown, skip script execution\n"
        "  -h, --help            Show this help\n",
        argv0);
}

static bool confirm_operation(const char *operation, bool force)
{
    char answer[16];

    if (force)
        return true;

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr,
            "terminusctl: refusing to %s without --force when stdin is not a TTY\n",
            operation);
        return false;
    }

    printf("Proceed with %s? [Y/n]: ", operation);
    fflush(stdout);
    if (!fgets(answer, sizeof(answer), stdin))
        return false;

    if (answer[0] == '\n' || answer[0] == 'y' || answer[0] == 'Y')
        return true;

    return false;
}

static int connect_control_socket(void)
{
    const char *socket_path = getenv("TERMINUSD_CONTROL_SOCKET");

    if (!socket_path || socket_path[0] == '\0')
        socket_path = CONTROL_SOCKET_PATH;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "terminusctl: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
            "terminusctl: connect(%s) failed: %s\n",
            socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int send_command_and_print_result(const char *cmd)
{
    int fd = connect_control_socket();
    if (fd < 0)
        return EXIT_FAILURE;

    size_t cmd_len = strlen(cmd);
    if (write(fd, cmd, cmd_len) != (ssize_t)cmd_len || write(fd, "\n", 1) != 1) {
        fprintf(stderr, "terminusctl: failed writing request: %s\n", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *resp = malloc(cap);
    if (!resp) {
        fprintf(stderr, "terminusctl: out of memory\n");
        close(fd);
        return EXIT_FAILURE;
    }

    for (;;) {
        if (len + 1024 >= cap) {
            size_t new_cap = cap * 2;
            char *new_resp = realloc(resp, new_cap);
            if (!new_resp) {
                fprintf(stderr, "terminusctl: out of memory\n");
                free(resp);
                close(fd);
                return EXIT_FAILURE;
            }
            resp = new_resp;
            cap = new_cap;
        }

        ssize_t nr = read(fd, resp + len, cap - len - 1);
        if (nr < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "terminusctl: read failed: %s\n", strerror(errno));
            free(resp);
            close(fd);
            return EXIT_FAILURE;
        }
        if (nr == 0)
            break;
        len += (size_t)nr;
    }
    close(fd);

    if (len == 0) {
        fprintf(stderr, "terminusctl: empty response from daemon\n");
        free(resp);
        return EXIT_FAILURE;
    }
    resp[len] = '\0';

    if (strncmp(resp, "OK ", 3) == 0) {
        fputs(resp + 3, stdout);
        if (len < 3 || resp[len - 1] != '\n')
            fputc('\n', stdout);
        free(resp);
        return EXIT_SUCCESS;
    }

    if (strncmp(resp, "OK\n", 3) == 0) {
        fputs(resp + 3, stdout);
        if (len < 3 || resp[len - 1] != '\n')
            fputc('\n', stdout);
        free(resp);
        return EXIT_SUCCESS;
    }

    if (strncmp(resp, "ERR ", 4) == 0) {
        fputs(resp + 4, stderr);
        if (len < 4 || resp[len - 1] != '\n')
            fputc('\n', stderr);
        free(resp);
        return EXIT_FAILURE;
    }

    if (strncmp(resp, "ERR\n", 4) == 0) {
        fputs(resp + 4, stderr);
        if (len < 4 || resp[len - 1] != '\n')
            fputc('\n', stderr);
        free(resp);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "terminusctl: malformed response: %s\n", resp);
    free(resp);
    return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
    action_t action = ACTION_NONE;
    bool force = false;
    bool skip_scripts = false;

    static const struct option long_options[] = {
        {"set-logind-inhibitor-delay", no_argument, 0, 'L'},
        {"force", no_argument, 0, 'f'},
        {"skip-scripts", no_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "hfs", long_options,
                  &option_index)) != -1) {
        switch (opt) {
        case 'L':
            action = (action == ACTION_NONE) ? ACTION_SET_LOGIND_INHIBITOR_DELAY : action;
            if (action != ACTION_SET_LOGIND_INHIBITOR_DELAY) {
                fprintf(stderr, "terminusctl: only one primary action can be specified\n");
                return EXIT_FAILURE;
            }
            break;
        case 'f':
            force = true;
            break;
        case 's':
            skip_scripts = true;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        const char *cmd = argv[optind++];

        if (action != ACTION_NONE) {
            fprintf(stderr, "terminusctl: cannot combine command with option action\n");
            return EXIT_FAILURE;
        }

        if (strcmp(cmd, "start") == 0) {
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            action = ACTION_START;
        } else if (strcmp(cmd, "stop") == 0) {
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            action = ACTION_STOP;
        } else if (strcmp(cmd, "status") == 0) {
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            action = ACTION_STATUS;
        } else if (strcmp(cmd, "reload-config") == 0) {
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            action = ACTION_RELOAD;
        } else if (strcmp(cmd, "shutdown-guard") == 0) {
            if (optind >= argc) {
                fprintf(stderr, "terminusctl: shutdown-guard requires enable or disable\n");
                return EXIT_FAILURE;
            }
            if (strcmp(argv[optind], "enable") == 0)
                action = ACTION_GUARD_ENABLE;
            else if (strcmp(argv[optind], "disable") == 0)
                action = ACTION_GUARD_DISABLE;
            else {
                fprintf(stderr, "terminusctl: shutdown-guard requires enable or disable\n");
                return EXIT_FAILURE;
            }
            optind++;
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(cmd, "shutdown-commands") == 0) {
            if (optind >= argc) {
                fprintf(stderr, "terminusctl: shutdown-commands requires enable or disable\n");
                return EXIT_FAILURE;
            }
            if (strcmp(argv[optind], "enable") == 0)
                action = ACTION_SHUTDOWN_ENABLE;
            else if (strcmp(argv[optind], "disable") == 0)
                action = ACTION_SHUTDOWN_DISABLE;
            else {
                fprintf(stderr, "terminusctl: shutdown-commands requires enable or disable\n");
                return EXIT_FAILURE;
            }
            optind++;
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(cmd, "system-reboot") == 0) {
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            action = ACTION_REBOOT;
        } else if (strcmp(cmd, "system-shutdown") == 0) {
            if (optind != argc) {
                usage(argv[0]);
                return EXIT_FAILURE;
            }
            action = ACTION_SYSTEM_SHUTDOWN;
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (action == ACTION_NONE) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (skip_scripts && action != ACTION_REBOOT && action != ACTION_SYSTEM_SHUTDOWN) {
        fprintf(stderr,
            "terminusctl: --skip-scripts is only valid with system-reboot or system-shutdown\n");
        return EXIT_FAILURE;
    }

    if (force && action != ACTION_REBOOT && action != ACTION_SYSTEM_SHUTDOWN) {
        fprintf(stderr,
            "terminusctl: --force is only valid with system-reboot or system-shutdown\n");
        return EXIT_FAILURE;
    }

    switch (action) {
    case ACTION_START:
        execlp("systemctl", "systemctl", "start", "terminusd", (char *)NULL);
        fprintf(stderr, "terminusctl: exec systemctl: %s\n", strerror(errno));
        return EXIT_FAILURE;
    case ACTION_STOP:
        execlp("systemctl", "systemctl", "stop", "terminusd", (char *)NULL);
        fprintf(stderr, "terminusctl: exec systemctl: %s\n", strerror(errno));
        return EXIT_FAILURE;
    case ACTION_STATUS:
        return send_command_and_print_result("STATUS");
    case ACTION_RELOAD:
        return send_command_and_print_result("RELOAD");
    case ACTION_SET_LOGIND_INHIBITOR_DELAY:
        return send_command_and_print_result("SET_LOGIND_INHIBITOR_DELAY");
    case ACTION_GUARD_ENABLE:
        return send_command_and_print_result("GUARD ENABLE");
    case ACTION_GUARD_DISABLE:
        return send_command_and_print_result("GUARD DISABLE");
    case ACTION_SHUTDOWN_ENABLE:
        return send_command_and_print_result("SHUTDOWN ENABLE");
    case ACTION_SHUTDOWN_DISABLE:
        return send_command_and_print_result("SHUTDOWN DISABLE");
    case ACTION_REBOOT:
        if (!confirm_operation("system reboot", force)) {
            fprintf(stderr, "terminusctl: reboot cancelled\n");
            return EXIT_FAILURE;
        }
        return send_command_and_print_result(skip_scripts ? "REBOOT SKIP" : "REBOOT");
    case ACTION_SYSTEM_SHUTDOWN:
        if (!confirm_operation("system shutdown", force)) {
            fprintf(stderr, "terminusctl: shutdown cancelled\n");
            return EXIT_FAILURE;
        }
        return send_command_and_print_result(skip_scripts ? "POWEROFF SKIP" : "POWEROFF");
    default:
        break;
    }

    return EXIT_FAILURE;
}
