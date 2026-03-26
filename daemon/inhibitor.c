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


static int load_config(const char *path)
{
        FILE *f = fopen(path, "r");
        if (!f)
                return -1;

        char line[PATH_MAX + 64];
        while (fgets(line, sizeof(line), f)) {
                // Strip line ending
                line[strcspn(line, "\r\n")] = '\0';

                // Skip leading whitespace, blank lines, and comments
                char *p = line + strspn(line, " \t");
                if (*p == '#' || *p == '\0')
                        continue;

                // Split on the first '='.
                char *eq = strchr(p, '=');
                if (!eq)
                        continue;
                *eq = '\0';

                char *key   = p;
                char *value = eq + 1;

                // Trim trailing whitespace from key.
                char *end = key + strlen(key);
                while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
                        *--end = '\0';

                // Trim leading and trailing whitespace from value.
                value += strspn(value, " \t");
                end = value + strlen(value);
                while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
                        *--end = '\0';

                if (strcmp(key, "shutdown_script") == 0) {
                        strncpy(shutdown_script, value,
                                sizeof(shutdown_script) - 1);
                        shutdown_script[sizeof(shutdown_script) - 1] = '\0';
                } else if (strcmp(key, "shutdown_script_user") == 0) {
                        strncpy(shutdown_script_user, value,
                                sizeof(shutdown_script_user) - 1);
                        shutdown_script_user[sizeof(shutdown_script_user) - 1] = '\0';
                } else if (strcmp(key, "shutdown_script_group") == 0) {
                        strncpy(shutdown_script_group, value,
                                sizeof(shutdown_script_group) - 1);
                        shutdown_script_group[sizeof(shutdown_script_group) - 1] = '\0';
                } else if (strcmp(key, "shutdown_script_env") == 0) {
                        strncpy(shutdown_script_env, value,
                                sizeof(shutdown_script_env) - 1);
                        shutdown_script_env[sizeof(shutdown_script_env) - 1] = '\0';
                } else if (strcmp(key, "set_max_inhibit_delay") == 0) {
                        if (strcmp(value, "true") == 0 ||
                            strcmp(value, "yes") == 0 ||
                            strcmp(value, "1") == 0) {
                                set_max_inhibit_delay = true;
                        } else if (strcmp(value, "false") == 0 ||
                                   strcmp(value, "no") == 0 ||
                                   strcmp(value, "0") == 0) {
                                set_max_inhibit_delay = false;
                        } else {
                                syslog(LOG_WARNING,
                                       "Invalid set_max_inhibit_delay value '%s' in %s; using false",
                                       value,
                                       path);
                                set_max_inhibit_delay = false;
                        }
                } else if (strcmp(key, "restart_logind_after_set") == 0) {
                        if (strcmp(value, "true") == 0 ||
                            strcmp(value, "yes") == 0 ||
                            strcmp(value, "1") == 0) {
                                restart_logind_after_set = true;
                        } else if (strcmp(value, "false") == 0 ||
                                   strcmp(value, "no") == 0 ||
                                   strcmp(value, "0") == 0) {
                                restart_logind_after_set = false;
                        } else {
                                syslog(LOG_WARNING,
                                       "Invalid restart_logind_after_set value '%s' in %s; using false",
                                       value,
                                       path);
                                restart_logind_after_set = false;
                        }
                } else if (strcmp(key, "max_inhibit_delay") == 0) {
                        char *endptr = NULL;
                        errno = 0;
                        long parsed = strtol(value, &endptr, 10);
                        if (errno == 0 && endptr != value && *endptr == '\0' &&
                            parsed > 0 && parsed <= INT_MAX) {
                                max_inhibit_delay = (int)parsed;
                        } else {
                                syslog(LOG_WARNING,
                                       "Invalid max_inhibit_delay value '%s' in %s; using %d",
                                       value,
                                       path,
                                       max_inhibit_delay);
                        }
                }
        }

        fclose(f);
        return 0;
}

