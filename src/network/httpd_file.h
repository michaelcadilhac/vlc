/*****************************************************************************
 * httpd_file.h
 *****************************************************************************
 * Copyright (C) 2004-2006 VLC authors and VideoLAN
 * Copyright © 2004-2007 Rémi Denis-Courmont
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Rémi Denis-Courmont
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef VLC_HTTPD_FILE_H
#define VLC_HTTPD_FILE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Single HTTP byte ranges, including open-ended and suffix ranges. Offsets
 * are limited to INT64_MAX because the HTTP daemon uses signed offsets. */
static inline bool httpd_file_number(const char **text, uint64_t *value)
{
    const char *p = *text;
    uint64_t n = 0;
    if (*p < '0' || *p > '9')
        return false;
    do {
        unsigned digit = *p++ - '0';
        if (n > ((uint64_t)INT64_MAX - digit) / 10)
            return false;
        n = n * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    *text = p;
    *value = n;
    return true;
}

static inline bool httpd_file_range(const char *range, uint64_t size,
                                   uint64_t *start, uint64_t *length)
{
    *start = 0;
    *length = size;
    if (!range || !*range)
        return size <= INT64_MAX;
    if (size == 0 || size > INT64_MAX || strncmp(range, "bytes=", 6))
        return false;
    const char *p = range + 6;
    uint64_t first, last;
    if (*p == '-')
    {
        ++p;
        if (!httpd_file_number(&p, &last) || *p || last == 0)
            return false;
        *length = last < size ? last : size;
        *start = size - *length;
        return true;
    }
    if (!httpd_file_number(&p, &first) || *p++ != '-' || first >= size)
        return false;
    last = size - 1;
    if (*p && (!httpd_file_number(&p, &last) || *p || last < first))
        return false;
    if (last >= size)
        last = size - 1;
    *start = first;
    *length = last - first + 1;
    return true;
}

#endif
