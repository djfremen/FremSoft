// FremSoft — event scheduler.
//
// When PDUStartComPrimitive matches a recorded exchange, we schedule
// one sched_event_t per recorded RX frame, with `due_us` set to
// now + sum_of_delays. The scheduler maintains a min-heap-ish ordered
// list per (hMod, hCLL) and:
//
//  - Returns the next due event when PDUGetEventItem polls.
//  - Fires the registered callback (set via PDURegisterEventCallback)
//    on a background thread when a callback is registered AND the
//    event becomes due, so Tech2Win sees responses asynchronously
//    just like real ECM traffic.

#ifndef FREMSOFT_SCHEDULER_H
#define FREMSOFT_SCHEDULER_H

#include <stdint.h>

typedef struct {
    uint64_t due_us;          // monotonic microseconds since fremsoft_init
    uint32_t h_mod;
    uint32_t h_cll;
    uint32_t h_cop;           // matches StartComPrimitive's returned handle
    void*    cop_tag;         // caller's tag (Tech2 keeps these per CLL)
    uint32_t item_type;       // 0x1300 PDU_IT_RESULT (most common)
                              // 0x1304 PDU_IT_ERROR  (used for unknown-request NRC)
    uint32_t num_data;
    uint8_t* data_bytes;      // owned by the scheduler, freed on PDUDestroyItem
                              // For RESULT items the convention is:
                              //   bytes[0..3] = response CAN-ID (big-endian)
                              //   bytes[4..]  = UDS payload
} sched_event_t;

typedef struct scheduler_t scheduler_t;  // opaque

scheduler_t* scheduler_new(void);
void         scheduler_free(scheduler_t* s);

// Push an event onto the queue with its due_us pre-computed.
// Returns 0 on success, non-zero on OOM.
int scheduler_push(scheduler_t* s, const sched_event_t* event);

// Pop the next event whose due_us <= now. Returns 1 if one was popped
// (filled into *out), 0 if queue is empty or no event is yet due.
//
// The caller is responsible for eventually freeing out->data_bytes via
// scheduler_release_event() (which the wrapper exposes as PDUDestroyItem).
int scheduler_pop_due(scheduler_t* s, uint32_t h_mod, uint32_t h_cll,
                      uint64_t now_us, sched_event_t* out);

void scheduler_release_event(sched_event_t* event);

// Background delivery thread — fires registered callbacks for events
// as they become due. Safe to call without registering a callback;
// it'll just no-op.
//
// The wrapper's PDURegisterEventCallback hands its function pointer to
// scheduler_set_callback(); the delivery thread calls it on each event
// becoming due.
typedef void (__stdcall *fremsoft_event_cb)(uint32_t hMod, uint32_t hCLL, void* pData);

void scheduler_set_callback(scheduler_t* s, uint32_t h_mod, uint32_t h_cll,
                            fremsoft_event_cb cb);
void scheduler_start_delivery_thread(scheduler_t* s);
void scheduler_stop_delivery_thread(scheduler_t* s);

// Drop all events for a specific hCop (used by PDUCancelComPrimitive).
void scheduler_cancel_cop(scheduler_t* s, uint32_t h_cop);

#endif // FREMSOFT_SCHEDULER_H
