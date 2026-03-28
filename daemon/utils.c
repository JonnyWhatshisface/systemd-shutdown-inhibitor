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

char *trim_trailing(char *s)
{
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

char *trim_leading(char *s)
{
    return s + strspn(s, " \t");
}

bool parse_bool(const char *value, bool default_val,
        const char *key, const char *path)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
      strcmp(value, "1") == 0)
        return true;
    if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
      strcmp(value, "0") == 0)
        return false;
    syslog(LOG_WARNING, "Invalid boolean value '%s' for '%s' in %s; using %s",
          value, key, path, default_val ? "true" : "false");
    return default_val;
}