static int lookup_uid(const char *value, uid_t *uid, gid_t *primary_gid,
                      bool *have_primary_gid, char *resolved_user,
                      size_t resolved_user_len)
{
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
                        strncpy(resolved_user, pwd->pw_name,
                                resolved_user_len - 1);
                        resolved_user[resolved_user_len - 1] = '\0';
                }
                return 0;
        }

        errno = 0;
        parsed = strtoul(value, &endptr, 10);
        if (errno != 0 || endptr == value || *endptr != '\0' ||
            parsed > (unsigned long)((uid_t)-1))
                return -1;

        *uid = (uid_t)parsed;

        pwd = getpwuid(*uid);
        if (pwd) {
                *primary_gid = pwd->pw_gid;
                *have_primary_gid = true;
                if (resolved_user_len > 0) {
                        strncpy(resolved_user, pwd->pw_name,
                                resolved_user_len - 1);
                        resolved_user[resolved_user_len - 1] = '\0';
                }
        }

        return 0;
}

static int lookup_gid(const char *value, gid_t *gid)
{
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
        if (errno != 0 || endptr == value || *endptr != '\0' ||
            parsed > (unsigned long)((gid_t)-1))
                return -1;

        *gid = (gid_t)parsed;
        return 0;
}

static int apply_script_credentials(void)
{
        uid_t target_uid = 0;
        gid_t target_gid = 0;
        gid_t primary_gid = 0;
        bool have_user = shutdown_script_user[0] != '\0';
        bool have_group = shutdown_script_group[0] != '\0';
        bool have_primary_gid = false;
        char resolved_user[SCRIPT_IDENTITY_LEN] = "";

        if (!have_user && !have_group)
                return 0;

        if (have_user && lookup_uid(shutdown_script_user, &target_uid,
                                    &primary_gid, &have_primary_gid,
                                    resolved_user,
                                    sizeof(resolved_user)) < 0) {
                fprintf(stderr, "Invalid shutdown_script_user '%s'\n",
                        shutdown_script_user);
                return -1;
        }

        if (have_group) {
                if (lookup_gid(shutdown_script_group, &target_gid) < 0) {
                        fprintf(stderr, "Invalid shutdown_script_group '%s'\n",
                                shutdown_script_group);
                        return -1;
                }
        } else if (have_user) {
                if (!have_primary_gid) {
                        fprintf(stderr,
                                "shutdown_script_user '%s' requires shutdown_script_group when no passwd entry exists\n",
                                shutdown_script_user);
                        return -1;
                }
                target_gid = primary_gid;
        }

        if (have_user) {
                if (resolved_user[0] != '\0') {
                        if (initgroups(resolved_user, target_gid) < 0) {
                                fprintf(stderr,
                                        "initgroups(%s) failed: %s\n",
                                        resolved_user,
                                        strerror(errno));
                                return -1;
                        }
                } else if (setgroups(0, NULL) < 0) {
                        fprintf(stderr,
                                "setgroups() failed: %s\n",
                                strerror(errno));
                        return -1;
                }
        }

        if (have_group) {
                if (setgid(target_gid) < 0) {
                        fprintf(stderr, "setgid(%lu) failed: %s\n",
                                (unsigned long)target_gid,
                                strerror(errno));
                        return -1;
                }
        } else if (have_user) {
                if (setgid(primary_gid) < 0) {
                        fprintf(stderr, "setgid(%lu) failed: %s\n",
                                (unsigned long)primary_gid,
                                strerror(errno));
                        return -1;
                }
        }

        if (have_user && setuid(target_uid) < 0) {
                fprintf(stderr, "setuid(%lu) failed: %s\n",
                        (unsigned long)target_uid,
                        strerror(errno));
                return -1;
        }

        return 0;
}

static bool is_active_inhibit_delay_line(const char *line)
{
        const char *p = line + strspn(line, " \t");
        size_t key_len = strlen("InhibitDelayMaxSec");

        if (*p == '#' || *p == '\0')
                return false;
        if (strncmp(p, "InhibitDelayMaxSec", key_len) != 0)
                return false;

        p += key_len;
        p += strspn(p, " \t");
        return *p == '=';
}

static int parse_inhibit_delay_value(const char *line, int *value)
{
        const char *p = line + strspn(line, " \t");
        size_t key_len = strlen("InhibitDelayMaxSec");
        char *endptr = NULL;
        long parsed;

        if (!is_active_inhibit_delay_line(line))
                return 0;

        p += key_len;
        p += strspn(p, " \t");
        p++; /* skip '=' */
        p += strspn(p, " \t");

        errno = 0;
        parsed = strtol(p, &endptr, 10);
        if (errno != 0 || endptr == p || parsed <= 0 || parsed > INT_MAX)
                return -1;

        while (*endptr == ' ' || *endptr == '\t')
                endptr++;
        if (*endptr != '\0' && *endptr != '\n' && *endptr != '\r')
                return -1;

        *value = (int)parsed;
        return 1;
}

