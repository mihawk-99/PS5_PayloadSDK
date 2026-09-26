/*
 * PS5 Platform - POSIX regular expressions (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * No system module exports regcomp, regexec or regfree, so code built against
 * the SDK's FreeBSD <regex.h> -- Mesa's driconf matches applications with it --
 * binds them to these. The engine is musl 1.2.5's (src/regex/, MIT), under
 * private names; this file converts FreeBSD's regex_t, regmatch_t and flag
 * values to musl's and back. The compiled pattern lives in re_g, the pointer
 * FreeBSD keeps its own compiled form in.
 */
#include "ps5platform/libc.h"

#include "regex/musl_regex.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* FreeBSD's re_magic for a compiled pattern (its regex2.h, MAGIC1). */
#define PS5_REGEX_MAGIC ((int)((('r' ^ 0200) << 8) | 'e'))

int
ps5_regcomp(struct ps5_regex *preg, const char *pattern, int cflags)
{
   if (!preg || !pattern)
      return PS5_REG_INVARG;
   /* REG_NOSPEC and REG_PEND are FreeBSD's own: a literal pattern, and one
    * that ends at re_endp rather than at its nul. */
   char *copy = NULL;
   if (cflags & PS5_REG_PEND) {
      if (!preg->re_endp || preg->re_endp < pattern)
         return PS5_REG_INVARG;
      const size_t length = (size_t)(preg->re_endp - pattern);
      copy = malloc(length + 1);
      if (!copy)
         return PS5_REG_ESPACE;
      memcpy(copy, pattern, length);
      copy[length] = '\0';
      pattern = copy;
   }
   if (cflags & PS5_REG_NOSPEC) {
      /* Every character stands for itself: escape the basic syntax's
       * special characters. */
      const size_t length = strlen(pattern);
      char *literal = malloc(length * 2 + 1);
      if (!literal) {
         free(copy);
         return PS5_REG_ESPACE;
      }
      char *at = literal;
      for (const char *c = pattern; *c; c++) {
         if (strchr(".[\\*^$", *c))
            *at++ = '\\';
         *at++ = *c;
      }
      *at = '\0';
      free(copy);
      copy = literal;
      pattern = copy;
      cflags &= ~PS5_REG_EXTENDED;
   }
   int flags = 0;
   if (cflags & PS5_REG_EXTENDED)
      flags |= REG_EXTENDED;
   if (cflags & PS5_REG_ICASE)
      flags |= REG_ICASE;
   if (cflags & PS5_REG_NOSUB)
      flags |= REG_NOSUB;
   if (cflags & PS5_REG_NEWLINE)
      flags |= REG_NEWLINE;

   regex_t *const compiled = calloc(1, sizeof(*compiled));
   if (!compiled) {
      free(copy);
      return PS5_REG_ESPACE;
   }
   /* musl's error codes 1-13 are FreeBSD's. */
   const int result = regcomp(compiled, pattern, flags);
   free(copy);
   if (result != REG_OK) {
      free(compiled);
      return result;
   }
   preg->re_magic = PS5_REGEX_MAGIC;
   preg->re_nsub = compiled->re_nsub;
   preg->re_g = compiled;
   return 0;
}

int
ps5_regexec(const struct ps5_regex *preg, const char *string, size_t nmatch, struct ps5_regmatch *pmatch,
            int eflags)
{
   if (!preg || preg->re_magic != PS5_REGEX_MAGIC || !preg->re_g || !string)
      return PS5_REG_BADPAT;
   const regex_t *const compiled = preg->re_g;
   /* The engine ignores the matches of a pattern compiled with REG_NOSUB. */
   const size_t count = pmatch ? nmatch : 0;

   /* REG_STARTEND: the subject is string[pmatch[0].rm_so, pmatch[0].rm_eo),
    * and offsets stay relative to string. */
   const char *subject = string;
   char *copy = NULL;
   regoff_t base = 0;
   if (eflags & PS5_REG_STARTEND) {
      if (!pmatch || pmatch[0].rm_so < 0 || pmatch[0].rm_eo < pmatch[0].rm_so)
         return PS5_REG_INVARG;
      base = (regoff_t)pmatch[0].rm_so;
      const size_t length = (size_t)(pmatch[0].rm_eo - pmatch[0].rm_so);
      copy = malloc(length + 1);
      if (!copy)
         return PS5_REG_ESPACE;
      memcpy(copy, string + base, length);
      copy[length] = '\0';
      subject = copy;
   }
   int flags = 0;
   if (eflags & PS5_REG_NOTBOL)
      flags |= REG_NOTBOL;
   if (eflags & PS5_REG_NOTEOL)
      flags |= REG_NOTEOL;

   regmatch_t *matches = NULL;
   if (count) {
      matches = calloc(count, sizeof(*matches));
      if (!matches) {
         free(copy);
         return PS5_REG_ESPACE;
      }
   }
   const int result = regexec(compiled, subject, count, matches, flags);
   if (result == REG_OK) {
      for (size_t i = 0; i < count; i++) {
         pmatch[i].rm_so = matches[i].rm_so < 0 ? -1 : matches[i].rm_so + base;
         pmatch[i].rm_eo = matches[i].rm_eo < 0 ? -1 : matches[i].rm_eo + base;
      }
   }
   free(matches);
   free(copy);
   return result;
}

void
ps5_regfree(struct ps5_regex *preg)
{
   if (!preg || preg->re_magic != PS5_REGEX_MAGIC || !preg->re_g)
      return;
   regfree(preg->re_g);
   free(preg->re_g);
   preg->re_g = NULL;
   preg->re_magic = 0;
}

size_t
ps5_regerror(int code, const struct ps5_regex *preg, char *buffer, size_t size)
{
   (void)preg;
   static const char *const messages[] = {
      [0] = "No error",
      [PS5_REG_NOMATCH] = "No match",
      [PS5_REG_BADPAT] = "Invalid regexp",
      [PS5_REG_ECOLLATE] = "Unknown collating element",
      [PS5_REG_ECTYPE] = "Unknown character class name",
      [PS5_REG_EESCAPE] = "Trailing backslash",
      [PS5_REG_ESUBREG] = "Invalid back reference",
      [PS5_REG_EBRACK] = "Missing ']'",
      [PS5_REG_EPAREN] = "Missing ')'",
      [PS5_REG_EBRACE] = "Missing '}'",
      [PS5_REG_BADBR] = "Invalid contents of {}",
      [PS5_REG_ERANGE] = "Invalid character range",
      [PS5_REG_ESPACE] = "Out of memory",
      [PS5_REG_BADRPT] = "Repetition not preceded by valid expression",
      [PS5_REG_EMPTY] = "Empty expression",
      [PS5_REG_ASSERT] = "Internal error",
      [PS5_REG_INVARG] = "Invalid argument",
      [PS5_REG_ILLSEQ] = "Illegal byte sequence",
   };
   const int index = code & ~PS5_REG_ITOA;
   const char *const message = index >= 0 && (size_t)index < sizeof(messages) / sizeof(messages[0]) &&
                                     messages[index]
                                  ? messages[index]
                                  : "Unknown error";
   const size_t length = strlen(message) + 1;
   if (size) {
      const size_t copied = length <= size ? length - 1 : size - 1;
      memcpy(buffer, message, copied);
      buffer[copied] = '\0';
   }
   return length;
}
