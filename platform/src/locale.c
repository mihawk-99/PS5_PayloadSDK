/*
 * PS5 Platform - C-locale number parsing (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * No system module exports newlocale, freelocale, strtod_l or strtof_l; Mesa
 * parses shader and configuration numbers with them so that the decimal point
 * is always '.', whatever locale the title set. The only locale these give is
 * "C" (and "POSIX", and "" for the environment's, which a title's is): its
 * numbers are parsed by the exported strtod and strtof, with '.' put in place
 * of the global locale's decimal point when that differs.
 */
#define _GNU_SOURCE 1

#include "ps5platform/libc.h"

#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

/* The one locale object: its address is all anyone sees of it. */
static const char c_locale_token = 'C';

void *
ps5_newlocale(int category_mask, const char *name, void *base)
{
   (void)category_mask;
   (void)base;
   if (!name) {
      errno = EINVAL;
      return NULL;
   }
   if (strcmp(name, "C") != 0 && strcmp(name, "POSIX") != 0 && name[0] != '\0') {
      errno = ENOENT;
      return NULL;
   }
   return (void *)&c_locale_token;
}

void
ps5_freelocale(void *locale)
{
   (void)locale;
}

/* The longest number the translation below copies: longer ones are parsed as
 * they stand. */
#define PS5_NUMBER_CHARS 512

/* Parses s with parse (strtod or strtof, through a double), with '.' as the
 * decimal point even when the global locale's differs. */
static double
parse_c_number(const char *s, char **end, double (*parse)(const char *, char **))
{
   const struct lconv *const conventions = localeconv();
   const char *const point = conventions ? conventions->decimal_point : ".";
   const char *const dot = strchr(s, '.');
   if (!point || strcmp(point, ".") == 0 || !dot || dot - s >= PS5_NUMBER_CHARS)
      return parse(s, end);
   /* Copy the number with the locale's point in place of the first '.', then
    * map the end back: characters after the point shift by the difference. */
   char copy[PS5_NUMBER_CHARS + 16];
   const size_t before = (size_t)(dot - s);
   const size_t point_length = strlen(point);
   if (point_length > 8)
      return parse(s, end);
   const size_t tail = strnlen(dot + 1, PS5_NUMBER_CHARS - before);
   memcpy(copy, s, before);
   memcpy(copy + before, point, point_length);
   memcpy(copy + before + point_length, dot + 1, tail);
   copy[before + point_length + tail] = '\0';
   char *copy_end = NULL;
   const double value = parse(copy, &copy_end);
   if (end) {
      size_t used = (size_t)(copy_end - copy);
      if (used > before)
         used = used >= before + point_length ? used - point_length + 1 : before;
      *end = (char *)s + used;
   }
   return value;
}

static double
parse_float(const char *s, char **end)
{
   return strtof(s, end);
}

double
ps5_strtod_l(const char *s, char **end, void *locale)
{
   (void)locale;
   return parse_c_number(s, end, strtod);
}

float
ps5_strtof_l(const char *s, char **end, void *locale)
{
   (void)locale;
   return (float)parse_c_number(s, end, parse_float);
}
