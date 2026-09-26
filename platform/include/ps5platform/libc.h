/*
 * PS5 Platform - the libc functions the console lacks, refuses or faults in.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each is here because of how the console behaves for a title, not because
 * of the SDK: PS5_RetroArch's docs/PLATFORM_FIRMWARE_ANALYSIS.md records
 * which system module exports what, and every behaviour below was measured
 * by one of our titles.
 *
 *   gmtime_r, localtime_r, utimensat, futimens, dirfd, clock_nanosleep,
 *   arc4random, arc4random_buf, arc4random_uniform, if_nameindex
 *                          no system module exports them
 *   openat, unlinkat, fchmodat, fstatat, mkdirat, renameat
 *                          only libkernel_sys exports them, which titles do
 *                          not import: the imports resolve to nothing
 *   statvfs, fstatvfs      exported by libc but built on statfs (only in
 *                          libkernel_sys): they fault
 *   opendir and its family exported, but refused to a title; enumeration
 *                          goes through getdents
 *   getaddrinfo, freeaddrinfo
 *                          routed by the SDK to a module titles do not load
 *   qsort_r, mkstemps, openlog, uname (__xuname), regcomp, regexec,
 *   regfree, regerror, __assert, __memset_chk
 *                          no system module exports them (the SDK's own
 *                          FreeBSD headers call the last two)
 *   newlocale, freelocale, strtod_l, strtof_l, dladdr
 *                          no system module exports them; the locale is "C"
 *   popen, pclose, open_memstream
 *                          no system module exports them, nor fork, funopen
 *                          or fopencookie to build them on: they fail as
 *                          POSIX lets them
 *
 * They carry a ps5_ prefix: a title that defined libc's own names would
 * export them, which the title converter refuses. Each consumer binds the
 * standard names to these its own way (the title's link wraps, its core
 * loader's import table). umask is not here: the kernel a title talks to has
 * none, and an emulated mask that nothing applies would only mislead; code
 * that needs a file's mode sets it after creating the file.
 */
#ifndef PS5PLATFORM_LIBC_H
#define PS5PLATFORM_LIBC_H

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#ifdef __cplusplus
extern "C" {
#endif

struct addrinfo;
struct if_nameindex;

struct tm *ps5_gmtime_r(const time_t *time, struct tm *result);
struct tm *ps5_localtime_r(const time_t *time, struct tm *result);

/* Not cryptographic: a splitmix64 generator seeded from the timestamp counter. */
uint32_t ps5_arc4random(void);
void ps5_arc4random_buf(void *buffer, size_t bytes);
uint32_t ps5_arc4random_uniform(uint32_t bound);

/* A writable filesystem with 16 GiB free: no query a title can make reports
 * the data partition's free space, and the callers (save-size checks) need
 * room. */
int ps5_statvfs(const char *path, struct statvfs *result);
int ps5_fstatvfs(int fd, struct statvfs *result);

/* Through utimes, with microsecond precision. */
int ps5_utimensat(int directory, const char *path, const struct timespec times[2], int flags);
int ps5_futimens(int fd, const struct timespec times[2]);

/* An absolute deadline becomes the interval still to go. */
int ps5_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                        struct timespec *remaining);

/* Name lookups and interface enumeration are refused as the callers expect. */
int ps5_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                    struct addrinfo **result);
void ps5_freeaddrinfo(struct addrinfo *info);
struct if_nameindex *ps5_if_nameindex(void);
void ps5_if_freenameindex(struct if_nameindex *list);

/* Directory streams through getdents. */
DIR *ps5_opendir(const char *path);
DIR *ps5_fdopendir(int fd);
struct dirent *ps5_readdir(DIR *directory);
void ps5_rewinddir(DIR *directory);
int ps5_dirfd(DIR *directory);
int ps5_closedir(DIR *directory);

/* The *at family, resolved against the path each directory descriptor was
 * opened with (recorded by ps5_openat and ps5_opendir, and checked against
 * the descriptor's device and inode before use). */
