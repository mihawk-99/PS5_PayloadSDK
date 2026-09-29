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
 *   arc4random, arc4random_buf, arc4random_uniform, if_nameindex, strcasestr,
 *   memccpy, times, sockatmark, getpwuid, getpwnam_r, posix_madvise, strsignal,
 *   gethostbyaddr, tmpfile,
 *   if_nametoindex, if_indextoname, mkstemp, isatty, link, symlink, readlink,
 *   fchown
 *                          no system module exports them
 *   openat, unlinkat, fchmodat, fstatat, mkdirat, renameat
 *                          only libkernel_sys exports them, which titles do
 *                          not import: the imports resolve to nothing
 *   statvfs, fstatvfs      exported by libc but built on statfs (only in
 *                          libkernel_sys): they fault
 *   opendir and its family exported, but refused to a title; enumeration
 *                          goes through getdents
 *   getaddrinfo, freeaddrinfo, getnameinfo, gethostbyname, gai_strerror
 *                          routed by the SDK to a module titles do not load
 *   qsort_r, mkstemps, openlog, uname (__xuname), regcomp, regexec,
 *   regfree, regerror, __assert, __memset_chk
 *                          no system module exports them (the SDK's own
 *                          FreeBSD headers call the last two)
 *   newlocale, freelocale, strtod_l, strtof_l, dladdr
 *                          no system module exports them; the locale is "C"
 *   popen, pclose          no system module exports them, nor fork: they fail
 *                          as POSIX lets them
 *   open_memstream         no system module exports it, nor funopen,
 *                          fopencookie or fmemopen: libc's FILE on a pipe,
 *                          published through fclose and fflush wraps
 *   memfd_create           no system module exports it: libkernel's anonymous
 *                          shared memory object, as FreeBSD 13 builds it
 *   accept4, getpagesizes, in6addr_any
 *                          no system module exports them
 *   realpath               refused to a title (EPERM), even for /app0: resolved
 *                          from the names and stat(), which libc++'s
 *                          std::filesystem canonical paths are built on
 *   syscall                a title may make no system call: write for SYS_write
 *                          (Abseil's raw logging), ENOSYS for the rest
 *   sysconf                exported, but _SC_NPROCESSORS_ONLN and _CONF answer
 *                          16 where a title's threads run on 13
 *   pthread_getaffinity_np, pthread_setaffinity_np
 *                          exported, but refuse sets larger than 16 bytes
 *                          (ERANGE), FreeBSD's 32-byte cpuset_t among them:
 *                          the exported 64-bit mask form answers instead
 *
 * They carry a ps5_ prefix: a title that defined libc's own names would
 * export them, which the title converter refuses. Each consumer binds the
 * standard names to these its own way (the title's link wraps, its core
 * loader's import table). The kernel a title talks to applies no file mode
 * mask: ps5_umask says so (0) and keeps nothing, since an emulated mask that
 * nothing applies would only mislead; code that needs a file's mode sets it
 * after creating the file.
 *
 *   statfs, fstatfs, umask, fork, setsid, wait4
 *                          only libkernel_sys exports them: the imports
 *                          resolve to nothing
 */
#ifndef PS5PLATFORM_LIBC_H
#define PS5PLATFORM_LIBC_H

#include <dirent.h>
#include <pthread.h>
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
struct hostent;
struct in6_addr;
struct sockaddr;
struct if_nameindex;
struct passwd;
struct tms;

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
/* The same answer in FreeBSD's struct statfs (<sys/mount.h>), with no device
 * behind the volume (f_mntfromname empty). result is a struct statfs. */
int ps5_statfs(const char *path, void *result);
int ps5_fstatfs(int fd, void *result);

/* No file mode mask is applied: always 0, and nothing is kept. */
mode_t ps5_umask(mode_t mask);

/* A title starts no process: fork fails with ENOSYS, setsid with EPERM, wait4
 * with ECHILD. */
pid_t ps5_fork(void);
pid_t ps5_setsid(void);
pid_t ps5_wait4(pid_t pid, int *status, int options, void *usage);

/* The first occurrence of needle in haystack, ignoring the case of ASCII
 * letters (the locale is "C"), as FreeBSD's strcasestr. */
char *ps5_strcasestr(const char *haystack, const char *needle);

/* A title has no user database: every lookup finds no entry (0, *result
 * NULL), as POSIX answers for a user it does not know. */
int ps5_getpwuid_r(uid_t uid, struct passwd *entry, char *buffer, size_t size, struct passwd **result);
struct passwd *ps5_getpwuid(uid_t uid);
/* The title has no user database: no user is found, by name either. */
int ps5_getpwnam_r(const char *name, struct passwd *entry, char *buffer, size_t size,
                   struct passwd **result);

/* posix_madvise over madvise: an error number, not errno, as POSIX says. */
int ps5_posix_madvise(void *address, size_t length, int advice);

/* FreeBSD's description of a signal ("Segmentation fault"), or "Unknown
 * signal: N" in a buffer of the calling thread's. */
char *ps5_strsignal(int signal);

/* Copies up to size bytes, stopping after the first byte equal to c; the
 * address after that byte, or NULL when it was not found. */
void *ps5_memccpy(void *destination, const void *source, int c, size_t size);

/* The process's CPU time in *buffer (its children have none) and the time
 * since the title started, both in clock ticks (CLK_TCK). */
clock_t ps5_times(struct tms *buffer);

/* Whether the socket's read pointer is at the out-of-band mark, through
 * SIOCATMARK, as FreeBSD implements it. */
int ps5_sockatmark(int fd);

/* accept, then SOCK_CLOEXEC and SOCK_NONBLOCK applied to the new socket, as
 * FreeBSD's accept4 does. */
int ps5_accept4(int fd, struct sockaddr *address, unsigned int *length, int flags);

/* The page sizes mappings use: the kernel's 16 KiB page, the one size a
 * title's mappings are made of. As FreeBSD's getpagesizes: with no array and
 * a count of 0, how many sizes there are; otherwise how many were stored. */
int ps5_getpagesizes(size_t sizes[], int count);

/* The IPv6 wildcard address (IN6ADDR_ANY_INIT). */
extern const struct in6_addr ps5_in6addr_any;

/* A thread's CPUs, as FreeBSD's pthread_getaffinity_np and
 * pthread_setaffinity_np take them (a cpuset_t, or a set of another size),
 * through the exported scePthreadGetaffinity and scePthreadSetaffinity and
 * their 64-bit mask: CPU n is bit n. A set naming CPUs past 63, or none, is
 * refused with EINVAL. Error numbers are returned, as pthread functions do. */
int ps5_pthread_getaffinity_np(pthread_t thread, size_t size, void *set);

/* realpath without the refused call: the path made absolute (getcwd), "." and
 * ".." resolved by name, "//" collapsed, and every component checked with
 * stat(): ENOENT for a missing one, ENOTDIR for a file with more below it.
 * Symbolic links are not followed, since a title cannot read them. With no
 * buffer, a PATH_MAX one is allocated with malloc, as POSIX says. */
char *ps5_realpath(const char *path, char *resolved);

/* syscall(2) without a system call: SYS_write is write(); any other number
 * fails with ENOSYS, as a system without that call answers. */
long ps5_syscall(long number, ...);

/* sysconf, with the CPUs a title's threads may run on for _SC_NPROCESSORS_ONLN
 * and _SC_NPROCESSORS_CONF: the CPUs in the first caller's affinity mask
 * (thirteen on the console, where sysconf says 16), so thread pools sized by it
 * do not ask for CPUs the title never gets. Everything else is sysconf's. */
long ps5_sysconf(int name);
int ps5_pthread_setaffinity_np(pthread_t thread, size_t size, const void *set);

/* Makes the file at least offset + length bytes long with the bytes past its
 * end written as zeros, so the space is taken, not a hole; bytes already in
 * the file are left alone. Returns 0 or an errno value, as POSIX says. */
int ps5_posix_fallocate(int fd, off_t offset, off_t length);

/* A title's sandbox refuses access() for every path, existing or not
 * (EPERM, measured on the console), while stat() answers. Existence is asked
 * of stat(); reading and writing a file, of opening it, since its mode bits do
 * not predict what a title may do (a title writes files of mode 0644 that
 * report another owner); reading a folder, of opening it. Writing a folder
 * and executing or searching are judged by the mode's write and execute bits
 * (any of the three classes). Fails with errno set, as access() does. */
int ps5_access(const char *path, int mode);

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
/* FreeBSD's message for each EAI_* code. */
const char *ps5_gai_strerror(int error);
struct hostent *ps5_gethostbyaddr(const void *address, unsigned int length, int type);
struct hostent *ps5_gethostbyname(const char *name);
int ps5_getnameinfo(const void *address, unsigned int length, char *host, unsigned int host_size,
                    char *service, unsigned int service_size, int flags);
struct if_nameindex *ps5_if_nameindex(void);
void ps5_if_freenameindex(struct if_nameindex *list);
unsigned int ps5_if_nametoindex(const char *name);
char *ps5_if_indextoname(unsigned int index, char *name);

/* The working directory, found by walking up from "." (libc's getcwd faults
 * for a title: the __getcwd it calls is only in libkernel_sys). NULL buffer:
 * malloc'd, of at least size bytes. */
char *ps5_getcwd(char *buffer, size_t size);

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
/* An unnamed read-write file, removed as it is created: in $TMPDIR, or in the
 * title's own /app0/tmp (made 0777). */
FILE *ps5_tmpfile(void);
int ps5_mkstemp(char *path_template);

/* A title has no terminals, no links and no users to give a file to: isatty
 * answers no terminal (ENOTTY, or EBADF for a closed descriptor), link and
 * symlink are refused as on a file system without them, readlink finds that an
 * existing path is no link (EINVAL), and fchown is not permitted (EPERM). */
int ps5_isatty(int fd);
int ps5_link(const char *existing, const char *name);
int ps5_symlink(const char *target, const char *name);
ssize_t ps5_readlink(const char *path, char *buffer, size_t size);
int ps5_fchown(int fd, uid_t owner, gid_t group);

/* The exported syslog takes no identity: opening the log changes nothing. */
void ps5_openlog(const char *ident, int option, int facility);

/* A title starts no processes: these fail, with ENOSYS. */
FILE *ps5_popen(const char *command, const char *mode);
int ps5_pclose(FILE *stream);

/* POSIX open_memstream (src/memstream.c): libc's own FILE on a pipe, which a
 * reader thread drains into a buffer from malloc. As POSIX says, *buffer (NUL-
 * terminated) and *size (without the NUL) are brought up to date by fflush and
 * fclose, and after fclose the buffer is the caller's to free. That needs the
 * consumer's link to wrap both (--wrap=fclose --wrap=fflush, which this layer's
 * __wrap_fclose and __wrap_fflush serve); without the wraps it fails with
 * ENOSYS. */
FILE *ps5_open_memstream(char **buffer, size_t *size);
int __wrap_fclose(FILE *stream);
int __wrap_fflush(FILE *stream);

/* memfd_create as FreeBSD 13 builds it, on an anonymous shared memory object
 * (libkernel's shm_open with SHM_ANON): a descriptor ftruncate sizes, whose
 * MAP_SHARED mappings all show the same pages. The name is only a label, as
 * on Linux. PS5_MFD_CLOEXEC (MFD_CLOEXEC) is the one flag: the kernel has no
 * seals to add or huge pages to ask for, so any other fails with EINVAL. */
#define PS5_MFD_CLOEXEC 0x00000001u
int ps5_memfd_create(const char *name, unsigned int flags);

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
/* The item's string in the C locale, "" for one it lacks; item is an
 * nl_item. */
char *ps5_nl_langinfo(int item);
char *ps5_nl_langinfo_l(int item, void *locale);
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

/* pthread_exit, after the calling thread's C++ thread_local destructors, which
 * the platform runs before libkernel's key destructors (src/cxa.c). */
void ps5_pthread_exit(void *value) __attribute__((__noreturn__));

/* Thread stacks in direct memory (src/threads.c): those of threads not yet
 * ended and joined, and freed ones kept for the next threads. */
void ps5_thread_stacks(unsigned *live, unsigned *cached);

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