static int restart_logind_service(void)
{
        int pipefd[2];
        if (pipe(pipefd) < 0) {
                syslog(LOG_ERR, "pipe() failed for logind restart: %s",
                       strerror(errno));
                return -1;
        }

        pid_t pid = fork();
        if (pid < 0) {
                syslog(LOG_ERR, "fork() failed for logind restart: %s",
                       strerror(errno));
                close(pipefd[0]);
                close(pipefd[1]);
                return -1;
        }

        if (pid == 0) {
                close(pipefd[0]);
                if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
                    dup2(pipefd[1], STDERR_FILENO) < 0) {
                        _exit(126);
                }
                if (pipefd[1] > STDERR_FILENO)
                        close(pipefd[1]);

                execl("/usr/bin/systemctl", "systemctl", "restart",
                      "systemd-logind.service", (char *)NULL);
                execl("/bin/systemctl", "systemctl", "restart",
                      "systemd-logind.service", (char *)NULL);

                fprintf(stderr, "exec(systemctl) failed: %s\n",
                        strerror(errno));
                _exit(127);
        }

        close(pipefd[1]);
        FILE *fp = fdopen(pipefd[0], "r");
        if (fp) {
                char buf[1024];
                while (fgets(buf, sizeof(buf), fp)) {
                        buf[strcspn(buf, "\r\n")] = '\0';
                        syslog(LOG_INFO, "[logind-restart] %s", buf);
                }
                fclose(fp);
        } else {
                close(pipefd[0]);
        }

        int status;
        if (waitpid(pid, &status, 0) < 0) {
                syslog(LOG_ERR, "waitpid() failed for logind restart: %s",
                       strerror(errno));
                return -1;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                syslog(LOG_INFO, "systemd-logind restarted successfully");
                return 0;
        }
        if (WIFEXITED(status)) {
                syslog(LOG_ERR,
                       "systemd-logind restart failed with exit status %d",
                       WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
                syslog(LOG_ERR,
                       "systemd-logind restart killed by signal %d",
                       WTERMSIG(status));
        }

        return -1;
}

static void ensure_logind_inhibit_delay(void)
{
        FILE *f = fopen(LOGIND_CONF, "r");
        int current = -1;
        bool found = false;
        char line[1024];

        if (!set_max_inhibit_delay) {
                syslog(LOG_INFO,
                       "set_max_inhibit_delay is false or unset; leaving %s unchanged",
                       LOGIND_CONF);
                return;
        }

        if (!f) {
                syslog(LOG_ERR, "Failed to read %s: %s", LOGIND_CONF,
                       strerror(errno));
                return;
        }

        while (fgets(line, sizeof(line), f)) {
                int parsed = -1;
                int state = parse_inhibit_delay_value(line, &parsed);
                if (state == 1) {
                        found = true;
                        current = parsed;
                } else if (state == -1 && is_active_inhibit_delay_line(line)) {
                        found = true;
                        current = -1;
                        syslog(LOG_WARNING,
                               "Malformed InhibitDelayMaxSec line in %s",
                               LOGIND_CONF);
                }
        }
        fclose(f);

        if (found && current == max_inhibit_delay) {
                syslog(LOG_INFO,
                       "InhibitDelayMaxSec already set to %d",
                       max_inhibit_delay);
                return;
        }

        syslog(LOG_INFO,
               "Updating InhibitDelayMaxSec to %d in %s",
               max_inhibit_delay,
               LOGIND_CONF);

        f = fopen(LOGIND_CONF, "r");
        if (!f) {
                syslog(LOG_ERR, "Failed to reopen %s for update: %s",
                       LOGIND_CONF,
                       strerror(errno));
                return;
        }

        struct stat st;
        if (fstat(fileno(f), &st) < 0) {
                syslog(LOG_ERR, "fstat(%s) failed: %s", LOGIND_CONF,
                       strerror(errno));
                fclose(f);
                return;
        }

        char tmp_path[] = "/etc/systemd/logind.conf.tmpXXXXXX";
        int tmp_fd = mkstemp(tmp_path);
        if (tmp_fd < 0) {
                syslog(LOG_ERR, "mkstemp() failed: %s", strerror(errno));
                fclose(f);
                return;
        }

        if (fchmod(tmp_fd, st.st_mode & 0777) < 0) {
                syslog(LOG_WARNING, "fchmod(%s) failed: %s", tmp_path,
                       strerror(errno));
        }
        if (fchown(tmp_fd, st.st_uid, st.st_gid) < 0) {
                syslog(LOG_WARNING, "fchown(%s) failed: %s", tmp_path,
                       strerror(errno));
        }

        FILE *tmp = fdopen(tmp_fd, "w");
        if (!tmp) {
                syslog(LOG_ERR, "fdopen() failed for temp file: %s",
                       strerror(errno));
                close(tmp_fd);
                unlink(tmp_path);
                fclose(f);
                return;
        }

        bool wrote_key = false;
        while (fgets(line, sizeof(line), f)) {
                if (is_active_inhibit_delay_line(line)) {
                        if (!wrote_key) {
                                fprintf(tmp, "InhibitDelayMaxSec=%d\n",
                                        max_inhibit_delay);
                                wrote_key = true;
                        }
                        continue;
                }

                fputs(line, tmp);
        }

        if (!wrote_key)
                fprintf(tmp, "\nInhibitDelayMaxSec=%d\n", max_inhibit_delay);

        if (fclose(tmp) != 0) {
                syslog(LOG_ERR, "Failed to write temp logind config: %s",
                       strerror(errno));
                unlink(tmp_path);
                fclose(f);
                return;
        }
        fclose(f);

        if (rename(tmp_path, LOGIND_CONF) < 0) {
                syslog(LOG_ERR, "Failed to replace %s: %s", LOGIND_CONF,
                       strerror(errno));
                unlink(tmp_path);
                return;
        }

        if (restart_logind_after_set) {
                restart_logind_service();
        } else {
                syslog(LOG_INFO,
                       "Updated %s without restarting systemd-logind.service because restart_logind_after_set is false or unset",
                       LOGIND_CONF);
        }
}

