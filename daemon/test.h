#ifndef TEST_H
#define TEST_H

#include <syslog.h>

void test_mode_log(int priority, const char *fmt, ...);
void log_test_mode_plan(void);

#endif /* TEST_H */
