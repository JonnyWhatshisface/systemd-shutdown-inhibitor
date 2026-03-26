#ifndef INHIBITOR_H
#define INHIBITOR_H

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#define CONF_SYSTEM "/etc/system-update-inhibitor.conf"
#define LOGIND_CONF "/etc/systemd/logind.conf"

#define SCRIPT_IDENTITY_LEN 256
#define DEFAULT_MAX_INHIBIT_DELAY 1800

static int  inhibitor_fd     = -1; // Global inhibitor fd. Releasing this releases the inhibitor lock
static char shutdown_script[PATH_MAX] = "";
static char shutdown_script_user[SCRIPT_IDENTITY_LEN] = "";
static char shutdown_script_group[SCRIPT_IDENTITY_LEN] = "";
static char shutdown_script_env[PATH_MAX] = "";
static bool set_max_inhibit_delay = false;
static bool restart_logind_after_set = false;
static int  max_inhibit_delay = DEFAULT_MAX_INHIBIT_DELAY;

#endif // INHIBITOR_H