static void load_selected_config(const char *config_path)
{
        const char *path = config_path ? config_path : CONF_SYSTEM;

        if (load_config(path) == 0) {
                syslog(LOG_INFO, "Loaded config from %s", path);
                return;
        }

        syslog(LOG_WARNING,
               "No config file found at %s; shutdown_script not set",
               path);
}


static char **load_script_env(void)
{
        /*
           Load environment variables from a file.
           Format: One "KEY=VALUE" pair per line.
           Allows for blank lines and comment lines (#)
        */
        if (shutdown_script_env[0] == '\0')
                return NULL;

        FILE *f = fopen(shutdown_script_env, "r");
        if (!f) {
                syslog(LOG_WARNING, "Failed to open env file %s: %s",
                       shutdown_script_env, strerror(errno));
                return NULL;
        }

        char line[PATH_MAX + 64];
        char **env_array = NULL;
        int env_count = 0;
        int env_capacity = 10;

        env_array = malloc(env_capacity * sizeof(char *));
        if (!env_array) {
                syslog(LOG_ERR, "malloc failed for env array");
                fclose(f);
                return NULL;
        }

        while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = '\0';

                char *p = line + strspn(line, " \t");
                if (*p == '#' || *p == '\0')
                        continue;

                if (!strchr(p, '='))
                        continue;

                if (env_count >= env_capacity - 1) {
                        env_capacity *= 2;
                        char **new_array = realloc(env_array,
                                                   env_capacity * sizeof(char *));
                        if (!new_array) {
                                syslog(LOG_ERR, "realloc failed for env array");
                                for (int i = 0; i < env_count; i++)
                                        free(env_array[i]);
                                free(env_array);
                                fclose(f);
                                return NULL;
                        }
                        env_array = new_array;
                }

                env_array[env_count] = malloc(strlen(p) + 1);
                if (!env_array[env_count]) {
                        syslog(LOG_ERR, "malloc failed for env entry");
                        for (int i = 0; i < env_count; i++)
                                free(env_array[i]);
                        free(env_array);
                        fclose(f);
                        return NULL;
                }
                strcpy(env_array[env_count], p);
                env_count++;
        }

        fclose(f);

        env_array[env_count] = NULL;

        syslog(LOG_INFO, "Loaded %d environment variables from %s",
               env_count, shutdown_script_env);

        return env_array;
}

