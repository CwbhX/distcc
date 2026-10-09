/* Private on-host format used by the compiler filesystem observer. */
#ifndef DISTCC_MIRROR_TRACE_H
#define DISTCC_MIRROR_TRACE_H

#include <stdint.h>

#define DCC_MIRROR_TRACE_ENV "DISTCC_MIRROR_TRACE"
#define DCC_MIRROR_TRACE_MAGIC UINT32_C(0x44545231)
#define DCC_MIRROR_TRACE_START 1
#define DCC_MIRROR_TRACE_END 2
#define DCC_MIRROR_TRACE_QUERY 3
#define DCC_MIRROR_TRACE_ERROR 4
#define DCC_MIRROR_TRACE_LQUERY 5

/* Native endian: this file is read on the same host that produced it. Each
 * header is followed by path_length bytes, without a terminating NUL. Paths
 * are absolute. QUERY preserves errno and the observed file type; successful
 * regular files must also be verified by content before accepting the job.
 * Every process must have matching START/END records and no ERROR record.
 * Unknown types, invalid lengths and truncated records invalidate the trace.
 */
struct dcc_mirror_trace_record {
    uint32_t magic;
    uint32_t type;
    uint32_t pid;
    uint32_t error;
    uint32_t mode;
    uint32_t path_length;
};

#endif