int ps5_openat(int directory, const char *name, int flags, ...);
int ps5_unlinkat(int directory, const char *name, int flags);
int ps5_fchmodat(int directory, const char *name, mode_t mode, int flags);
int ps5_fstatat(int directory, const char *name, struct stat *status, int flags);
int ps5_mkdirat(int directory, const char *name, mode_t mode);
int ps5_renameat(int from_directory, const char *from, int to_directory, const char *to);

/* qsort_r in the FreeBSD form the SDK's stdlib.h declares (the thunk before
 * the comparator, and passed to it first), over the exported qsort. */
void ps5_qsort_r(void *base, size_t count, size_t size, void *thunk,
                 int (*compare)(void *thunk, const void *a, const void *b));

/* Replaces the six X's before a suffix of suffix_length characters; the file
 * is created 0666, since a title's files stay reachable over FTP. */
int ps5_mkstemps(char *path_template, int suffix_length);

/* The exported syslog takes no identity: opening the log changes nothing. */
void ps5_openlog(const char *ident, int option, int facility);

/* A title starts no processes and has no memory-backed stdio stream: these
 * fail, with ENOSYS. */
FILE *ps5_popen(const char *command, const char *mode);
int ps5_pclose(FILE *stream);
FILE *ps5_open_memstream(char **buffer, size_t *size);

/* uname through sysctl; __xuname is what the SDK's utsname.h calls. */
int ps5___xuname(int length, void *names);

/* The C locale only (and "POSIX", and "" for a title's environment): numbers
 * parse with '.' as the decimal point whatever the global locale is. The
 * locale arguments are locale_t's. */
void *ps5_newlocale(int category_mask, const char *name, void *base);
void ps5_freelocale(void *locale);
double ps5_strtod_l(const char *s, char **end, void *locale);
float ps5_strtof_l(const char *s, char **end, void *locale);

/* FreeBSD's xlocale family in the C locale (src/xlocale.c): each does what its
 * plain counterpart does. The locale arguments are locale_t's, the catalogues
 * nl_catd's. */
struct lconv *ps5_localeconv_l(void *locale);
long long ps5_strtoll_l(const char *s, char **end, int base, void *locale);
unsigned long long ps5_strtoull_l(const char *s, char **end, int base, void *locale);
long double ps5_strtold_l(const char *s, char **end, void *locale);
int ps5_snprintf_l(char *out, size_t size, void *locale, const char *format, ...);
int ps5_sscanf_l(const char *in, void *locale, const char *format, ...);
int ps5_asprintf_l(char **out, void *locale, const char *format, ...);
int ps5_strcoll_l(const char *a, const char *b, void *locale);
size_t ps5_strxfrm_l(char *out, const char *in, size_t size, void *locale);
size_t ps5_strftime_l(char *out, size_t size, const char *format, const struct tm *time, void *locale);
int ps5_wcscoll_l(const wchar_t *a, const wchar_t *b, void *locale);
size_t ps5_wcsxfrm_l(wchar_t *out, const wchar_t *in, size_t size, void *locale);
wint_t ps5_btowc_l(int c, void *locale);
int ps5_wctob_l(wint_t c, void *locale);
int ps5_iswctype_l(wint_t c, wctype_t class_mask, void *locale);
size_t ps5_mbrlen_l(const char *s, size_t n, mbstate_t *state, void *locale);
size_t ps5_mbrtowc_l(wchar_t *out, const char *s, size_t n, mbstate_t *state, void *locale);
size_t ps5_mbsrtowcs_l(wchar_t *out, const char **in, size_t size, mbstate_t *state, void *locale);
size_t ps5_mbsnrtowcs_l(wchar_t *out, const char **in, size_t in_bytes, size_t size, mbstate_t *state,
                        void *locale);
size_t ps5_wcrtomb_l(char *out, wchar_t c, mbstate_t *state, void *locale);
size_t ps5_wcsnrtombs_l(char *out, const wchar_t **in, size_t in_chars, size_t size, mbstate_t *state,
                        void *locale);
