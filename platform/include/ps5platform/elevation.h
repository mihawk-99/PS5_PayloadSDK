/*
 * PS5 Platform - /data for a sandboxed title, by asking the Lapy daemon (src/elevation.c).
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from the cooperative elevation client of ps5-native-app-boilerplate
 * (Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later), which implements the
 * application side of the contract documented by mpereiraesaa/PS5-Lapy-JB-Daemon.
 * The daemon, and the kernel work it does, belong to that upstream project; this
 * file contains none of it and never touches kernel state. docs/ELEVATION.md has the
 * contract, how the daemon is run, and what to do with a title that used the
 * helper ELF this replaces.
 *
 * A title starts able to see only its own sandbox (/app0 and /download0). With a
 * Lapy owned-root daemon waiting, ps5_elevation_request makes the title's process
 * privileged enough for /data and proves it with a real write and read there: only
 * PS5_ELEVATION_OK permits using /data. Call it once, during single-threaded
 * startup, before any worker thread exists. Any other status, a missing daemon
 * included, authorizes nothing; the caller decides whether to go on without /data
 * or to stop, and never to fall back on another way of elevating.
 */
#ifndef PS5PLATFORM_ELEVATION_H
#define PS5PLATFORM_ELEVATION_H

#ifdef __cplusplus
extern "C" {
#endif

enum ps5_elevation_capability {
   PS5_ELEVATION_FILESYSTEM = 1
};

/* The values are Lapy's client results, as the boilerplate reports them. */
enum ps5_elevation_status {
   PS5_ELEVATION_OK = 0,
   PS5_ELEVATION_INVALID_REQUEST = 1,
   PS5_ELEVATION_UNSUPPORTED_CAPABILITY = 3,
   PS5_ELEVATION_UNAVAILABLE = 5,      /* /download0/lapy_owned_result cannot be opened */
   PS5_ELEVATION_PREPARE_FAILED = 6,   /* seteuid(geteuid()) failed */
   PS5_ELEVATION_APPLY_FAILED = 7,     /* /data opened and the write/read proof did not match */
   PS5_ELEVATION_TRANSPORT_ERROR = 9,  /* the request could not be published */
   PS5_ELEVATION_TIMEOUT = 11          /* no daemon made /data reachable in ten seconds */
};

/* The files and limits of the contract; the defaults are Lapy's. Only a test
 * has a reason to change them. */
struct ps5_elevation_config {
   const char *request_path;      /* "/download0/elevate_proc": the request the daemon takes */
   const char *request_directory; /* "/download0": where the request is written before it is renamed */
   const char *result_path;       /* "/download0/lapy_owned_result": the client's report to the daemon */
   const char *data_directory;    /* "/data": where the proof file is written */
   unsigned polls;                /* 200 */
   unsigned poll_interval_us;     /* 50000 */
};

/* One request with the contract's own paths. */
enum ps5_elevation_status ps5_elevation_request(enum ps5_elevation_capability capability);
enum ps5_elevation_status ps5_elevation_request_with(const struct ps5_elevation_config *config,
                                                     enum ps5_elevation_capability capability);

/* "ok", "timeout", ...; for a log line. */
const char *ps5_elevation_status_name(enum ps5_elevation_status status);

#ifdef __cplusplus
}
#endif

#endif /* PS5PLATFORM_ELEVATION_H */
