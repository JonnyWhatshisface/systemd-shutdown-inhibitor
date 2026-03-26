#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define main inhibitor_daemon_main
#include "../daemon/inhibitor.c"
#undef main

static void reset_globals(void)
{
        shutdown_script[0] = '\0';
        shutdown_script_user[0] = '\0';
        shutdown_script_group[0] = '\0';
        shutdown_script_env[0] = '\0';
        set_max_inhibit_delay = false;
        restart_logind_after_set = false;
        max_inhibit_delay = DEFAULT_MAX_INHIBIT_DELAY;
}

static void write_file(const char *path, const char *content)
{
        FILE *f = fopen(path, "w");
        assert(f != NULL);
        assert(fputs(content, f) >= 0);
        assert(fclose(f) == 0);
}

static void test_parse_inhibit_delay_value(void)
{
        int value = -1;

        assert(parse_inhibit_delay_value("InhibitDelayMaxSec=120\n", &value) == 1);
        assert(value == 120);

        value = -1;
        assert(parse_inhibit_delay_value("  InhibitDelayMaxSec = 45\r\n", &value) == 1);
        assert(value == 45);

        value = -1;
        assert(parse_inhibit_delay_value("#InhibitDelayMaxSec=22\n", &value) == 0);
        assert(value == -1);

        value = -1;
        assert(parse_inhibit_delay_value("InhibitDelayMaxSec=abc\n", &value) == -1);
        assert(value == -1);

        value = -1;
        assert(parse_inhibit_delay_value("InhibitDelayMaxSec=25 sec\n", &value) == -1);
        assert(value == -1);
}

static void test_load_config_parses_values(void)
{
        char path[] = "/tmp/inhibitor-config-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        assert(close(fd) == 0);

        write_file(path,
                   "# comment\n"
                   " shutdown_script = /usr/local/bin/update.sh \n"
                   "shutdown_script_user = root\n"
                   "shutdown_script_group = wheel\n"
                   "shutdown_script_env = /etc/inhibitor.env\n"
                   "set_max_inhibit_delay = yes\n"
                   "restart_logind_after_set = true\n"
                   "max_inhibit_delay = 3600\n");

        reset_globals();
        assert(load_config(path) == 0);
        assert(strcmp(shutdown_script, "/usr/local/bin/update.sh") == 0);
        assert(strcmp(shutdown_script_user, "root") == 0);
        assert(strcmp(shutdown_script_group, "wheel") == 0);
        assert(strcmp(shutdown_script_env, "/etc/inhibitor.env") == 0);
        assert(set_max_inhibit_delay == true);
        assert(restart_logind_after_set == true);
        assert(max_inhibit_delay == 3600);

        assert(unlink(path) == 0);
}

static void test_load_config_invalid_values_fall_back(void)
{
        char path[] = "/tmp/inhibitor-config-bad-XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        assert(close(fd) == 0);

        write_file(path,
                   "set_max_inhibit_delay = notabool\n"
                   "restart_logind_after_set = maybe\n"
                   "max_inhibit_delay = 0\n");

        reset_globals();
        assert(load_config(path) == 0);
        assert(set_max_inhibit_delay == false);
        assert(restart_logind_after_set == false);
        assert(max_inhibit_delay == DEFAULT_MAX_INHIBIT_DELAY);

        assert(unlink(path) == 0);
}

int main(void)
{
        test_parse_inhibit_delay_value();
        test_load_config_parses_values();
        test_load_config_invalid_values_fall_back();
        puts("All unit tests passed");
        return 0;
}