int ps5_mbtowc_l(wchar_t *out, const char *s, size_t n, void *locale);
#if defined(__FreeBSD__)
/* FreeBSD's ctype internals, from the console's own C rune table. */
int ps5____mb_cur_max_l(void *locale);
unsigned long ps5____runetype_l(int c, void *locale);
int ps5____tolower_l(int c, void *locale);
int ps5____toupper_l(int c, void *locale);
const void *ps5___runes_for_locale(void *locale, int *mb_sb_limit);
#endif
/* A title has no message catalogue: opening one fails, and catgets gives the
 * caller's own string. */
void *ps5_catopen(const char *name, int flag);
char *ps5_catgets(void *catalogue, int set, int message, const char *fallback);
int ps5_catclose(void *catalogue);
/* The calling thread's return addresses, through the title's unwinder; the
 * symbols form writes them as addresses. */
int ps5_backtrace(void **frames, int size);
void ps5_backtrace_symbols_fd(void *const *frames, int count, int fd);

/* libc++abi's hook for C++ thread_local destructors: run, last registered
 * first, when the thread exits, or at exit() for the thread calling it. */
int ps5___cxa_thread_atexit_impl(void (*destructor)(void *), void *object, void *dso);

/* An address's object and symbol: a title's executable carries no table to
 * answer from, so this reports nothing, as dladdr does for an unknown address.
 * info is a Dl_info. */
int ps5_dladdr(const void *address, void *info);

/* The stack a thread gets when its creator asks for none (or for less): the
 * main thread's. A consumer linking with --wrap=pthread_create gets it for
 * every such thread, its libraries' included (src/threads.c). */
#define PS5_THREAD_STACK_BYTES ((size_t)2 << 20)

/* What the SDK's assert.h and fortified string.h call. */
void ps5___assert(const char *function, const char *file, int line, const char *expression)
   __attribute__((__noreturn__));
void *ps5___memset_chk(void *destination, int value, size_t length, size_t destination_length);

/* POSIX regular expressions in the SDK's FreeBSD <regex.h> form: these structs
 * are laid out as its regex_t and regmatch_t, and the flag and error values are
 * its own. The engine is musl's (src/regex/). */
struct ps5_regex {
   int re_magic;
   size_t re_nsub;
   const char *re_endp;
   void *re_g;
};
struct ps5_regmatch {
   int64_t rm_so;
   int64_t rm_eo;
};
#define PS5_REG_EXTENDED 0001
#define PS5_REG_ICASE 0002
#define PS5_REG_NOSUB 0004
#define PS5_REG_NEWLINE 0010
#define PS5_REG_NOSPEC 0020
#define PS5_REG_PEND 0040
#define PS5_REG_NOTBOL 00001
#define PS5_REG_NOTEOL 00002
#define PS5_REG_STARTEND 00004
#define PS5_REG_NOMATCH 1
#define PS5_REG_BADPAT 2
#define PS5_REG_ECOLLATE 3
#define PS5_REG_ECTYPE 4
#define PS5_REG_EESCAPE 5
#define PS5_REG_ESUBREG 6
#define PS5_REG_EBRACK 7
#define PS5_REG_EPAREN 8
#define PS5_REG_EBRACE 9
#define PS5_REG_BADBR 10
#define PS5_REG_ERANGE 11
#define PS5_REG_ESPACE 12
#define PS5_REG_BADRPT 13
#define PS5_REG_EMPTY 14
#define PS5_REG_ASSERT 15
#define PS5_REG_INVARG 16
#define PS5_REG_ILLSEQ 17
#define PS5_REG_ITOA 0400
int ps5_regcomp(struct ps5_regex *preg, const char *pattern, int cflags);
int ps5_regexec(const struct ps5_regex *preg, const char *string, size_t nmatch,
                struct ps5_regmatch *pmatch, int eflags);
void ps5_regfree(struct ps5_regex *preg);
size_t ps5_regerror(int code, const struct ps5_regex *preg, char *buffer, size_t size);

/* For the host tests: the path of name relative to a directory's path. */
int ps5_join_path(const char *directory, const char *name, char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_LIBC_H */
