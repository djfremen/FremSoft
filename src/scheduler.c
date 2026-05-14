// FremSoft — event scheduler.
//
// Simple linked list with a single CRITICAL_SECTION. Push appends with the
// caller's pre-computed due_us; pop_due walks linearly and detaches the
// first event whose (due_us <= now) AND matches (h_mod, h_cll). Recordings
// have ~1-7 events per request; queue depth stays in low double digits even
// during a busy menu walk, so linear is fine.
//
// MVP: no async delivery thread yet. Tech2Win calls PDUGetEventItem in a
// poll loop; we serve from the queue synchronously. Adding the delivery
// thread is a follow-up once the basic flow proves out.

#include "scheduler.h"
#include "fremsoft.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

extern void shim_log(const char* fmt, ...);

typedef struct event_node {
    sched_event_t       event;
    struct event_node*  next;
} event_node_t;

struct scheduler_t {
    CRITICAL_SECTION  lock;
    event_node_t*     head;  // singly-linked, ordered by insertion
    fremsoft_event_cb cb;
    uint32_t          cb_h_mod;
    uint32_t          cb_h_cll;
    HANDLE            delivery_thread;
    volatile LONG     stop_flag;
};

scheduler_t* scheduler_new(void) {
    scheduler_t* s = (scheduler_t*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    InitializeCriticalSection(&s->lock);
    return s;
}

void scheduler_free(scheduler_t* s) {
    if (!s) return;
    scheduler_stop_delivery_thread(s);
    EnterCriticalSection(&s->lock);
    while (s->head) {
        event_node_t* n = s->head;
        s->head = n->next;
        free(n->event.data_bytes);
        free(n);
    }
    LeaveCriticalSection(&s->lock);
    DeleteCriticalSection(&s->lock);
    free(s);
}

int scheduler_push(scheduler_t* s, const sched_event_t* e) {
    if (!s || !e) return -1;
    event_node_t* n = (event_node_t*)calloc(1, sizeof(*n));
    if (!n) return -1;
    n->event = *e;  // shallow copy; data_bytes ownership transfers to us
    EnterCriticalSection(&s->lock);
    // Append at tail to preserve insertion order (which is also due-time
    // order for our use — we always push events with monotonically
    // increasing due_us within a single PDUStartComPrimitive).
    if (!s->head) {
        s->head = n;
    } else {
        event_node_t* cur = s->head;
        while (cur->next) cur = cur->next;
        cur->next = n;
    }
    LeaveCriticalSection(&s->lock);
    return 0;
}

int scheduler_pop_due(scheduler_t* s, uint32_t h_mod, uint32_t h_cll,
                      uint64_t now_us, sched_event_t* out)
{
    if (!s || !out) return 0;
    int popped = 0;
    EnterCriticalSection(&s->lock);
    event_node_t** prev = &s->head;
    event_node_t*  cur  = s->head;
    while (cur) {
        if (cur->event.h_mod == h_mod && cur->event.h_cll == h_cll
            && cur->event.due_us <= now_us)
        {
            *out = cur->event;
            *prev = cur->next;
            free(cur);
            popped = 1;
            break;
        }
        prev = &cur->next;
        cur  = cur->next;
    }
    LeaveCriticalSection(&s->lock);
    return popped;
}

void scheduler_release_event(sched_event_t* e) {
    if (!e) return;
    free(e->data_bytes);
    e->data_bytes = NULL;
}

void scheduler_set_callback(scheduler_t* s, uint32_t h_mod, uint32_t h_cll,
                            fremsoft_event_cb cb)
{
    if (!s) return;
    EnterCriticalSection(&s->lock);
    s->cb       = cb;
    s->cb_h_mod = h_mod;
    s->cb_h_cll = h_cll;
    LeaveCriticalSection(&s->lock);
}

static DWORD WINAPI delivery_thread_proc(LPVOID arg) {
    scheduler_t* s = (scheduler_t*)arg;
    while (!InterlockedCompareExchange(&s->stop_flag, 0, 0)) {
        EnterCriticalSection(&s->lock);
        fremsoft_event_cb cb = s->cb;
        uint32_t h_mod = s->cb_h_mod;
        uint32_t h_cll = s->cb_h_cll;
        LeaveCriticalSection(&s->lock);
        if (cb) {
            // Invoke the callback if any event for the registered (h_mod, h_cll)
            // is due. The callback wakes Tech2Win which will then call
            // PDUGetEventItem to pull the actual data.
            sched_event_t peek = {0};
            uint64_t now_us = GetTickCount64() * 1000ULL;
            EnterCriticalSection(&s->lock);
            event_node_t* cur = s->head;
            int has_due = 0;
            while (cur) {
                if (cur->event.h_mod == h_mod && cur->event.h_cll == h_cll
                    && cur->event.due_us <= now_us) { has_due = 1; break; }
                cur = cur->next;
            }
            LeaveCriticalSection(&s->lock);
            if (has_due) cb(h_mod, h_cll, NULL);
        }
        Sleep(5);  // 5 ms tick — Tech2Win's typical poll cadence is ~10 ms
    }
    return 0;
}

void scheduler_start_delivery_thread(scheduler_t* s) {
    if (!s || s->delivery_thread) return;
    InterlockedExchange(&s->stop_flag, 0);
    s->delivery_thread = CreateThread(NULL, 0, delivery_thread_proc, s, 0, NULL);
    if (!s->delivery_thread) {
        shim_log("WARN |scheduler|CreateThread failed (err=%lu)", GetLastError());
    }
}

void scheduler_stop_delivery_thread(scheduler_t* s) {
    if (!s || !s->delivery_thread) return;
    InterlockedExchange(&s->stop_flag, 1);
    WaitForSingleObject(s->delivery_thread, 200);
    CloseHandle(s->delivery_thread);
    s->delivery_thread = NULL;
}

void scheduler_cancel_cop(scheduler_t* s, uint32_t h_cop) {
    if (!s) return;
    EnterCriticalSection(&s->lock);
    event_node_t** prev = &s->head;
    event_node_t*  cur  = s->head;
    while (cur) {
        if (cur->event.h_cop == h_cop) {
            event_node_t* doomed = cur;
            *prev = cur->next;
            cur = cur->next;
            free(doomed->event.data_bytes);
            free(doomed);
        } else {
            prev = &cur->next;
            cur  = cur->next;
        }
    }
    LeaveCriticalSection(&s->lock);
}