static void run_shutdown_script(const char *script)
{
        int pipefd[2];
        if (pipe(pipefd) < 0) {
                syslog(LOG_ERR, "pipe() failed: %s", strerror(errno));
                return;
        }

        pid_t pid = fork();
        if (pid < 0) {
                syslog(LOG_ERR, "fork() failed: %s", strerror(errno));
                close(pipefd[0]);
                close(pipefd[1]);
                return;
        }

        if (pid == 0) {
                /* 
                   Child Process. 
                   We need stdout and stderr for the pipe write end. 
                */
                close(pipefd[0]);
                if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
                    dup2(pipefd[1], STDERR_FILENO) < 0) {
                        _exit(126);
                }
                /* 
                   Close the original write-end fd if it isn't one we just
                   dup2'd onto (avoids double-close of a low-numbered fd).
                */
                if (pipefd[1] > STDERR_FILENO)
                        close(pipefd[1]);

                if (apply_script_credentials() < 0)
                        _exit(126);

                // Load environment if configured, otherwise use parents current env
                char **env = load_script_env();
                if (!env)
                        env = environ;

                // Script path for execve
                char *argv[] = { (char *)script, NULL };

                execve(script, argv, env);

                // execve failed; message goes through the pipe.
                fprintf(stderr, "execve(%s) failed: %s\n",
                        script, strerror(errno));
                _exit(127);
        }

        // Parent Process: drain the pipe, logging each line, then reap the child.
        close(pipefd[1]);
        FILE *fp = fdopen(pipefd[0], "r");
        if (fp) {
                char buf[1024];
                while (fgets(buf, sizeof(buf), fp)) {
                        buf[strcspn(buf, "\r\n")] = '\0';
                        syslog(LOG_INFO, "[script] %s", buf);
                }
                fclose(fp); // Closes all pipe[] fd's
        } else {
                close(pipefd[0]);
        }

        int status;
        if (waitpid(pid, &status, 0) < 0) {
                syslog(LOG_ERR, "waitpid() failed: %s", strerror(errno));
        } else if (WIFEXITED(status)) {
                int code = WEXITSTATUS(status);
                if (code == 0)
                        syslog(LOG_INFO,
                               "shutdown_script finished successfully");
                else
                        syslog(LOG_WARNING,
                               "shutdown_script exited with status %d", code);
        } else if (WIFSIGNALED(status)) {
                syslog(LOG_WARNING,
                       "shutdown_script killed by signal %d",
                       WTERMSIG(status));
        }
}

static int handle_prepare_for_shutdown(sd_bus_message *m, void *userdata,
                                       sd_bus_error *ret_error)
{
        int active = 0;
        int r;

        (void)userdata;
        (void)ret_error;

        r = sd_bus_message_read(m, "b", &active);
        if (r < 0) {
                syslog(LOG_ERR,
                       "Failed to parse signal: %s",
                       strerror(-r));
                return r;
        }

        if (!active)
                // Shutdown was cancelled - nothing to do.
                return 0;

        if (shutdown_script[0] == '\0') {
                syslog(LOG_WARNING,
                       "PrepareForShutdown received but no "
                       "shutdown_script configured");
        } else {
                if (shutdown_script_user[0] != '\0' ||
                    shutdown_script_group[0] != '\0') {
                        syslog(LOG_INFO,
                               "PrepareForShutdown: executing shutdown_script: %s as user='%s' group='%s'",
                               shutdown_script,
                               shutdown_script_user[0] != '\0' ? shutdown_script_user : "<unchanged>",
                               shutdown_script_group[0] != '\0' ? shutdown_script_group : "<unchanged>");
                } else {
                        syslog(LOG_INFO,
                               "PrepareForShutdown: executing shutdown_script: %s",
                               shutdown_script);
                }
                run_shutdown_script(shutdown_script);
        }

        syslog(LOG_INFO, "Releasing inhibitor lock");
        if (inhibitor_fd >= 0) {
                close(inhibitor_fd);
                inhibitor_fd = -1;
        }

        return 0;
}

static int daemonize(void)
{
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

        if (dup2(null_fd, STDIN_FILENO) < 0 ||
            dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(null_fd, STDERR_FILENO) < 0) {
                int saved = errno;
                close(null_fd);
                errno = saved;
                return -1;
        }

        if (null_fd > STDERR_FILENO)
                close(null_fd);

        return 0;
}

