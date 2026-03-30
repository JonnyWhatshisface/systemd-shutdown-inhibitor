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

#include <dirent.h>

static int cmp_str_ptr(const void *a, const void *b) {
    const char *sa = *(const char * const *)a;
    const char *sb = *(const char * const *)b;
    return strcmp(sa, sb);
}

static int load_dropin_dir(const char *dir_path) {
    char *entries[256];
    int entry_count = 0, loaded_count = 0;
    struct dirent *de;
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno == ENOENT)
            return 0;

        syslog(LOG_WARNING, "Failed to open config drop-in directory %s: %s", dir_path, strerror(errno));
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
          strcmp(de->d_name, "..") == 0)
            continue;

        if (entry_count >= (int)(sizeof(entries) / sizeof(entries[0]))) {
            syslog(LOG_WARNING, "Too many drop-in files in %s (max %zu); ignoring extras", dir_path, sizeof(entries) / sizeof(entries[0]));
            break;
        }

        entries[entry_count] = strdup(de->d_name);
        if (!entries[entry_count]) {
            syslog(LOG_ERR, "Out of memory while reading drop-in directory %s", dir_path);
            break;
        }
        entry_count++;
    }

    closedir(dir);

    qsort(entries, (size_t)entry_count, sizeof(entries[0]), cmp_str_ptr);

    for (int i = 0; i < entry_count; i++) {
        char path[PATH_MAX];
        struct stat st;

        int n = snprintf(path, sizeof(path), "%s/%s", dir_path, entries[i]);

        if (n < 0 || (size_t)n >= sizeof(path)) {
            syslog(LOG_WARNING, "Drop-in path too long, skipping %s/%s", dir_path, entries[i]);
            free(entries[i]);
            continue;
        }

        if (stat(path, &st) != 0) {
            syslog(LOG_WARNING, "Cannot stat drop-in %s: %s", path, strerror(errno));
            free(entries[i]);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            free(entries[i]);
            continue;
        }

        if (load_config(path) == 0) {
            loaded_count++;
        } else {
            syslog(LOG_WARNING, "Failed to load config drop-in %s", path);
        }

        free(entries[i]);
    }

    return loaded_count;
}

