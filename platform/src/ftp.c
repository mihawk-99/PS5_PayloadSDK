/*
 * PS5 Platform - a client of an FTP server on the console itself
 * (include/ps5platform/ftp.h).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ps5platform/ftp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int
ftp_connect_local(unsigned port)
{
   const int fd = socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   struct sockaddr_in address;
   memset(&address, 0, sizeof(address));
   address.sin_family = AF_INET;
   address.sin_port = htons((unsigned short)port);
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   const struct timeval timeout = {.tv_sec = 10};
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
   if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
      close(fd);
      return -1;
   }
   return fd;
}

/* Reads one line of the control connection, without its line end. */
static int
ftp_read_line(struct ps5_ftp *ftp, char *line, size_t size)
{
   size_t used = 0;
   for (;;) {
      char c;
      const ssize_t got = recv(ftp->control, &c, 1, 0);
      if (got <= 0)
         return -1;
      if (c == '\n')
         break;
      if (c != '\r' && used + 1 < size)
         line[used++] = c;
   }
   line[used] = '\0';
   return 0;
}

/* Reads a reply, one line or several ("123-" up to "123 "), and returns its code. */
static int
ftp_read_reply(struct ps5_ftp *ftp)
{
   char line[sizeof(ftp->reply)];
   if (ftp_read_line(ftp, line, sizeof(line)) != 0 || strlen(line) < 3)
      return ftp->code = -1;
   const int code = atoi(line);
   if (line[3] == '-') {
      char end[5];
      snprintf(end, sizeof(end), "%.3s ", line);
      do {
         if (ftp_read_line(ftp, line, sizeof(line)) != 0)
            return ftp->code = -1;
      } while (strncmp(line, end, 4) != 0);
   }
   snprintf(ftp->reply, sizeof(ftp->reply), "%s", line);
   return ftp->code = code;
}

int
ps5_ftp_command(struct ps5_ftp *ftp, const char *format, ...)
{
   if (ftp->control < 0)
      return -1;
   char command[600];
   va_list arguments;
   va_start(arguments, format);
   const int length = vsnprintf(command, sizeof(command) - 2, format, arguments);
   va_end(arguments);
   if (length < 0 || (size_t)length >= sizeof(command) - 2)
      return -1;
   memcpy(command + length, "\r\n", 2);
   if (ps5_ftp_send(ftp->control, command, (size_t)length + 2) != 0)
      return ftp->code = -1;
   return ftp_read_reply(ftp);
}

int
ps5_ftp_open(struct ps5_ftp *ftp, unsigned port)
{
   memset(ftp, 0, sizeof(*ftp));
   ftp->control = ftp_connect_local(port);
   if (ftp->control < 0)
      return -1;
   int code = ftp_read_reply(ftp);
   if (code == 220)
      code = ps5_ftp_command(ftp, "USER anonymous");
   if (code == 331)
      code = ps5_ftp_command(ftp, "PASS anonymous");
   if (code == 230 || code == 202)
      code = ps5_ftp_command(ftp, "TYPE I");
   if (code != 200) {
      close(ftp->control);
      ftp->control = -1;
      return -1;
   }
   return 0;
}

/* A passive data connection, then `command` on it; the data connection when
 * the server starts the transfer, or -1. */
static int
ftp_transfer_begin(struct ps5_ftp *ftp, const char *command, const char *path)
{
   if (ps5_ftp_command(ftp, "PASV") != 227)
      return -1;
   const char *numbers = strchr(ftp->reply, '(');
   unsigned h[4], p[2];
   if (!numbers || sscanf(numbers, "(%u,%u,%u,%u,%u,%u)", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) != 6)
      return -1;
   /* The server's own address may be any of the console's; the data
    * connection goes to the loopback like the control one. */
   const int data = ftp_connect_local(p[0] * 256 + p[1]);
   if (data < 0)
      return -1;
   const int code = ps5_ftp_command(ftp, "%s %s", command, path);
   if (code != 150 && code != 125) {
      close(data);
      return -1;
   }
   return data;
}

long long
ps5_ftp_size(struct ps5_ftp *ftp, const char *path)
{
   if (ps5_ftp_command(ftp, "SIZE %s", path) != 213)
      return -1;
   return strtoll(ftp->reply + 4, NULL, 10);
}

int
ps5_ftp_list(struct ps5_ftp *ftp, const char *path, void (*entry)(void *context, const char *name, int directory),
             void *context)
{
   const int data = ftp_transfer_begin(ftp, "LIST", path);
   if (data < 0)
      return -1;
   char line[1024];
   size_t used = 0;
   char block[4096];
   ssize_t got;
   while ((got = recv(data, block, sizeof(block), 0)) > 0) {
      for (ssize_t i = 0; i < got; ++i) {
         const char c = block[i];
         if (c != '\n') {
            if (c != '\r' && used + 1 < sizeof(line))
               line[used++] = c;
            continue;
         }
         line[used] = '\0';
         used = 0;
         /* "drwxrwxrwx 1 0 0 65536 Jan 23 11:44 name": the name follows
          * eight fields. */
         const char *name = line;
         for (int field = 0; field < 8 && *name; ++field) {
            while (*name && *name != ' ')
               ++name;
            while (*name == ' ')
               ++name;
         }
         if (*name && strcmp(name, ".") && strcmp(name, ".."))
            entry(context, name, line[0] == 'd');
      }
   }
   return ps5_ftp_transfer_end(ftp, data);
}

int
ps5_ftp_append_begin(struct ps5_ftp *ftp, const char *path)
{
   const int data = ftp_transfer_begin(ftp, "APPE", path);
   if (data < 0)
      return -1;
   const int size = 4 << 20, on = 1;
   setsockopt(data, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
   /* The server's writes to storage fill its receive window; with Nagle's
    * algorithm the last small segment then waits for an ACK the server delays,
    * and the transfer runs at the delayed-ACK timer's pace. */
   setsockopt(data, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
   return data;
}

int
ps5_ftp_send(int data, const void *bytes, size_t size)
{
   const char *at = bytes;
   while (size) {
      const ssize_t sent = send(data, at, size, 0);
      if (sent < 0 && errno == EINTR)
         continue;
      if (sent <= 0)
         return -1;
      at += sent;
      size -= (size_t)sent;
   }
   return 0;
}

int
ps5_ftp_transfer_end(struct ps5_ftp *ftp, int data)
{
   close(data);
   const int code = ftp_read_reply(ftp);
   return code == 226 || code == 250 ? 0 : -1;
}

unsigned
ps5_ftp_find_local(unsigned first, unsigned last, int greeting_ms)
{
   for (unsigned port = first; port && port <= last && port <= 65535; ++port) {
      const int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
         return 0;
      struct sockaddr_in address;
      memset(&address, 0, sizeof(address));
      address.sin_family = AF_INET;
      address.sin_port = htons((unsigned short)port);
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
         close(fd);
         continue;
      }
      const struct timeval timeout = {.tv_sec = greeting_ms / 1000, .tv_usec = greeting_ms % 1000 * 1000};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      char greeting[4] = {0};
      const ssize_t got = recv(fd, greeting, 3, MSG_WAITALL);
      close(fd);
      if (got == 3 && !memcmp(greeting, "220", 3))
         return port;
   }
   return 0;
}

void
ps5_ftp_close(struct ps5_ftp *ftp)
{
   if (ftp->control < 0)
      return;
   ps5_ftp_command(ftp, "QUIT");
   close(ftp->control);
   ftp->control = -1;
}
