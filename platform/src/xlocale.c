/*
 * PS5 Platform - FreeBSD's xlocale family, message catalogues and backtrace
 * (include/ps5platform/libc.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libc++ (its locale, streams and filesystem), glslang, SPIRV-Cross and the
 * Vulkan CTS are written against FreeBSD's xlocale interface: the `_l`
 * functions take the locale a call uses. The console's libc exports the plain
 * functions and none of the `_l` ones. A title has one locale, "C" (newlocale
 * gives no other, src/locale.c), so each of these does what its plain
 * counterpart does in the C locale. PS5_RetroArch carried these as
 * src/locale_shims.c; they are here so every title has them, with the
 * FreeBSD ctype internals answering from the console's own C rune table
 * rather than from placeholders.
 */
#define _GNU_SOURCE 1

#include "ps5platform/libc.h"

#include <errno.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <unwind.h>
#include <wchar.h>
#include <wctype.h>

#if defined(__FreeBSD__)
#include <runetype.h>
/* The SDK's headers reach these through functions no system module exports
 * (__getCurrentRuneLocale, ___mb_cur_max); the variables are exported. */
#undef _CurrentRuneLocale
extern const _RuneLocale *_CurrentRuneLocale;
extern const _RuneLocale _DefaultRuneLocale;
extern int __mb_cur_max;
#endif

/* The SDK's FreeBSD headers make the wide classification functions macros
 * over internals no system module exports; the functions are exported. */
#undef iswalpha
#undef iswblank
#undef iswcntrl
#undef iswctype
#undef iswdigit
#undef iswgraph
#undef iswlower
#undef iswprint
#undef iswpunct
#undef iswspace
#undef iswupper
#undef iswxdigit
#undef towlower
#undef towupper

/* ------------------------------------------------------------- the C locale */

/* localeconv as POSIX gives it in the C locale, the only locale a title has.
 * The console's reports an empty decimal point (docs/PROBE.md), and code that
 * builds a number for strtod from it puts nothing where the '.' was:
 * nlohmann::json (tinygltf's parser) then read every glTF number's integer part
 * only, 0.62 as 0. Consumers bind localeconv to this (PS5_Vulkan's
 * tools/radv-link.sh), so it must not call localeconv itself. */
struct lconv *
ps5_localeconv(void)
{
   static char empty[] = "";
   static char point[] = ".";
   static struct lconv c_conventions = {
      .decimal_point = point,
      .thousands_sep = empty,
      .grouping = empty,
      .int_curr_symbol = empty,
      .currency_symbol = empty,
      .mon_decimal_point = empty,
      .mon_thousands_sep = empty,
      .mon_grouping = empty,
      .positive_sign = empty,
      .negative_sign = empty,
      .int_frac_digits = CHAR_MAX,
      .frac_digits = CHAR_MAX,
      .p_cs_precedes = CHAR_MAX,
      .p_sep_by_space = CHAR_MAX,
      .n_cs_precedes = CHAR_MAX,
      .n_sep_by_space = CHAR_MAX,
      .p_sign_posn = CHAR_MAX,
      .n_sign_posn = CHAR_MAX,
      .int_p_cs_precedes = CHAR_MAX,
      .int_n_cs_precedes = CHAR_MAX,
      .int_p_sep_by_space = CHAR_MAX,
      .int_n_sep_by_space = CHAR_MAX,
      .int_p_sign_posn = CHAR_MAX,
      .int_n_sign_posn = CHAR_MAX,
   };
   return &c_conventions;
}

struct lconv *
ps5_localeconv_l(void *locale)
{
   (void)locale;
   static struct lconv c_conventions;
   static int ready;
   if (!ready) {
      /* The C locale's conventions, whatever the global locale is. */
      c_conventions = *localeconv();
      c_conventions.decimal_point = (char *)".";
      c_conventions.thousands_sep = (char *)"";
      c_conventions.grouping = (char *)"";
      ready = 1;
   }
   return &c_conventions;
}