int load_config(const char *path) {
    char line[PATH_MAX + 128];
    char section[SCRIPT_NAME_LEN] = "";
    script_entry_t *cur = NULL;

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';

        char *p = trim_leading(line);
        if (*p == '#' || *p == '\0')
            continue;

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (!close) {
                syslog(LOG_WARNING, "Malformed section header in %s: %s", path, p);
                continue;
            }
            *close = '\0';
            char *name = trim_leading(p + 1);
            trim_trailing(name);

            strncpy(section, name, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';

            if (strcmp(section, "main") == 0) {
                cur = NULL;
            } else {
                cur = NULL;
                for (int i = 0; i < script_count; i++) {
                    if (strcmp(scripts[i].name, section) == 0) {
                        cur = &scripts[i];
                        break;
                    }
                }
                if (!cur) {
                    if (script_count >= MAX_SCRIPTS) {
                        syslog(LOG_WARNING, "Too many script sections (max %d); ignoring [%s]", MAX_SCRIPTS, section);
                        cur = NULL;
                    } else {
                        cur = &scripts[script_count++];
                        memset(cur, 0, sizeof(*cur));
                        snprintf(cur->name, sizeof(cur->name), "%s", section);
                        cur->priority = 1000;
                        cur->enabled = true;
                    }
                }
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key   = trim_trailing(p);
        char *value = trim_leading(eq + 1);
        trim_trailing(value);

        if (strcmp(section, "main") == 0 || section[0] == '\0') {
            if (strcmp(key, "set_max_inhibit_delay") == 0) {
                set_max_inhibit_delay = parse_bool(value, false, key, path);
            } else if (strcmp(key, "restart_logind_after_set") == 0) {
                restart_logind_after_set = parse_bool(value, false, key, path);
            } else if (strcmp(key, "max_inhibit_delay") == 0) {
                char *endptr = NULL;
                errno = 0;
                long parsed = strtol(value, &endptr, 10);
                if (errno == 0 && endptr != value && *endptr == '\0' && parsed > 0 && parsed <= INT_MAX) {
                    max_inhibit_delay = (int)parsed;
                } else {
                    syslog(LOG_WARNING, "Invalid max_inhibit_delay '%s' in %s; using %d", value, path, max_inhibit_delay);
                }
            } else if (strcmp(key, "shutdown_guard_command") == 0) {
                strncpy(guard_config.command, value, sizeof(guard_config.command) - 1);
            } else if (strcmp(key, "shutdown_guard_type") == 0) {
                if (strcmp(value, "persist") == 0)
                    guard_config.type = GUARD_TYPE_PERSIST;
                else if (strcmp(value, "oneshot") == 0)
                    guard_config.type = GUARD_TYPE_ONESHOT;
                else
                    syslog(LOG_WARNING, "Unknown shutdown_guard_type '%s' in %s; " "using oneshot", value, path);
            } else if (strcmp(key, "shutdown_guard_interval") == 0) {
                char *endptr = NULL;
                errno = 0;
                unsigned long parsed = strtoul(value, &endptr, 10);
                if (errno == 0 && endptr != value && *endptr == '\0' && parsed > 0)
                    guard_config.interval = (unsigned int)parsed;
                else
                    syslog(LOG_WARNING, "Invalid shutdown_guard_interval '%s' in %s; using %u", value, path, guard_config.interval);
            } else if (strcmp(key, "shutdown_guard_threshold") == 0) {
                char *endptr = NULL;
                errno = 0;
                unsigned long parsed =
                strtoul(value, &endptr, 10);
                if (errno == 0 && endptr != value && *endptr == '\0' && parsed > 0)
                    guard_config.threshold = (unsigned int)parsed;
                else
                    syslog(LOG_WARNING, "Invalid shutdown_guard_threshold '%s' in %s; using %u", value, path, guard_config.threshold);
            } else if (strcmp(key, "shutdown_guard_run_as_user") == 0) {
                strncpy(guard_config.user, value, sizeof(guard_config.user) - 1);
            } else if (strcmp(key, "shutdown_guard_run_as_group") == 0) {
                strncpy(guard_config.group, value, sizeof(guard_config.group) - 1);
            } else if (strcmp(key, "shutdown_guard_run_env") == 0) {
                strncpy(guard_config.env, value, sizeof(guard_config.env) - 1);
            } else if (strcmp(key, "shutdown_guard_enabled") == 0) {
                guard_config.enabled = parse_bool(value, false, key, path);
            } else {
                syslog(LOG_WARNING, "Unknown key '%s' in [main] of %s", key, path);
            }
        } else if (cur) {
            if (strcmp(key, "command") == 0) {
                strncpy(cur->command, value, sizeof(cur->command) - 1);
            } else if (strcmp(key, "user") == 0) {
                strncpy(cur->user, value, sizeof(cur->user) - 1);
            } else if (strcmp(key, "group") == 0) {
                strncpy(cur->group, value, sizeof(cur->group) - 1);
            } else if (strcmp(key, "env") == 0) {
                strncpy(cur->env, value, sizeof(cur->env) - 1);
            } else if (strcmp(key, "priority") == 0) {
                char *endptr = NULL;
                errno = 0;
                unsigned long parsed = strtoul(value, &endptr, 10);
                if (errno == 0 && endptr != value &&
                  *endptr == '\0') {
                    cur->priority = (unsigned int)parsed;
                } else {
                    syslog(LOG_WARNING, "Invalid priority '%s' in [%s] of %s; using %u", value, cur->name, path, cur->priority);
                }
            } else if (strcmp(key, "critical") == 0) {
                cur->critical = parse_bool(value, false, key, path);
            } else if (strcmp(key, "enabled") == 0) {
                cur->enabled = parse_bool(value, true, key, path);
            } else if (strcmp(key, "simulate_exit_code") == 0) {
                char *endptr = NULL;
                errno = 0;
                long parsed = strtol(value, &endptr, 10);
                if (errno == 0 && endptr != value && *endptr == '\0' && parsed >= 0 && parsed <= 255) {
                    cur->simulate_exit_code = (int)parsed;
                } else {
                    syslog(LOG_WARNING, "Invalid simulate_exit_code '%s' in [%s] of %s; using 0", value, cur->name, path);
                }
            } else {
                syslog(LOG_WARNING, "Unknown key '%s' in [%s] of %s", key, cur->name, path);
            }
        }
    }

    fclose(f);
    return 0;
}

static void filter_disabled_scripts(void) {
    int write_idx = 0;
    for (int read_idx = 0; read_idx < script_count; read_idx++) {
        if (scripts[read_idx].enabled) {
            if (write_idx != read_idx)
                memcpy(&scripts[write_idx], &scripts[read_idx], sizeof(script_entry_t));
            write_idx++;
        }
    }
    script_count = write_idx;
}

void reset_config_state(void) {
    script_count = 0;
    memset(scripts, 0, sizeof(scripts));

    set_max_inhibit_delay = false;
    restart_logind_after_set = false;
    max_inhibit_delay = DEFAULT_MAX_INHIBIT_DELAY;

    memset(&guard_config, 0, sizeof(guard_config));
    guard_config.type = GUARD_TYPE_ONESHOT;
    guard_config.interval = 30;
    guard_config.threshold = 1;
    guard_config.enabled = false;
}

void load_selected_config(const char *config_path) {
    const char *path = config_path ? config_path : CONF_SYSTEM;
    bool loaded_any = false;

    reset_config_state();

    if (load_config(path) == 0) {
        syslog(LOG_INFO, "Loaded config from %s (%d shutdown action(s))", path, script_count);
        loaded_any = true;
    } else {
        syslog(LOG_WARNING, "No config file found at %s", path);
    }

    if (!config_path) {
        int dropins = load_dropin_dir(CONF_DROPIN_DIR);
        if (dropins > 0) {
            syslog(LOG_INFO, "Loaded %d config drop-in file(s) from %s", dropins, CONF_DROPIN_DIR);
            loaded_any = true;
        }
    }

    // Filter out disabled scripts from the final configuration.
    filter_disabled_scripts();

    if (!loaded_any) {
        syslog(LOG_WARNING, "No shutdown actions configured.");
    } else {
        syslog(LOG_INFO, "Final configuration contains %d shutdown action(s)", script_count);
    }
}
