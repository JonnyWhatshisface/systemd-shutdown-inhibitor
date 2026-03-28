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
#include "test.h"

void test_mode_log(int priority, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    syslog(priority, "%s", msg);
    fprintf(stderr, "%s\n", msg);
}

// External declarations for variables in inhibitor.c
extern script_entry_t scripts[MAX_SCRIPTS];
extern int script_count;

static int cmp_script_index(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const script_entry_t *sa = &scripts[ia];
    const script_entry_t *sb = &scripts[ib];

    if (sa->priority < sb->priority)
        return -1;
    if (sa->priority > sb->priority)
        return 1;
    return strcmp(sa->name, sb->name);
}

void log_test_mode_plan(void)
{
    if (script_count == 0) {
        test_mode_log(LOG_INFO,
                 "[test-mode] No configured scripts; nothing would run");
        return;
    }

    int order[MAX_SCRIPTS];
    for (int idx = 0; idx < script_count; idx++)
        order[idx] = idx;
    qsort(order, (size_t)script_count, sizeof(order[0]),
         cmp_script_index);

    test_mode_log(LOG_INFO,
             "[test-mode] Would execute %d script section(s) in this order:",
             script_count);
    for (int i = 0; i < script_count; ) {
        unsigned int cur_prio = scripts[order[i]].priority;
        int j = i;
        while (j < script_count && scripts[order[j]].priority == cur_prio)
            j++;

        int group_size = j - i;
        test_mode_log(LOG_INFO,
                 "[test-mode] parallel-group priority=%u count=%d",
                 cur_prio, group_size);

        bool critical_fail = false;
        for (int k = i; k < j; k++) {
            const script_entry_t *e = &scripts[order[k]];
            test_mode_log(LOG_INFO,
                     "[test-mode] #%d section=%s priority=%u command=\"%s\" user=%s group=%s env=%s",
                     k + 1,
                     e->name,
                     e->priority,
                     e->command[0] ? e->command : "<unset>",
                     e->user[0] ? e->user : "<daemon>",
                     e->group[0] ? e->group : "<daemon>",
                     e->env[0] ? e->env : "<inherited>");
            if (e->critical && e->simulate_exit_code != 0) {
                test_mode_log(LOG_ERR,
                         "[test-mode] critical script '%s' simulate_exit_code=%d: FAILED",
                         e->name, e->simulate_exit_code);
                critical_fail = true;
            }
        }

        if (critical_fail) {
            // Count remaining priority groups after this one
            int remaining = 0;
            for (int ni = j; ni < script_count; ) {
                unsigned int np = scripts[order[ni]].priority;
                remaining++;
                while (ni < script_count &&
                    scripts[order[ni]].priority == np)
                    ni++;
            }
            test_mode_log(LOG_ERR,
                     "[test-mode] critical failure in priority group %u -- "
                     "aborting %d remaining group(s); inhibitor would release",
                     cur_prio, remaining);
            return;
        }

        i = j;
    }
}
