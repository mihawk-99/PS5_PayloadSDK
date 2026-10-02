/* Shared CPU/GPU direct-memory availability. No allocation or cached estimates. */
#ifndef PS5PLATFORM_MEMORY_H
#define PS5PLATFORM_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ps5_memory_stats {
   uint64_t total_bytes, free_bytes, largest_bytes, flexible_bytes;
   unsigned free_ranges;
};
/* False means availability is unknown. A snapshot can change immediately;
 * callers must still handle allocation failure. Never called from a signal handler. */
bool ps5_memory_query(struct ps5_memory_stats *stats);
#ifdef __cplusplus
}
#endif
#endif
