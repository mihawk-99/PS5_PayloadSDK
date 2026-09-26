/*
 * PS5 Platform - the regex structs are the SDK's (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A consumer compiled against the SDK's FreeBSD <regex.h> binds regcomp and
 * its family to ps5_regcomp and the rest, so struct ps5_regex and
 * struct ps5_regmatch must be that header's regex_t and regmatch_t, byte for
 * byte, and the flag values its own. The console build checks it here; the
 * host tests build against another libc's regex.h and skip it.
 */
#include "ps5platform/libc.h"

#if defined(__PROSPERO__)
#include <regex.h>
#include <stddef.h>

_Static_assert(sizeof(regex_t) == sizeof(struct ps5_regex), "regex_t");
_Static_assert(offsetof(regex_t, re_magic) == offsetof(struct ps5_regex, re_magic), "re_magic");
_Static_assert(offsetof(regex_t, re_nsub) == offsetof(struct ps5_regex, re_nsub), "re_nsub");
_Static_assert(offsetof(regex_t, re_endp) == offsetof(struct ps5_regex, re_endp), "re_endp");
_Static_assert(offsetof(regex_t, re_g) == offsetof(struct ps5_regex, re_g), "re_g");
_Static_assert(sizeof(regmatch_t) == sizeof(struct ps5_regmatch), "regmatch_t");
_Static_assert(offsetof(regmatch_t, rm_eo) == offsetof(struct ps5_regmatch, rm_eo), "rm_eo");
_Static_assert(REG_EXTENDED == PS5_REG_EXTENDED && REG_ICASE == PS5_REG_ICASE &&
                  REG_NOSUB == PS5_REG_NOSUB && REG_NEWLINE == PS5_REG_NEWLINE &&
                  REG_NOSPEC == PS5_REG_NOSPEC && REG_PEND == PS5_REG_PEND,
               "regcomp flags");
_Static_assert(REG_NOTBOL == PS5_REG_NOTBOL && REG_NOTEOL == PS5_REG_NOTEOL &&
                  REG_STARTEND == PS5_REG_STARTEND,
               "regexec flags");
_Static_assert(REG_NOMATCH == PS5_REG_NOMATCH && REG_ESPACE == PS5_REG_ESPACE &&
                  REG_ILLSEQ == PS5_REG_ILLSEQ && REG_ITOA == PS5_REG_ITOA,
               "error codes");
#endif

/* An object file with nothing in it is still one. */
typedef int ps5_regex_abi_checked;
