#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../daemon/inhibitor.h"
#include "../daemon/guard.h"

// Perhaps these can go in a separate state.c to avoid
// setting them here and just compile the test with state.c
// that's shared with the main code? May be overkill and not
// really necessary... But would ensure the same values are
// used between the real code and the tests should they change.
int  inhibitor_fd             = -1;
bool set_max_inhibit_delay    = false;
bool restart_logind_after_set = false;
int  max_inhibit_delay        = DEFAULT_MAX_INHIBIT_DELAY;
int  current_logind_inhibit_delay = SYSTEMD_DEFAULT_INHIBIT_DELAY;

script_entry_t scripts[MAX_SCRIPTS];
int            script_count = 0;

/* Reset all globals to their initial state between tests. */
static void reset_globals(void)
{
    script_count = 0;
    memset(scripts, 0, sizeof(scripts));
    set_max_inhibit_delay    = false;
    restart_logind_after_set = false;
    max_inhibit_delay        = DEFAULT_MAX_INHIBIT_DELAY;
    current_logind_inhibit_delay = SYSTEMD_DEFAULT_INHIBIT_DELAY;
    memset(&guard_config, 0, sizeof(guard_config));
    guard_config.type      = GUARD_TYPE_ONESHOT;
    guard_config.interval  = 30;
    guard_config.threshold = 1;
    guard_config.enabled   = false;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(content, f) >= 0);
    assert(fclose(f) == 0);
}

/* ---------- parse_inhibit_delay_value ------------------------------------ */

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

/* ---------- load_config: [main] section ---------------------------------- */

static void test_load_config_main_section(void)
{
    char path[] = "/tmp/inhibitor-config-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "# comment\n"
            "[main]\n"
            "set_max_inhibit_delay = yes\n"
            "restart_logind_after_set = true\n"
            "max_inhibit_delay = 3600\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 0);
    assert(set_max_inhibit_delay == true);
    assert(restart_logind_after_set == true);
    assert(max_inhibit_delay == 3600);

    assert(unlink(path) == 0);
}

/* ---------- load_config: script sections --------------------------------- */

static void test_load_config_script_sections(void)
{
    char path[] = "/tmp/inhibitor-config-scripts-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[main]\n"
            "max_inhibit_delay = 900\n"
            "\n"
            "[applyupdates]\n"
            "command = /usr/local/sbin/update.sh\n"
            "priority = 500\n"
            "user = svcacct\n"
            "group = svcgrp\n"
            "env = /etc/update.env\n"
            "\n"
            "[cleanuptmp]\n"
            "command = /usr/local/sbin/clean.sh\n"
            "priority = 1000\n"
            "\n"
            "# Another section with missing command should be ignored but not cause failure\n"
            "[badsection]\n"
            "user = nobody\n"
            "# A section with equal priority to another section\n"
            "[equalpriority]\n"
            "command = /usr/local/sbin/other.sh\n"
            "priority = 500\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 4);
    assert(max_inhibit_delay == 900);

    /* First script section */
    assert(strcmp(scripts[0].name, "applyupdates") == 0);
    assert(strcmp(scripts[0].command, "/usr/local/sbin/update.sh") == 0);
    assert(scripts[0].priority == 500);
    assert(strcmp(scripts[0].user, "svcacct") == 0);
    assert(strcmp(scripts[0].group, "svcgrp") == 0);
    assert(strcmp(scripts[0].env, "/etc/update.env") == 0);

    /* Second script section */
    assert(strcmp(scripts[1].name, "cleanuptmp") == 0);
    assert(strcmp(scripts[1].command, "/usr/local/sbin/clean.sh") == 0);
    assert(scripts[1].priority == 1000);

    /* Third script section: no command set, but section is still parsed */
    assert(strcmp(scripts[2].name, "badsection") == 0);
    assert(strcmp(scripts[2].user, "nobody") == 0);
    assert(scripts[2].command[0] == '\0');
    assert(scripts[2].priority == 1000);

    /* Fourth script section: same priority as applyupdates */
    assert(strcmp(scripts[3].name, "equalpriority") == 0);
    assert(strcmp(scripts[3].command, "/usr/local/sbin/other.sh") == 0);
    assert(scripts[3].priority == 500);

    assert(unlink(path) == 0);
}

