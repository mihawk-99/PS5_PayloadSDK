/*
 * PS5 Platform - a client of an FTP server on the console itself.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Writing through another process. The console holds a title's writes to about
 * 2 MiB/s once a burst of about 1.3 GiB is spent, whether they go through
 * write(), O_DIRECT or a shared mapping, while an FTP server in another process
 * writes the same folder at full speed (docs/PROBE.md, "Sustained writes").
 * This client talks to such a server on 127.0.0.1: anonymous login, binary
 * transfers, passive data connections. Only sockets are used.
 */
#ifndef PS5PLATFORM_FTP_H
#define PS5PLATFORM_FTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ps5_ftp {
   int control;       /* the control connection, -1 when closed */
   int code;          /* the last reply's code */
   char reply[256];   /* the last reply's last line */
};

/* Connects to the server on 127.0.0.1:`port` and logs in anonymously, in
 * binary mode. Returns 0, or -1 with `ftp` closed. */
int ps5_ftp_open(struct ps5_ftp *ftp, unsigned port);

/* Sends one command (printf-style, without the line end) and returns the
 * reply's code, or -1 when the connection failed. */
int ps5_ftp_command(struct ps5_ftp *ftp, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* Starts appending to the server's file `path` (APPE, which creates it when
 * missing and never truncates it, so a file the caller already has open stays
 * the same file). Returns the data connection to send the bytes on, or -1. */
int ps5_ftp_append_begin(struct ps5_ftp *ftp, const char *path);

/* Sends all of `bytes` on a data connection. Returns 0, or -1. */
int ps5_ftp_send(int data, const void *bytes, size_t size);

/* Closes a data connection and waits for the server to confirm the transfer.
 * Returns 0 when it did (the bytes are in the file), or -1. */
int ps5_ftp_transfer_end(struct ps5_ftp *ftp, int data);

/* The first port in [first, last] on 127.0.0.1 whose service greets with an
 * FTP "220" within `greeting_ms`, or 0. A closed port refuses at once, so a
 * whole range takes about a second; a service that says something else, or
 * nothing, is left at once or after `greeting_ms`. */
unsigned ps5_ftp_find_local(unsigned first, unsigned last, int greeting_ms);

/* Logs out and closes the control connection. */
void ps5_ftp_close(struct ps5_ftp *ftp);

#ifdef __cplusplus
}
#endif

#endif
