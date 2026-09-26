/*
 * PS5 Platform - the regex engine's own interface.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * musl 1.2.5's regex.h (MIT, COPYRIGHT.musl), which the engine beside it
 * (regcomp.c, regexec.c, tre-mem.c, tre.h: musl's TRE) is written against,
 * under private names: the console's callers use FreeBSD's regex.h, whose
 * regex_t and flag values differ, and ../regex.c converts between the two.
 */
#ifndef PS5_MUSL_REGEX_H
#define PS5_MUSL_REGEX_H

#include <stddef.h>
#include <sys/types.h>

#define regex_t ps5_musl_regex_t
#define regmatch_t ps5_musl_regmatch_t
#define regoff_t ps5_musl_regoff_t
#define regcomp ps5_musl_regcomp
#define regexec ps5_musl_regexec
#define regfree ps5_musl_regfree

typedef ssize_t regoff_t;

typedef struct re_pattern_buffer {
   size_t re_nsub;
   void *__opaque, *__padding[4];
   size_t __nsub2;
   char __padding2;
} regex_t;

typedef struct {
   regoff_t rm_so;
   regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8

#define REG_NOTBOL 1
#define REG_NOTEOL 2

#define REG_OK 0
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13

#define REG_ENOSYS -1

int regcomp(regex_t *__restrict preg, const char *__restrict pattern, int cflags);
int regexec(const regex_t *__restrict preg, const char *__restrict string, size_t nmatch,
            regmatch_t *__restrict pmatch, int eflags);
void regfree(regex_t *preg);

#endif /* PS5_MUSL_REGEX_H */
