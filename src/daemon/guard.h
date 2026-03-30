#ifndef GUARD_H
#define GUARD_H

#include <stdbool.h>

void guard_start(void);
int  guard_get_fd(void);
void guard_on_readable(void);
void guard_tick(void);
int  guard_poll_timeout_ms(void);

// Control hooks used by terminusctl
bool guard_runtime_set_enabled(bool enabled);
bool guard_runtime_set_shutdown_disabled(bool disabled);
bool guard_runtime_unmask_forced(void);
bool guard_shutdowns_disabled(void);

#endif /* GUARD_H */
