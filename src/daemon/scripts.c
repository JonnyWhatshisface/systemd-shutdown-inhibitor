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

extern char **environ;

int cmp_priority(const void *a, const void *b) {
    const script_entry_t *sa = (const script_entry_t *)a;
    const script_entry_t *sb = (const script_entry_t *)b;

    if (sa->priority < sb->priority)
        return -1;
    if (sa->priority > sb->priority)
        return 1;
    return strcmp(sa->name, sb->name);
}

char **parse_command_argv(const char *command, int *argv_count) {
    // Parses command strings into argv array.
    if (!command || command[0] == '\0') {
        *argv_count = 0;
        return NULL;
    }

    // Make a copy we can modify
    char *cmd_copy = strdup(command);
    if (!cmd_copy) {
        *argv_count = 0;
        return NULL;
    }

    // Count arguments
    int count = 0;
    bool in_arg = false;
    for (char *p = cmd_copy; *p; p++) {
        if (*p == ' ' || *p == '\t') {
            in_arg = false;
        } else if (!in_arg) {
            count++;
            in_arg = true;
        }
    }

    if (count == 0) {
        free(cmd_copy);
        *argv_count = 0;
        return NULL;
    }

    // Allocate argv array with +1 for NULL terminator
    char **argv = malloc((size_t)(count + 1) * sizeof(char *));
    if (!argv) {
        free(cmd_copy);
        *argv_count = 0;
        return NULL;
    }

    // Parse arguments
    int idx = 0;
    char *saveptr = NULL;
    char *token = strtok_r(cmd_copy, " \t", &saveptr);
    while (token && idx < count) {
        argv[idx++] = strdup(token);
        if (!argv[idx - 1]) {
            // Allocation failed!
            for (int i = 0; i < idx - 1; i++)
                free(argv[i]);
            free(argv);
            free(cmd_copy);
            *argv_count = 0;
            return NULL;
        }
        token = strtok_r(NULL, " \t", &saveptr);
    }
    argv[idx] = NULL;

    free(cmd_copy);
    *argv_count = idx;
    return argv;
}