int main(int argc, char *argv[])
{
        const char *config_path = NULL;
        int opt;
        int option_index = 0;
        static const struct option long_options[] = {
                {"config", required_argument, 0, 'c'},
                {0, 0, 0, 0}
        };

        while ((opt = getopt_long(argc, argv, "c:", long_options,
                                  &option_index)) != -1) {
                switch (opt) {
                case 'c':
                        config_path = optarg;
                        break;
                default:
                        fprintf(stderr,
                                "Usage: %s [-c PATH] [--config PATH]\n",
                                argv[0]);
                        return EXIT_FAILURE;
                }
        }

        if (optind < argc) {
                fprintf(stderr,
                        "Usage: %s [-c PATH] [--config PATH]\n",
                        argv[0]);
                return EXIT_FAILURE;
        }

        if (daemonize() < 0) {
                fprintf(stderr, "system-update-inhibitor: daemonize() failed: %s\n",
                        strerror(errno));
                return EXIT_FAILURE;
        }

        openlog("system-update-inhibitor", LOG_PID, LOG_DAEMON);

        load_selected_config(config_path);
        ensure_logind_inhibit_delay();

        sd_bus        *bus   = NULL;
        sd_bus_slot   *slot  = NULL;
        sd_bus_message *reply = NULL;
        sd_bus_error   error  = SD_BUS_ERROR_NULL;
        int raw_fd;
        int r;

        // Connect to the system D-Bus.
        r = sd_bus_open_system(&bus);
        if (r < 0) {
                syslog(LOG_ERR, "Failed to connect to system bus: %s",
                       strerror(-r));
                goto finish;
        }

        //  Register as a "delay" shutdown inhibitor.
        r = sd_bus_call_method(
                bus,
                "org.freedesktop.login1",               // destination 
                "/org/freedesktop/login1",              // object path
                "org.freedesktop.login1.Manager",       // interface
                "Inhibit",                              // method
                &error,
                &reply,
                "ssss",                             // what, who, why mode
                "shutdown",
                "Update Script",
                "Trigger updates on shutdown/reboot",
                "delay");
        if (r < 0) {
                syslog(LOG_ERR, "Failed to acquire inhibitor lock: %s",
                       error.message ? error.message : strerror(-r));
                goto finish;
        }

        // Read the Unix fd returned by Inhibit().
        r = sd_bus_message_read(reply, "h", &raw_fd);
        if (r < 0) {
                syslog(LOG_ERR, "Failed to read inhibitor fd: %s",
                       strerror(-r));
                goto finish;
        }

        inhibitor_fd = dup(raw_fd);
        if (inhibitor_fd < 0) {
                r = -errno;
                syslog(LOG_ERR, "Failed to dup inhibitor fd: %s",
                       strerror(errno));
                goto finish;
        }

        sd_bus_message_unref(reply);
        reply = NULL;

        // Subscribe to PrepareForShutdown signal and
        // set callback to handle_prepare_for_shutdown()
        r = sd_bus_match_signal(
                bus,
                &slot,
                "org.freedesktop.login1",               // destination
                "/org/freedesktop/login1",              // object path
                "org.freedesktop.login1.Manager",       // interface
                "PrepareForShutdown",                   // member
                handle_prepare_for_shutdown,            // Function to call when signal received
                NULL);
        if (r < 0) {
                syslog(LOG_ERR,
                       "Failed to subscribe to PrepareForShutdown: %s",
                       strerror(-r));
                goto finish;
        }

        syslog(LOG_INFO,
               "Inhibitor running, waiting for PrepareForShutdown signal");

        // Event loop
        for (;;) {
                r = sd_bus_process(bus, NULL);
                if (r < 0) {
                        syslog(LOG_ERR, "Bus processing error: %s",
                               strerror(-r));
                        break;
                }
                if (r > 0)
                        continue;

                r = sd_bus_wait(bus, UINT64_MAX);
                if (r < 0 && r != -EINTR) {
                        syslog(LOG_ERR, "Bus wait error: %s", strerror(-r));
                        break;
                }
        }

finish:
        sd_bus_error_free(&error);
        if (reply)
                sd_bus_message_unref(reply);
        if (slot)
                sd_bus_slot_unref(slot);
        if (inhibitor_fd >= 0)
                close(inhibitor_fd);
        if (bus)
                sd_bus_unref(bus);

        closelog();
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