/* The C locale's answers, as FreeBSD's C locale gives them; an item it does
 * not have is "". No system module exports nl_langinfo (SPIRV-Cross reads
 * RADIXCHAR to print shader constants). By name, since a host's items number
 * differently. */
char *
ps5_nl_langinfo_l(int item, void *locale)
{
   static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
   static const char *const abdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
   static const char *const months[] = {"January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December"};
   static const char *const abmonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
   (void)locale;

   switch (item) {
   case CODESET: return (char *)"US-ASCII";
   case D_T_FMT: return (char *)"%a %b %e %H:%M:%S %Y";
   case D_FMT: return (char *)"%m/%d/%y";
   case T_FMT: return (char *)"%H:%M:%S";
   case T_FMT_AMPM: return (char *)"%I:%M:%S %p";
   case AM_STR: return (char *)"AM";
   case PM_STR: return (char *)"PM";
   case RADIXCHAR: return (char *)".";
   case YESEXPR: return (char *)"^[yY]";
   case NOEXPR: return (char *)"^[nN]";
#ifdef YESSTR
   case YESSTR: return (char *)"yes";
   case NOSTR: return (char *)"no";
#endif
#ifdef D_MD_ORDER
   case D_MD_ORDER: return (char *)"md";
#endif
   default: break;
   }
   if (item >= DAY_1 && item <= DAY_7)
      return (char *)days[item - DAY_1];
   if (item >= ABDAY_1 && item <= ABDAY_7)
      return (char *)abdays[item - ABDAY_1];
   if (item >= MON_1 && item <= MON_12)
      return (char *)months[item - MON_1];
   if (item >= ABMON_1 && item <= ABMON_12)
      return (char *)abmonths[item - ABMON_1];
#ifdef ALTMON_1
   if (item >= ALTMON_1 && item <= ALTMON_12)
      return (char *)months[item - ALTMON_1];
#endif
   /* THOUSEP, CRNCYSTR, the eras and alternative digits, and anything else. */
   return (char *)"";
}

char *
ps5_nl_langinfo(int item)
{
   return ps5_nl_langinfo_l(item, NULL);
}

long long
ps5_strtoll_l(const char *s, char **end, int base, void *locale)
{
   (void)locale;
   return strtoll(s, end, base);
}

unsigned long long
ps5_strtoull_l(const char *s, char **end, int base, void *locale)
{
   (void)locale;
   return strtoull(s, end, base);
}

long double
ps5_strtold_l(const char *s, char **end, void *locale)
{
   /* The C locale's point is '.', and so is the console's (src/locale.c
    * handles a title that changes the global one for double and float). */
   (void)locale;
   return strtold(s, end);
}

int
ps5_snprintf_l(char *out, size_t size, void *locale, const char *format, ...)
{
   (void)locale;
   va_list args;
   va_start(args, format);
   const int result = vsnprintf(out, size, format, args);
   va_end(args);
   return result;
}

int
ps5_sscanf_l(const char *in, void *locale, const char *format, ...)
{
   (void)locale;
   va_list args;
   va_start(args, format);
   const int result = vsscanf(in, format, args);
   va_end(args);
   return result;
}

int
ps5_asprintf_l(char **out, void *locale, const char *format, ...)
{
   (void)locale;
   va_list args;
   va_start(args, format);
   const int result = vasprintf(out, format, args);
   va_end(args);
   return result;
}

int
ps5_strcoll_l(const char *a, const char *b, void *locale)
{
   (void)locale;
   return strcoll(a, b);
}

size_t
ps5_strxfrm_l(char *out, const char *in, size_t size, void *locale)
{
   (void)locale;
   return strxfrm(out, in, size);
}

size_t
ps5_strftime_l(char *out, size_t size, const char *format, const struct tm *time, void *locale)
{
   (void)locale;
   return strftime(out, size, format, time);
}

int
ps5_wcscoll_l(const wchar_t *a, const wchar_t *b, void *locale)
{
   (void)locale;
   return wcscoll(a, b);
}