void free_argv(char **argv) {
    if (!argv)
        return;
    for (int i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
}

static int lookup_uid(const char *value, uid_t *uid, gid_t *primary_gid, bool *have_primary_gid, char *resolved_user, size_t resolved_user_len) {
    struct passwd *pwd;
    char *endptr = NULL;
    unsigned long parsed;

    *have_primary_gid = false;
    if (resolved_user_len > 0)
        resolved_user[0] = '\0';

    if (value[0] == '\0')
        return -1;

    pwd = getpwnam(value);
    if (pwd) {
        *uid = pwd->pw_uid;
        *primary_gid = pwd->pw_gid;
        *have_primary_gid = true;
        if (resolved_user_len > 0) {
            strncpy(resolved_user, pwd->pw_name, resolved_user_len - 1);
            resolved_user[resolved_user_len - 1] = '\0';
        }
        return 0;
    }

    errno = 0;
    parsed = strtoul(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' || parsed > (unsigned long)((uid_t)-1))
        return -1;

    *uid = (uid_t)parsed;

    pwd = getpwuid(*uid);
    if (pwd) {
        *primary_gid = pwd->pw_gid;
        *have_primary_gid = true;
        if (resolved_user_len > 0) {
            strncpy(resolved_user, pwd->pw_name, resolved_user_len - 1);
            resolved_user[resolved_user_len - 1] = '\0';
        }
    }

    return 0;
}

static int lookup_gid(const char *value, gid_t *gid) {
    struct group *grp;
    char *endptr = NULL;
    unsigned long parsed;

    if (value[0] == '\0')
        return -1;

    grp = getgrnam(value);
    if (grp) {
        *gid = grp->gr_gid;
        return 0;
    }

    errno = 0;
    parsed = strtoul(value, &endptr, 10);
    if (errno != 0 || endptr == value || *endptr != '\0' || parsed > (unsigned long)((gid_t)-1))
        return -1;

    *gid = (gid_t)parsed;
    return 0;
}

int apply_entry_credentials(const script_entry_t *e) {
    uid_t target_uid = 0;
    gid_t target_gid = 0;
    gid_t primary_gid = 0;
    bool have_user      = e->user[0]  != '\0';
    bool have_group     = e->group[0] != '\0';
    bool have_primary_gid = false;
    char resolved_user[SCRIPT_IDENTITY_LEN] = "";

    if (!have_user && !have_group)
        return 0;

    if (have_user && lookup_uid(e->user, &target_uid, &primary_gid, &have_primary_gid, resolved_user, sizeof(resolved_user)) < 0) {
        fprintf(stderr, "[%s] Invalid user '%s'\n", e->name, e->user);
        return -1;
    }

    if (have_group) {
        if (lookup_gid(e->group, &target_gid) < 0) {
            fprintf(stderr, "[%s] Invalid group '%s'\n", e->name, e->group);
            return -1;
        }
    } else if (have_user) {
        if (!have_primary_gid) {
            fprintf(stderr, "[%s] user '%s' requires group when no passwd entry exists\n", e->name, e->user);
            return -1;
        }
        target_gid = primary_gid;
    }

    if (have_user) {
        if (resolved_user[0] != '\0') {
            if (initgroups(resolved_user, target_gid) < 0) {
                fprintf(stderr, "[%s] initgroups(%s) failed: %s\n", e->name, resolved_user, strerror(errno));
                return -1;
            }
        } else if (setgroups(0, NULL) < 0) {
            fprintf(stderr, "[%s] setgroups() failed: %s\n", e->name, strerror(errno));
            return -1;
        }
    }

    if (have_group) {
        if (setgid(target_gid) < 0) {
            fprintf(stderr, "[%s] setgid(%lu) failed: %s\n", e->name, (unsigned long)target_gid, strerror(errno));
            return -1;
        }
    } else if (have_user) {
        if (setgid(primary_gid) < 0) {
            fprintf(stderr, "[%s] setgid(%lu) failed: %s\n", e->name, (unsigned long)primary_gid, strerror(errno));
            return -1;
        }
    }

    if (have_user && setuid(target_uid) < 0) {
        fprintf(stderr, "[%s] setuid(%lu) failed: %s\n", e->name, (unsigned long)target_uid, strerror(errno));
        return -1;
    }

    return 0;
}

char **load_entry_env(const script_entry_t *e){
    if (e->env[0] == '\0')
        return NULL;

    FILE *f = fopen(e->env, "r");
    if (!f) {
        syslog(LOG_WARNING, "[%s] Failed to open env file %s: %s", e->name, e->env, strerror(errno));
        return NULL;
    }

    char line[PATH_MAX + 64];
    char **env_array = NULL;
    int env_count    = 0;
    int env_capacity = 10;

    env_array = malloc(env_capacity * sizeof(char *));
    if (!env_array) {
        syslog(LOG_ERR, "[%s] malloc failed for env array", e->name);
        fclose(f);
        return NULL;
    }

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = trim_leading(line);
        if (*p == '#' || *p == '\0')
            continue;
        if (!strchr(p, '='))
            continue;

        if (env_count >= env_capacity - 1) {
            env_capacity *= 2;
            char **tmp = realloc(env_array,
                      env_capacity * sizeof(char *));
            if (!tmp) {
                syslog(LOG_ERR, "[%s] realloc failed for env array", e->name);
                for (int i = 0; i < env_count; i++)
                    free(env_array[i]);
                free(env_array);
                fclose(f);
                return NULL;
            }
            env_array = tmp;
        }

        env_array[env_count] = strdup(p);
        if (!env_array[env_count]) {
            syslog(LOG_ERR, "[%s] strdup failed for env entry", e->name);
            for (int i = 0; i < env_count; i++)
                free(env_array[i]);
            free(env_array);
            fclose(f);
            return NULL;
        }
        env_count++;
    }

    fclose(f);
    env_array[env_count] = NULL;
    syslog(LOG_INFO, "[%s] Loaded %d environment variables from %s", e->name, env_count, e->env);
    return env_array;
}

void free_env_array(char **env) {
    if (!env)
        return;
    for (int i = 0; env[i]; i++)
        free(env[i]);
    free(env);
}

static pid_t launch_entry(const script_entry_t *e, int out_pipe[2]) {
    if (pipe(out_pipe) < 0) {
        syslog(LOG_ERR, "[%s] pipe() failed: %s", e->name, strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "[%s] fork() failed: %s", e->name, strerror(errno));
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(out_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0 || dup2(out_pipe[1], STDERR_FILENO) < 0)
            _exit(126);
        if (out_pipe[1] > STDERR_FILENO)
            close(out_pipe[1]);

        if (apply_entry_credentials(e) < 0)
            _exit(126);

        char **env = load_entry_env(e);
        bool env_is_custom = (env != NULL);
        if (!env)
            env = environ;

        int argv_count = 0;
        char **argv = parse_command_argv(e->command, &argv_count);
        if (!argv || argv_count == 0) {
            if (env_is_custom)
                free_env_array(env);
            fprintf(stderr, "[%s] Failed to parse command: \"%s\"\n", e->name, e->command);
            _exit(127);
        }

        execve(argv[0], argv, env);
        int saved_errno = errno;
        if (env_is_custom)
            free_env_array(env);
        fprintf(stderr, "[%s] execve(\"%s\") failed: %s\n", e->name, argv[0], strerror(saved_errno));
        free_argv(argv);
        _exit(127);
    }

    close(out_pipe[1]);
    return pid;
}

static int cmp_script_index(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const script_entry_t *sa = &scripts[ia];
    const script_entry_t *sb = &scripts[ib];

    if (sa->priority < sb->priority)
        return -1;
    if (sa->priority > sb->priority)
        return 1;
    return strcmp(sa->name, sb->name);
}

void run_all_scripts(void) {
    if (script_count == 0) {
        syslog(LOG_WARNING, "PrepareForShutdown received but no scripts configured");
        return;
    }

    int order[MAX_SCRIPTS];
    for (int idx = 0; idx < script_count; idx++)
        order[idx] = idx;
    qsort(order, (size_t)script_count, sizeof(order[0]),
            cmp_script_index);

    int i = 0;
    while (i < script_count) {
          unsigned int cur_prio = scripts[order[i]].priority;

        int j = i;
        while (j < script_count &&
              scripts[order[j]].priority == cur_prio)
            j++;

        int group_size = j - i;
        running_t running[MAX_SCRIPTS];
        memset(running, 0, sizeof(running));

        for (int k = 0; k < group_size; k++) {
            const script_entry_t *e = &scripts[order[i + k]];

            if (e->command[0] == '\0') {
                syslog(LOG_WARNING, "[%s] command not set; skipping", e->name);
                running[k].pid = -1;
                running[k].fd  = -1;
                strncpy(running[k].name, e->name, sizeof(running[k].name) - 1);
                running[k].name[sizeof(running[k].name) - 1] = '\0';
                continue;
            }

            syslog(LOG_INFO, "[%s] Launching command=\"%s\" priority=%u user=%s group=%s", e->name, e->command, e->priority, e->user[0]  ? e->user  : "<daemon>", e->group[0] ? e->group : "<daemon>");

            int out_pipe[2];
            pid_t pid = launch_entry(e, out_pipe);
            running[k].pid       = pid;
            running[k].critical  = e->critical;
            running[k].exit_code = 0;
            strncpy(running[k].name, e->name, sizeof(running[k].name) - 1);
            running[k].name[sizeof(running[k].name) - 1] = '\0';

            if (pid > 0) {
                running[k].fd = out_pipe[0];
            } else {
                close(out_pipe[0]);
                running[k].fd = -1;
                // A launch failure counts as a critical failure
                if (e->critical)
                    running[k].exit_code = -1;
            }
        }

        struct pollfd pfds[MAX_SCRIPTS];
        int open_count = 0;

        for (int k = 0; k < group_size; k++) {
            pfds[k].fd     = running[k].fd;
            pfds[k].events = (running[k].fd >= 0) ? POLLIN : 0;
            if (running[k].fd >= 0)
                open_count++;
        }

        while (open_count > 0) {
            int ret = poll(pfds, (nfds_t)group_size, -1);
            if (ret < 0) {
                if (errno == EINTR)
                    continue;
                syslog(LOG_ERR, "poll() failed: %s", strerror(errno));
                break;
            }

            for (int k = 0; k < group_size; k++) {
                if (pfds[k].fd < 0)
                    continue;
                if (!(pfds[k].revents & (POLLIN | POLLHUP)))
                    continue;

                char tmp[512];
                ssize_t n = read(pfds[k].fd, tmp, sizeof(tmp));
                if (n <= 0) {
                    if (running[k].linelen > 0) {
                        running[k].linebuf[running[k].linelen] = '\0';
                        syslog(LOG_INFO, "[%s] %s", running[k].name, running[k].linebuf);
                        running[k].linelen = 0;
                    }
                    close(pfds[k].fd);
                    pfds[k].fd    = -1;
                    running[k].fd = -1;
                    open_count--;
                    continue;
                }

                for (ssize_t ci = 0; ci < n; ci++) {
                    char c = tmp[ci];
                    if (c == '\n' || c == '\r') {
                        running[k].linebuf[running[k].linelen] = '\0';
                        syslog(LOG_INFO, "[%s] %s", running[k].name, running[k].linebuf);
                        running[k].linelen = 0;
                    } else if (running[k].linelen < (int)sizeof(running[k].linebuf) - 1) {
                        running[k].linebuf[running[k].linelen++] = c;
                    }
                }
            }
        }

        bool critical_failed = false;
        for (int k = 0; k < group_size; k++) {
            if (running[k].pid <= 0)
                continue;

            int status;
            int wret;
            do {
                wret = waitpid(running[k].pid, &status, 0);
            } while (wret < 0 && errno == EINTR);

            if (wret < 0) {
                syslog(LOG_ERR, "[%s] waitpid() failed: %s", running[k].name, strerror(errno));
            } else if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                running[k].exit_code = code;
                if (code == 0)
                    syslog(LOG_INFO, "[%s] finished successfully", running[k].name);
                else {
                    syslog(LOG_WARNING, "[%s] exited with status %d", running[k].name, code);

                    if (running[k].critical) {
                        syslog(LOG_ERR,
                              "[%s] critical action failed (exit_code=%d); aborting remaining priority groups", running[k].name, running[k].exit_code);
                        critical_failed = true;
                    }
                }
            } else if (WIFSIGNALED(status)) {
                running[k].exit_code = -1;
                syslog(LOG_WARNING,
                      "[%s] killed by signal %d",
                      running[k].name, WTERMSIG(status));

                if (running[k].critical) {
                    syslog(LOG_ERR,
                          "[%s] critical action failed (signal=%d); aborting remaining priority groups", running[k].name, WTERMSIG(status));
                    critical_failed = true;
                }
            }
        }

        // Scripts within a group always run in parallel regardless of
        // the critical flag; only the transition to the next group is
        // gated on critical failures.
        if (critical_failed) {
            syslog(LOG_ERR, "Critical action failure in priority group %u — releasing inhibitor lock without running further scripts", cur_prio);
            if (inhibitor_fd >= 0) {
                close(inhibitor_fd);
                inhibitor_fd = -1;
            }
            return;
        }

        i = j;
    }
}