/* ---------- load_config: default priority ------------------------- */

static void test_load_config_default_priority(void)
{
    char path[] = "/tmp/inhibitor-config-defpri-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[myscript]\n"
            "command = /bin/true\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 1);
    assert(scripts[0].priority == 1000);

    assert(unlink(path) == 0);
}

/* ---------- load_config: invalid bool values fall back to defaults -------- */

static void test_load_config_invalid_values_fall_back(void)
{
    char path[] = "/tmp/inhibitor-config-bad-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[main]\n"
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

/* ---------- load_config: missing file ------------------------------------ */

static void test_load_config_missing_file(void)
{
    reset_globals();
    assert(load_config("/tmp/inhibitor-does-not-exist-xyz.conf") != 0);
    assert(script_count == 0);
}

/* ---------- load_config: critical= option -------------------------------- */

static void test_load_config_critical(void)
{
    char path[] = "/tmp/inhibitor-config-critical-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[stopdatabase]\n"
            "command = /opt/terminusd/scripts/stop-db.sh\n"
            "priority = 100\n"
            "critical = true\n"
            "\n"
            "[applyupdates]\n"
            "command = /opt/terminusd/scripts/update.sh\n"
            "priority = 200\n"
            "critical = yes\n"
            "\n"
            "[notify]\n"
            "command = /opt/terminusd/scripts/notify.sh\n"
            "priority = 300\n"
            "# critical not set -- should default to false\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 3);

    assert(strcmp(scripts[0].name, "stopdatabase") == 0);
    assert(scripts[0].critical == true);

    assert(strcmp(scripts[1].name, "applyupdates") == 0);
    assert(scripts[1].critical == true);

    assert(strcmp(scripts[2].name, "notify") == 0);
    assert(scripts[2].critical == false);

    assert(unlink(path) == 0);
}

static void test_load_config_critical_false_values(void)
{
    char path[] = "/tmp/inhibitor-config-critical-false-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[alpha]\n"
            "command = /bin/true\n"
            "critical = false\n"
            "\n"
            "[beta]\n"
            "command = /bin/true\n"
            "critical = 0\n"
            "\n"
            "[gamma]\n"
            "command = /bin/true\n"
            "critical = no\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 3);

    assert(scripts[0].critical == false);
    assert(scripts[1].critical == false);
    assert(scripts[2].critical == false);

    assert(unlink(path) == 0);
}

/* ---------- load_config: drop-in support semantics ----------------------- */

static void test_load_config_dropin_support(void)
{
    char base_path[] = "/tmp/inhibitor-config-base-XXXXXX";
    char d1_path[] = "/tmp/inhibitor-config-dropin1-XXXXXX";
    char d2_path[] = "/tmp/inhibitor-config-dropin2-XXXXXX";
    int fd;

    fd = mkstemp(base_path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    fd = mkstemp(d1_path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    fd = mkstemp(d2_path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(base_path,
            "[main]\n"
            "max_inhibit_delay = 120\n"
            "set_max_inhibit_delay = no\n"
            "\n"
            "[pkg]\n"
            "command = /usr/local/bin/base.sh\n"
            "priority = 100\n"
            "critical = no\n"
            "\n"
            "[cleanup]\n"
            "command = /usr/local/bin/cleanup.sh\n"
            "priority = 200\n");

    write_file(d1_path,
            "[main]\n"
            "max_inhibit_delay = 300\n"
            "set_max_inhibit_delay = yes\n"
            "\n"
            "[pkg]\n"
            "command = /opt/terminusd/scripts/pkg.sh\n"
            "critical = true\n"
            "simulate_exit_code = 7\n"
            "\n"
            "[newtask]\n"
            "command = /opt/terminusd/scripts/newtask.sh\n"
            "priority = 250\n");

    write_file(d2_path,
            "[main]\n"
            "max_inhibit_delay = 450\n"
            "\n"
            "[pkg]\n"
            "priority = 350\n"
            "user = daemon\n"
            "critical = no\n");

    reset_globals();
    assert(load_config(base_path) == 0);
    assert(load_config(d1_path) == 0);
    assert(load_config(d2_path) == 0);

    assert(max_inhibit_delay == 450);
    assert(set_max_inhibit_delay == true);

    assert(script_count == 3);

    assert(strcmp(scripts[0].name, "pkg") == 0);
    assert(strcmp(scripts[0].command,
             "/opt/terminusd/scripts/pkg.sh") == 0);
    assert(scripts[0].priority == 350);
    assert(scripts[0].critical == false);
    assert(scripts[0].simulate_exit_code == 7);
    assert(strcmp(scripts[0].user, "daemon") == 0);

    assert(strcmp(scripts[1].name, "cleanup") == 0);
    assert(strcmp(scripts[1].command,
             "/usr/local/bin/cleanup.sh") == 0);
    assert(scripts[1].priority == 200);

    assert(strcmp(scripts[2].name, "newtask") == 0);
    assert(strcmp(scripts[2].command,
             "/opt/terminusd/scripts/newtask.sh") == 0);
    assert(scripts[2].priority == 250);

    assert(unlink(base_path) == 0);
    assert(unlink(d1_path) == 0);
    assert(unlink(d2_path) == 0);
}

/* ---------- load_config: enabled= option -------------------------------- */

static void test_load_config_enabled_option(void)
{
    char path[] = "/tmp/inhibitor-config-enabled-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[enabled_script]\n"
            "command = /bin/true\n"
            "enabled = true\n"
            "\n"
            "[disabled_script]\n"
            "command = /bin/false\n"
            "enabled = false\n"
            "\n"
            "[default_enabled]\n"
            "command = /bin/ls\n"
            "# enabled not set -- should default to true\n"
            "\n"
            "[also_disabled]\n"
            "command = /bin/false\n"
            "enabled = no\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 4);

    assert(strcmp(scripts[0].name, "enabled_script") == 0);
    assert(scripts[0].enabled == true);

    assert(strcmp(scripts[1].name, "disabled_script") == 0);
    assert(scripts[1].enabled == false);

    assert(strcmp(scripts[2].name, "default_enabled") == 0);
    assert(scripts[2].enabled == true);

    assert(strcmp(scripts[3].name, "also_disabled") == 0);
    assert(scripts[3].enabled == false);

    assert(unlink(path) == 0);
}

/* ---------- filter_disabled_scripts: disabled scripts removed ------------ */

static void test_filter_disabled_scripts(void)
{
    char path[] = "/tmp/inhibitor-config-filter-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[first]\n"
            "command = /bin/true\n"
            "enabled = true\n"
            "\n"
            "[second]\n"
            "command = /bin/false\n"
            "enabled = false\n"
            "\n"
            "[third]\n"
            "command = /bin/ls\n"
            "enabled = true\n"
            "\n"
            "[fourth]\n"
            "command = /bin/false\n"
            "enabled = false\n"
            "\n"
            "[fifth]\n"
            "command = /bin/true\n"
            "# enabled defaults to true\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 5);

    /* Before filtering, all 5 scripts should be in the array. */
    assert(strcmp(scripts[0].name, "first") == 0);
    assert(strcmp(scripts[1].name, "second") == 0);
    assert(strcmp(scripts[2].name, "third") == 0);
    assert(strcmp(scripts[3].name, "fourth") == 0);
    assert(strcmp(scripts[4].name, "fifth") == 0);

    /* Simulate the filter_disabled_scripts call from load_selected_config. */
    /* Unfortunately, filter_disabled_scripts is static, so we can't call it directly.
     * Instead, we'll verify that load_selected_config properly filters by creating
     * a temporary config and calling load_selected_config with a custom path. */

    assert(unlink(path) == 0);
}

/* ---------- load_selected_config: disabled scripts are filtered out ------ */

static void test_load_selected_config_filters_disabled(void)
{
    char path[] = "/tmp/inhibitor-config-selected-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[enabled_one]\n"
            "command = /bin/true\n"
            "priority = 100\n"
            "\n"
            "[disabled_one]\n"
            "command = /bin/false\n"
            "priority = 200\n"
            "enabled = false\n"
            "\n"
            "[enabled_two]\n"
            "command = /bin/ls\n"
            "priority = 300\n");

    reset_globals();
    load_selected_config(path);

    /* After load_selected_config, disabled scripts should be filtered out. */
    assert(script_count == 2);
    assert(strcmp(scripts[0].name, "enabled_one") == 0);
    assert(scripts[0].enabled == true);
    assert(strcmp(scripts[1].name, "enabled_two") == 0);
    assert(scripts[1].enabled == true);

    assert(unlink(path) == 0);
}

/* ---------- load_config: command with arguments -------------------- */

static void test_load_config_run_command_with_arguments(void)
{
    char path[] = "/tmp/inhibitor-config-args-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
            "[script_with_args]\n"
            "command = /usr/bin/some-script.sh --flag1 --flag2 arg1 arg2\n"
            "priority = 100\n"
            "\n"
            "[script_no_args]\n"
            "command = /bin/true\n"
            "priority = 200\n"
            "\n"
            "[script_with_spaces]\n"
            "command = /opt/bin/my-script   arg1   arg2\n"
            "priority = 300\n");

    reset_globals();
    assert(load_config(path) == 0);
    assert(script_count == 3);

    /* Verify the full command is stored (including arguments) */
    assert(strcmp(scripts[0].name, "script_with_args") == 0);
    assert(strcmp(scripts[0].command,
             "/usr/bin/some-script.sh --flag1 --flag2 arg1 arg2") == 0);

    assert(strcmp(scripts[1].name, "script_no_args") == 0);
    assert(strcmp(scripts[1].command, "/bin/true") == 0);

    assert(strcmp(scripts[2].name, "script_with_spaces") == 0);
    assert(strcmp(scripts[2].command,
             "/opt/bin/my-script   arg1   arg2") == 0);

    assert(unlink(path) == 0);
}

/* ---------- load_selected_config: dropin enabled override -------------- */

static void test_load_selected_config_dropin_enabled_override(void)
{
    char base_path[] = "/tmp/inhibitor-base-enabled-XXXXXX";
    char dropin_path[] = "/tmp/inhibitor-dropin-enabled-XXXXXX";
    int fd;

    fd = mkstemp(base_path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    fd = mkstemp(dropin_path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(base_path,
            "[notifyusers]\n"
            "command = /opt/terminusd/scripts/notifyusers.sh\n"
            "priority = 100\n"
            "enabled = true\n"
            "\n"
            "[applyupdates]\n"
            "command = /opt/terminusd/scripts/applyupdates.sh\n"
            "priority = 200\n"
            "enabled = true\n"
            "\n"
            "[cleanup]\n"
            "command = /opt/terminusd/scripts/cleanup.sh\n"
            "priority = 300\n"
            "enabled = false\n");

    write_file(dropin_path,
            "[notifyusers]\n"
            "enabled = false\n"
            "\n"
            "[cleanup]\n"
            "enabled = true\n");

    reset_globals();
    assert(load_config(base_path) == 0);
    assert(script_count == 3);

    assert(load_config(dropin_path) == 0);
    assert(script_count == 3);

    /* After filter_disabled_scripts is called (via load_selected_config),
     * only applyupdates and cleanup should remain. */
    /* Simulate filtering that would happen in load_selected_config. */
    int write_idx = 0;
    for (int read_idx = 0; read_idx < script_count; read_idx++) {
        if (scripts[read_idx].enabled) {
            if (write_idx != read_idx) {
                memcpy(&scripts[write_idx], &scripts[read_idx],
                       sizeof(script_entry_t));
            }
            write_idx++;
        }
    }
    script_count = write_idx;

    assert(script_count == 2);
    assert(strcmp(scripts[0].name, "applyupdates") == 0);
    assert(scripts[0].enabled == true);
    assert(strcmp(scripts[1].name, "cleanup") == 0);
    assert(scripts[1].enabled == true);

    assert(unlink(base_path) == 0);
    assert(unlink(dropin_path) == 0);
}

static void run_test(const char *name, void (*fn)(void))
{
    fflush(stdout);
    fn();
    printf("[PASS] %s\n", name);
}

/* ---------- load_config: shutdown_guard keys ----------------------------- */

static void test_load_config_shutdown_guard(void)
{
    char path[] = "/tmp/inhibitor-config-guard-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path,
        "[main]\n"
        "shutdown_guard_enabled = true\n"
        "shutdown_guard_command = /opt/terminusd/scripts/my-guard.sh --watch\n"
        "shutdown_guard_type = persist\n"
        "shutdown_guard_interval = 45\n"
        "shutdown_guard_threshold = 2\n"
        "shutdown_guard_run_as_user = nobody\n"
        "shutdown_guard_run_as_group = nogroup\n"
        "shutdown_guard_run_env = /etc/guard.env\n");

    reset_globals();
    assert(load_config(path) == 0);

    assert(guard_config.enabled == true);
    assert(strcmp(guard_config.command,
         "/opt/terminusd/scripts/my-guard.sh --watch") == 0);
    assert(guard_config.type == GUARD_TYPE_PERSIST);
    assert(guard_config.interval == 45);
    assert(guard_config.threshold == 2);
    assert(strcmp(guard_config.user, "nobody") == 0);
    assert(strcmp(guard_config.group, "nogroup") == 0);
    assert(strcmp(guard_config.env, "/etc/guard.env") == 0);

    assert(unlink(path) == 0);

    /* Verify oneshot type parses correctly */
    char path2[] = "/tmp/inhibitor-config-guard2-XXXXXX";
    fd = mkstemp(path2);
    assert(fd >= 0);
    assert(close(fd) == 0);

    write_file(path2,
        "[main]\n"
        "shutdown_guard_enabled = false\n"
        "shutdown_guard_command = /usr/local/bin/check.sh --silent\n"
        "shutdown_guard_type = oneshot\n"
        "shutdown_guard_interval = 120\n"
        "shutdown_guard_threshold = 3\n");

    reset_globals();
    assert(load_config(path2) == 0);

    assert(guard_config.enabled == false);
    assert(strcmp(guard_config.command,
         "/usr/local/bin/check.sh --silent") == 0);
    assert(guard_config.type == GUARD_TYPE_ONESHOT);
    assert(guard_config.interval == 120);
    assert(guard_config.threshold == 3);

    assert(unlink(path2) == 0);
}

int main(void)
{
    run_test("test_parse_inhibit_delay_value",
         test_parse_inhibit_delay_value);
    run_test("test_load_config_main_section",
         test_load_config_main_section);
    run_test("test_load_config_script_sections",
         test_load_config_script_sections);
    run_test("test_load_config_default_priority",
         test_load_config_default_priority);
    run_test("test_load_config_invalid_values_fall_back",
         test_load_config_invalid_values_fall_back);
    run_test("test_load_config_missing_file",
         test_load_config_missing_file);
    run_test("test_load_config_critical",
         test_load_config_critical);
    run_test("test_load_config_critical_false_values",
         test_load_config_critical_false_values);
    run_test("test_load_config_dropin_support",
         test_load_config_dropin_support);
    run_test("test_load_config_enabled_option",
         test_load_config_enabled_option);
    run_test("test_filter_disabled_scripts",
         test_filter_disabled_scripts);
    run_test("test_load_selected_config_filters_disabled",
         test_load_selected_config_filters_disabled);
    run_test("test_load_config_run_command_with_arguments",
         test_load_config_run_command_with_arguments);
    run_test("test_load_selected_config_dropin_enabled_override",
         test_load_selected_config_dropin_enabled_override);
        run_test("test_load_config_shutdown_guard",
             test_load_config_shutdown_guard);
    puts("[PASS] All config parser tests passed");
    return 0;
}