size_t
ps5_wcsxfrm_l(wchar_t *out, const wchar_t *in, size_t size, void *locale)
{
   (void)locale;
   return wcsxfrm(out, in, size);
}

/* ------------------------------------------------------ multibyte characters */

wint_t
ps5_btowc_l(int c, void *locale)
{
   (void)locale;
   return btowc(c);
}

int
ps5_wctob_l(wint_t c, void *locale)
{
   (void)locale;
   return wctob(c);
}

int
ps5_iswctype_l(wint_t c, wctype_t class_mask, void *locale)
{
   (void)locale;
   return iswctype(c, class_mask);
}

size_t
ps5_mbrlen_l(const char *s, size_t n, mbstate_t *state, void *locale)
{
   (void)locale;
   return mbrlen(s, n, state);
}

size_t
ps5_mbrtowc_l(wchar_t *out, const char *s, size_t n, mbstate_t *state, void *locale)
{
   (void)locale;
   return mbrtowc(out, s, n, state);
}

size_t
ps5_mbsrtowcs_l(wchar_t *out, const char **in, size_t size, mbstate_t *state, void *locale)
{
   (void)locale;
   return mbsrtowcs(out, in, size, state);
}

size_t
ps5_wcrtomb_l(char *out, wchar_t c, mbstate_t *state, void *locale)
{
   (void)locale;
   return wcrtomb(out, c, state);
}

int
ps5_mbtowc_l(wchar_t *out, const char *s, size_t n, void *locale)
{
   (void)locale;
   return mbtowc(out, s, n);
}

/* No system module exports mbsnrtowcs or wcsnrtombs: the bounded conversions
 * are their unbounded forms' loops over mbrtowc and wcrtomb, stopped at the
 * bound. */
size_t
ps5_mbsnrtowcs_l(wchar_t *out, const char **in, size_t in_bytes, size_t size, mbstate_t *state,
                 void *locale)
{
   (void)locale;
   const char *s = *in;
   if (!s)
      return 0;
   const char *const start = s;
   mbstate_t local = {0};
   mbstate_t *const st = state ? state : &local;
   size_t converted = 0;
   while (!out || converted < size) {
      const size_t left = in_bytes - (size_t)(s - start);
      if (left == 0)
         break;
      wchar_t c;
      const size_t used = mbrtowc(&c, s, left, st);
      if (used == (size_t)-1) {
         *in = s;
         return (size_t)-1;
      }
      if (used == (size_t)-2)
         break;
      if (used == 0) {
         if (out)
            out[converted] = L'\0';
         *in = NULL;
         return converted;
      }
      if (out)
         out[converted] = c;
      converted++;
      s += used;
   }
   if (out)
      *in = s;
   return converted;
}

size_t
ps5_wcsnrtombs_l(char *out, const wchar_t **in, size_t in_chars, size_t size, mbstate_t *state,
                 void *locale)
{
   (void)locale;
   const wchar_t *s = *in;
   if (!s)
      return 0;
   mbstate_t local = {0};
   mbstate_t *const st = state ? state : &local;
   size_t written = 0;
   for (size_t i = 0; i < in_chars; i++) {
      char bytes[MB_LEN_MAX];
      mbstate_t before = *st;
      const size_t used = wcrtomb(bytes, s[i], st);
      if (used == (size_t)-1) {
         *in = s + i;
         return (size_t)-1;
      }
      if (out && written + used > size) {
         *st = before;
         *in = s + i;
         return written;
      }
      if (out)
         memcpy(out + written, bytes, used);
      if (s[i] == L'\0') {
         if (out)
            *in = NULL;
         return written + used - 1;
      }
      written += used;
   }
   if (out)
      *in = s + in_chars;
   return written;
}

/* ------------------------------------------------- FreeBSD's ctype internals */

#if defined(__FreeBSD__)
int
ps5____mb_cur_max_l(void *locale)
{
   (void)locale;
   return __mb_cur_max;
}

