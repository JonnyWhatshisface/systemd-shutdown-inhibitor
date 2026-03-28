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

int parse_inhibit_delay_value(const char *line, int *value)
{
    const char *p = line + strspn(line, " \t");
    size_t key_len = strlen("InhibitDelayMaxSec");
    char *endptr = NULL;
    long parsed;

    if (!is_active_inhibit_delay_line(line))
        return 0;

    p += key_len;
    p += strspn(p, " \t");
    p++;
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
    int wret;
    do {
        wret = waitpid(pid, &status, 0);
    } while (wret < 0 && errno == EINTR);

    if (wret < 0) {
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

void ensure_logind_inhibit_delay(void)
{
    FILE *f = fopen(LOGIND_CONF, "r");
    int current = SYSTEMD_DEFAULT_INHIBIT_DELAY;
    bool found = false;
    bool malformed = false;
    char line[1024];

    if (!set_max_inhibit_delay) {
        syslog(LOG_INFO,
              "set_max_inhibit_delay is false or unset; leaving %s unchanged",
              LOGIND_CONF);
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
            malformed = false;
            current = parsed;
        } else if (state == -1 && is_active_inhibit_delay_line(line)) {
            malformed = true;
            syslog(LOG_WARNING,
                  "Malformed InhibitDelayMaxSec line in %s",
                  LOGIND_CONF);
        }
    }
    fclose(f);

    current_logind_inhibit_delay = found ? current : SYSTEMD_DEFAULT_INHIBIT_DELAY;

    if (!set_max_inhibit_delay)
        return;

    if (!malformed && found && current == max_inhibit_delay) {
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

    current_logind_inhibit_delay = max_inhibit_delay;

    if (restart_logind_after_set) {
        restart_logind_service();
    } else {
        syslog(LOG_INFO,
              "Updated %s without restarting systemd-logind.service because restart_logind_after_set is false or unset",
              LOGIND_CONF);
    }
}
