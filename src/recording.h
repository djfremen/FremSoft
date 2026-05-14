// FremSoft — recording loader. Reads JSON from
// fremsoft/recordings/*.json (built by fremsoft/tools/build_recording.py)
// and exposes O(1) lookup by (can_id, uds_request_bytes).

#ifndef FREMSOFT_RECORDING_H
#define FREMSOFT_RECORDING_H

#include <stdint.h>

typedef struct {
    uint32_t can_id;          // big-endian on the wire, host-order here
    uint32_t uds_len;
    uint8_t* uds_bytes;       // owned by the recording
    uint32_t delay_ms;        // delay relative to the previous frame
} recorded_response_t;

typedef struct {
    uint32_t can_id;          // host-order
    uint32_t uds_len;
    uint8_t* uds_bytes;       // request key
    uint32_t num_responses;
    recorded_response_t* responses;
} recorded_exchange_t;

typedef struct recording_t recording_t;  // opaque

// Load a JSON recording into memory. Returns NULL on failure.
// Allocates everything internally; free with recording_free().
recording_t* recording_load(const char* json_path);
void         recording_free(recording_t* r);

// Look up the responses for a given (can_id, uds_bytes) request.
// Returns the matched exchange (do NOT free the returned pointer)
// or NULL if no match.
const recorded_exchange_t* recording_lookup(
    const recording_t* r,
    uint32_t can_id,
    const uint8_t* uds_bytes,
    uint32_t uds_len);

// Number of unique (can_id, uds) keys in the recording.
uint32_t recording_size(const recording_t* r);

// Diagnostic — write recording metadata to the FremSoft log.
void recording_log_summary(const recording_t* r);

#endif // FREMSOFT_RECORDING_H
