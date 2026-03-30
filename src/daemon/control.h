#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>

int control_setup_socket(void);
void control_handle_socket_ready(int control_listen_fd,
                 const char *selected_config_path_runtime,
                 bool selected_config_is_custom);
void control_cleanup_socket(void);
void control_set_test_mode(bool enabled);
bool control_consume_skip_shutdown_scripts_once(void);

#endif
