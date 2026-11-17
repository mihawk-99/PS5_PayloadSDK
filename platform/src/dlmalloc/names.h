/*
 * PS5 Platform - the title heap's dlmalloc under names of its own.
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A title may link another dlmalloc (Boost.Container's alloc_lib.c exports
 * the same mspace API, configured differently): with the plain names the
 * linker took whichever archive came first, or refused both as duplicates.
 */
#ifndef PS5P_DLMALLOC_NAMES_H
#define PS5P_DLMALLOC_NAMES_H

#define create_mspace ps5p_dl_create_mspace
#define create_mspace_with_base ps5p_dl_create_mspace_with_base
#define destroy_mspace ps5p_dl_destroy_mspace
#define mspace_bulk_free ps5p_dl_mspace_bulk_free
#define mspace_calloc ps5p_dl_mspace_calloc
#define mspace_footprint ps5p_dl_mspace_footprint
#define mspace_footprint_limit ps5p_dl_mspace_footprint_limit
#define mspace_free ps5p_dl_mspace_free
#define mspace_independent_calloc ps5p_dl_mspace_independent_calloc
#define mspace_independent_comalloc ps5p_dl_mspace_independent_comalloc
#define mspace_mallinfo ps5p_dl_mspace_mallinfo
#define mspace_malloc ps5p_dl_mspace_malloc
#define mspace_mallopt ps5p_dl_mspace_mallopt
#define mspace_max_footprint ps5p_dl_mspace_max_footprint
#define mspace_memalign ps5p_dl_mspace_memalign
#define mspace_realloc ps5p_dl_mspace_realloc
#define mspace_realloc_in_place ps5p_dl_mspace_realloc_in_place
#define mspace_set_footprint_limit ps5p_dl_mspace_set_footprint_limit
#define mspace_track_large_chunks ps5p_dl_mspace_track_large_chunks
#define mspace_trim ps5p_dl_mspace_trim
#define mspace_usable_size ps5p_dl_mspace_usable_size

#endif
