#ifndef INHIBITOR_H
#define INHIBITOR_H

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <sys/wait.h>
#include <poll.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define CONF_SYSTEM "/etc/terminusd.conf"
#define CONF_DROPIN_DIR "/etc/terminus.d"
#define LOGIND_CONF "/etc/systemd/logind.conf"
#define CONTROL_SOCKET_PATH "/run/terminusd.sock"
#define SYSTEMD_DEFAULT_INHIBIT_DELAY   5

#define SCRIPT_IDENTITY_LEN             256
#define SCRIPT_NAME_LEN                 64
#define DEFAULT_MAX_INHIBIT_DELAY       1800
#define MAX_SCRIPTS                     64

// Per-script configuration
typedef struct {
    char         name[SCRIPT_NAME_LEN];
    char         command[PATH_MAX];
    char         user[SCRIPT_IDENTITY_LEN];
    char         group[SCRIPT_IDENTITY_LEN];
    char         env[PATH_MAX];
    unsigned int priority;
    bool         critical;
    bool         enabled;         // if false, script will be skipped entirely
    int          simulate_exit_code; // test-mode only: non-zero simulates a script failure
} script_entry_t;

extern int  inhibitor_fd;
extern bool set_max_inhibit_delay;
extern bool restart_logind_after_set;
extern int  max_inhibit_delay;
extern int  current_logind_inhibit_delay;

typedef struct {
    pid_t  pid;
    int    fd;
    char   name[SCRIPT_NAME_LEN];
    char   linebuf[1024];
    int    linelen;
    bool   critical;
    int    exit_code;  // 0=success, >0=error exit, -1=signal/unknown
} running_t;

typedef enum {
    GUARD_TYPE_ONESHOT = 0,
    GUARD_TYPE_PERSIST
} guard_type_t;

struct shutdown_guard_config {
    char         command[PATH_MAX];           // command + optional args
    char         user[SCRIPT_IDENTITY_LEN];
    char         group[SCRIPT_IDENTITY_LEN];
    char         env[PATH_MAX];
    guard_type_t type;                        // GUARD_TYPE_ONESHOT or PERSIST
    unsigned int interval;                    // oneshot: seconds between runs
    unsigned int threshold;                   // oneshot: consecutive failures before mask
    bool         enabled;                     // default: false
};

extern struct shutdown_guard_config guard_config;

extern script_entry_t scripts[MAX_SCRIPTS];
extern int            script_count;

char *trim_trailing(char *s);
char *trim_leading(char *s);
bool parse_bool(const char *value, bool default_val,
        const char *key, const char *path);

int load_config(const char *path);
void load_selected_config(const char *config_path);
void reset_config_state(void);

// Exec helpers (scripts.c) — shared with guard.c
char **parse_command_argv(const char *command, int *argv_count);
void   free_argv(char **argv);
char **load_entry_env(const script_entry_t *e);
void   free_env_array(char **env);
int    apply_entry_credentials(const script_entry_t *e);

int parse_inhibit_delay_value(const char *line, int *value);
void ensure_logind_inhibit_delay(void);

void run_all_scripts(void);

int cmp_priority(const void *a, const void *b);

#endif /* INHIBITOR_H */