/* The class of a character past the rune table's cache (FreeBSD's
 * _CACHED_RUNES), as the table would give it: the exported wide
 * classification functions answer for the C locale. */
unsigned long
ps5____runetype_l(int c, void *locale)
{
   (void)locale;
   if (c < 0)
      return 0;
   const wint_t w = (wint_t)c;
   unsigned long mask = 0;
   if (iswalpha(w))
      mask |= _CTYPE_A;
   if (iswcntrl(w))
      mask |= _CTYPE_C;
   if (iswdigit(w))
      mask |= _CTYPE_D;
   if (iswgraph(w))
      mask |= _CTYPE_G;
   if (iswlower(w))
      mask |= _CTYPE_L;
   if (iswpunct(w))
      mask |= _CTYPE_P;
   if (iswspace(w))
      mask |= _CTYPE_S;
   if (iswupper(w))
      mask |= _CTYPE_U;
   if (iswxdigit(w))
      mask |= _CTYPE_X;
   if (iswblank(w))
      mask |= _CTYPE_B;
   if (iswprint(w))
      mask |= _CTYPE_R;
   return mask;
}

int
ps5____tolower_l(int c, void *locale)
{
   (void)locale;
   return c < 0 ? c : (int)towlower((wint_t)c);
}

int
ps5____toupper_l(int c, void *locale)
{
   (void)locale;
   return c < 0 ? c : (int)towupper((wint_t)c);
}

/* A locale's rune table: the console's own C table, which its libc exports,
 * and the single-byte limit that goes with it. */
const void *
ps5___runes_for_locale(void *locale, int *mb_sb_limit)
{
   (void)locale;
   if (mb_sb_limit)
      *mb_sb_limit = __mb_sb_limit;
   return _CurrentRuneLocale ? _CurrentRuneLocale : &_DefaultRuneLocale;
}
#endif

/* -------------------------------------------------------- message catalogues */

/* A title ships its own strings and has no catalogue to open. */
void *
ps5_catopen(const char *name, int flag)
{
   (void)name;
   (void)flag;
   errno = ENOENT;
   return (void *)-1;
}

char *
ps5_catgets(void *catalogue, int set, int message, const char *fallback)
{
   (void)catalogue;
   (void)set;
   (void)message;
   errno = EBADF;
   return (char *)fallback;
}

int
ps5_catclose(void *catalogue)
{
   (void)catalogue;
   errno = EBADF;
   return -1;
}

/* ---------------------------------------------------------------- backtrace */

struct backtrace_walk {
   void **frames;
   int size;
   int count;
};

static _Unwind_Reason_Code
backtrace_frame(struct _Unwind_Context *context, void *opaque)
{
   struct backtrace_walk *const walk = opaque;
   if (walk->count >= walk->size)
      return _URC_END_OF_STACK;
   const uintptr_t ip = _Unwind_GetIP(context);
   if (ip == 0)
      return _URC_END_OF_STACK;
   walk->frames[walk->count++] = (void *)ip;
   return _URC_NO_REASON;
}

/* The return addresses of the calling thread's frames, through the unwinder
 * the title links (libunwind over the executable's .eh_frame). */
int
ps5_backtrace(void **frames, int size)
{
   struct backtrace_walk walk = {.frames = frames, .size = size > 0 ? size : 0, .count = 0};
   if (walk.size)
      _Unwind_Backtrace(backtrace_frame, &walk);
   return walk.count;
}

/* One address a line: a title's executable carries no symbol table to name
 * them from; the PC symbolises them against the linked ELF. */
void
ps5_backtrace_symbols_fd(void *const *frames, int count, int fd)
{
   /* No system module exports dprintf. */
   for (int i = 0; i < count; i++) {
      char line[32];
      const int length = snprintf(line, sizeof(line), "%p\n", frames[i]);
      if (length > 0)
         (void)!write(fd, line, (size_t)length);
   }
}
