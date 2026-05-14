// FremSoft — playback / standalone implementation of the D-PDU API.
//
// Mode selected at attach via HKLM\SOFTWARE\OpenSAAB\Collector\Mode.
// In PLAYBACK mode the existing CSTech2Win shim's wrappers consult our
// fremsoft_get_mode() and route the data path here instead of forwarding
// to the real DLL.
//
// MVP scope (matches docs/standalone-export-map.md):
//   - DATA PATH (3): PDUStartComPrimitive, PDUGetEventItem,
//                    PDURegisterEventCallback — all implemented.
//   - PDUDestroyItem — frees scheduled-event payload.
//   - PDUCancelComPrimitive — drops queued events for the cancelled hCop.
//   - All other PDU* fns: stub returning STATUS_NOERROR (or fall-through
//     to the real DLL when in PLAYBACK + real DLL is loaded).
//
// What's NOT implemented yet (out of MVP):
//   - STANDALONE mode (where real DLL isn't loaded). PLAYBACK works first.
//   - Synthesis fallback for unknown requests beyond NRC + log.
//   - Periodic msg / filter enforcement.

#include "fremsoft.h"
#include "recording.h"
#include "scheduler.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void shim_log(const char* fmt, ...);
extern void unknown_log_init(void);
extern void unknown_log_record(uint32_t can_id, const uint8_t* uds, uint32_t uds_len);
extern void unknown_log_shutdown(void);

// ---- Module-local state -----------------------------------------------------

static fremsoft_mode_t g_mode = FREMSOFT_MODE_PASSTHROUGH;
static recording_t*    g_recording = NULL;
static scheduler_t*    g_scheduler = NULL;
static volatile LONG   g_next_h_cop = 0x10000;  // hCop generator
static FILE*           g_activity_log = NULL;
static CRITICAL_SECTION g_act_lock;
static int             g_act_init = 0;

// ---- Activity log (fremsoft_<ts>.log in same format as cstech2win shim) -----

static uint64_t wall_ms_now(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000;
}

static uint64_t since_attach_ms(void) {
    static uint64_t t0 = 0;
    uint64_t t = wall_ms_now();
    if (t0 == 0) t0 = t;
    return t - t0;
}

static void open_activity_log(void) {
    InitializeCriticalSection(&g_act_lock);
    char dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { dir[0] = '.'; dir[1] = 0; }
    char path[MAX_PATH];
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(path, MAX_PATH,
             "%s\\fremsoft_%04d%02d%02d-%02d%02d%02d.log",
             dir, st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    g_activity_log = fopen(path, "wb");
    if (g_activity_log) {
        setvbuf(g_activity_log, NULL, _IONBF, 0);
        fprintf(g_activity_log,
                "# fremsoft activity log -- same format as cstech2win shim\n"
                "# format: ms|tid|HEX|REQ-PDU|len=N|<chipsoft hdr><uds bytes>\n"
                "# format: ms|tid|HEX|RSP-UDS|len=N|<chipsoft hdr><uds bytes>\n"
                "# log file: %s\n", path);
        shim_log("INIT |fremsoft|activity log opened: %s", path);
    } else {
        shim_log("WARN |fremsoft|activity log fopen failed for %s", path);
    }
    g_act_init = 1;
}

void fremsoft_log_req_pdu(const uint8_t* bytes, uint32_t len) {
    if (!g_activity_log) return;
    EnterCriticalSection(&g_act_lock);
    fprintf(g_activity_log, "%llu|%lu|HEX  |REQ-PDU|len=%u|",
            (unsigned long long)since_attach_ms(),
            GetCurrentThreadId(), len);
    for (uint32_t i = 0; i < len; i++) {
        fprintf(g_activity_log, "%02X", bytes[i]);
        if (i + 1 < len) fputc(' ', g_activity_log);
    }
    fputc('\n', g_activity_log);
    LeaveCriticalSection(&g_act_lock);
}

void fremsoft_log_rsp_uds(const uint8_t* bytes, uint32_t len) {
    if (!g_activity_log) return;
    EnterCriticalSection(&g_act_lock);
    fprintf(g_activity_log, "%llu|%lu|HEX  |RSP-UDS|len=%u|",
            (unsigned long long)since_attach_ms(),
            GetCurrentThreadId(), len);
    for (uint32_t i = 0; i < len; i++) {
        fprintf(g_activity_log, "%02X", bytes[i]);
        if (i + 1 < len) fputc(' ', g_activity_log);
    }
    fputc('\n', g_activity_log);
    LeaveCriticalSection(&g_act_lock);
}

// ---- Lifecycle --------------------------------------------------------------

static int read_recording_path_from_registry(char* out, DWORD cap) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\OpenSAAB\\Collector", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD type = 0;
    DWORD sz = cap;
    LONG rc = RegQueryValueExA(key, "PlaybackRecording", NULL, &type,
                               (BYTE*)out, &sz);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_SZ && sz > 0;
}

int fremsoft_init(fremsoft_mode_t mode) {
    g_mode = mode;
    if (mode < FREMSOFT_MODE_PLAYBACK) return 0;  // passthrough/record: nothing to do here

    open_activity_log();
    unknown_log_init();

    char rec_path[MAX_PATH] = {0};
    if (!read_recording_path_from_registry(rec_path, MAX_PATH)) {
        // Fallback: try a default path next to the install dir.
        snprintf(rec_path, MAX_PATH,
                 "C:\\Program Files\\OpenSAAB\\Collector\\recording.json");
        shim_log("WARN |fremsoft|registry PlaybackRecording missing; trying default %s", rec_path);
    }

    g_recording = recording_load(rec_path);
    if (!g_recording) {
        shim_log("FATAL|fremsoft|recording_load failed for %s; falling back to PASSTHROUGH",
                 rec_path);
        g_mode = FREMSOFT_MODE_PASSTHROUGH;
        return -1;
    }
    recording_log_summary(g_recording);

    g_scheduler = scheduler_new();
    if (!g_scheduler) {
        shim_log("FATAL|fremsoft|scheduler_new failed");
        return -1;
    }
    scheduler_start_delivery_thread(g_scheduler);

    shim_log("INIT |fremsoft|mode=%d ready (recording=%u exchanges)",
             (int)mode, recording_size(g_recording));
    return 0;
}

void fremsoft_shutdown(void) {
    if (g_scheduler) { scheduler_free(g_scheduler); g_scheduler = NULL; }
    if (g_recording) { recording_free(g_recording); g_recording = NULL; }
    if (g_activity_log) {
        EnterCriticalSection(&g_act_lock);
        fclose(g_activity_log); g_activity_log = NULL;
        LeaveCriticalSection(&g_act_lock);
        DeleteCriticalSection(&g_act_lock);
        g_act_init = 0;
    }
    unknown_log_shutdown();
}

fremsoft_mode_t fremsoft_get_mode(void) { return g_mode; }

// ---- Data path: PDUStartComPrimitive -----------------------------------------
//
// Tech2Win sends `pCoPData` for ISO15765 in this layout:
//   bytes [0..3]  CAN-ID big-endian (or extended)
//   bytes [4..]   UDS payload (PCI byte already stripped or not, depending
//                 on Tech2 codepath; for Check Codes the bytes are raw UDS
//                 like 'A9 81 12').
//
// We extract (can_id, uds_bytes) and look up in the recording. If found,
// schedule one event per recorded RX with due_us = now + cumulative_delay.
// If not found, log + schedule an NRC.

T_PDU_ERROR fremsoft_PDUStartComPrimitive(
    UNUM32 hMod, UNUM32 hCLL, UNUM32 CoPType,
    UNUM32 CoPDataSize, UNUM8* pCoPData,
    void* pCoPCtrlData, void* pCoPTag, UNUM32* phCoP)
{
    (void)pCoPCtrlData;
    UNUM32 h_cop = (UNUM32)InterlockedIncrement(&g_next_h_cop);
    if (phCoP) *phCoP = h_cop;

    // Only handle SENDRECV (0x8010) for the MVP. Other CoPTypes
    // (STARTCOMM 0x8001, STOPCOMM 0x8002) just succeed silently — Tech2Win
    // uses them to bring the channel up; nothing for FremSoft to do.
    if (CoPType != 0x8010) {
        shim_log("FREM |StartCoP|hMod=0x%X hCLL=0x%X CoPType=0x%X (non-SENDRECV) hCop=0x%X — ack",
                 hMod, hCLL, CoPType, h_cop);
        return PDU_STATUS_NOERROR;
    }

    if (CoPDataSize < 5 || !pCoPData) {
        shim_log("FREM |StartCoP|short pCoPData (%u bytes) — NRC", CoPDataSize);
        return PDU_STATUS_NOERROR;
    }

    // Extract CAN-ID from bytes 0..3 (big-endian uint32 — but for 11-bit
    // IDs the high two bytes are zero).
    uint32_t can_id =
          ((uint32_t)pCoPData[0] << 24)
        | ((uint32_t)pCoPData[1] << 16)
        | ((uint32_t)pCoPData[2] <<  8)
        | ((uint32_t)pCoPData[3] <<  0);
    const uint8_t* uds = pCoPData + 4;
    uint32_t       uds_len = CoPDataSize - 4;

    fremsoft_log_req_pdu(pCoPData, CoPDataSize);

    const recorded_exchange_t* match =
        recording_lookup(g_recording, can_id, uds, uds_len);

    uint64_t now_us = GetTickCount64() * 1000ULL;

    if (match) {
        // Schedule each recorded RX with cumulative delay.
        uint64_t cumulative_us = 0;
        for (uint32_t i = 0; i < match->num_responses; i++) {
            cumulative_us += (uint64_t)match->responses[i].delay_ms * 1000ULL;
            sched_event_t e = {0};
            e.due_us     = now_us + cumulative_us;
            e.h_mod      = hMod;
            e.h_cll      = hCLL;
            e.h_cop      = h_cop;
            e.cop_tag    = pCoPTag;
            e.item_type  = 0x1300;  // PDU_IT_RESULT
            // Compose PDU response payload: 4-byte BE CAN-ID prefix + UDS bytes
            uint32_t out_can = match->responses[i].can_id;
            uint32_t total = 4 + match->responses[i].uds_len;
            e.num_data = total;
            e.data_bytes = (uint8_t*)malloc(total);
            if (!e.data_bytes) {
                shim_log("FATAL|StartCoP|OOM scheduling response");
                continue;
            }
            e.data_bytes[0] = (uint8_t)((out_can >> 24) & 0xFF);
            e.data_bytes[1] = (uint8_t)((out_can >> 16) & 0xFF);
            e.data_bytes[2] = (uint8_t)((out_can >>  8) & 0xFF);
            e.data_bytes[3] = (uint8_t)((out_can      ) & 0xFF);
            memcpy(e.data_bytes + 4, match->responses[i].uds_bytes,
                   match->responses[i].uds_len);
            scheduler_push(g_scheduler, &e);
            fremsoft_log_rsp_uds(e.data_bytes, total);
        }
        shim_log("FREM |StartCoP|hMod=0x%X hCLL=0x%X CAN=0x%04X uds_len=%u hCop=0x%X "
                 "MATCH (%u responses scheduled)",
                 hMod, hCLL, can_id, uds_len, h_cop, match->num_responses);
    } else {
        // Unknown — log it, schedule a 7F <SID> 11 NRC at +50ms.
        unknown_log_record(can_id, uds, uds_len);
        sched_event_t e = {0};
        e.due_us    = now_us + 50000;
        e.h_mod     = hMod;
        e.h_cll     = hCLL;
        e.h_cop     = h_cop;
        e.cop_tag   = pCoPTag;
        e.item_type = 0x1300;
        e.num_data  = 4 + 3;
        e.data_bytes = (uint8_t*)malloc(7);
        if (e.data_bytes) {
            // Reply CAN-ID: SAAB convention is request_id + 0x400. Best
            // guess for unknown — Tech2Win likely doesn't care about the
            // exact response ID for an NRC.
            uint32_t reply = can_id + 0x400;
            e.data_bytes[0] = (uint8_t)((reply >> 24) & 0xFF);
            e.data_bytes[1] = (uint8_t)((reply >> 16) & 0xFF);
            e.data_bytes[2] = (uint8_t)((reply >>  8) & 0xFF);
            e.data_bytes[3] = (uint8_t)((reply      ) & 0xFF);
            e.data_bytes[4] = 0x7F;
            e.data_bytes[5] = uds[0];   // echo the requested SID
            e.data_bytes[6] = 0x11;     // ServiceNotSupported
            scheduler_push(g_scheduler, &e);
            fremsoft_log_rsp_uds(e.data_bytes, 7);
        }
        shim_log("FREM |StartCoP|hMod=0x%X hCLL=0x%X CAN=0x%04X uds_len=%u hCop=0x%X "
                 "UNKNOWN (NRC scheduled)", hMod, hCLL, can_id, uds_len, h_cop);
    }
    return PDU_STATUS_NOERROR;
}

// ---- Data path: PDUGetEventItem ----------------------------------------------
//
// Tech2Win polls this to drain replies. We allocate a PDU_EVENT_ITEM +
// PDU_RESULT_DATA + pDataBytes on the heap and hand back its address;
// PDUDestroyItem (or our shutdown) frees it.

typedef struct {
    UNUM32  ItemType;
    UNUM32  hCop;
    void*   pCoPTag;
    UNUM32  Timestamp;
    void*   pData;
} PDU_EVENT_ITEM_MIN;
typedef struct {
    UNUM32  RxFlag_NumFlagBytes;
    UNUM8*  RxFlag_pFlagData;
    UNUM32  UniqueRespIdentifier;
    UNUM32  AcceptanceId;
    UNUM32  TimestampFlags_NumBytes;
    UNUM8*  TimestampFlags_pFlagData;
    UNUM32  TxMsgDoneTimestamp;
    UNUM32  StartMsgTimestamp;
    void*   pExtraInfo;
    UNUM32  NumDataBytes;
    UNUM8*  pDataBytes;
} PDU_RESULT_DATA_MIN;

T_PDU_ERROR fremsoft_PDUGetEventItem(UNUM32 hMod, UNUM32 hCLL, void** pEventItem)
{
    if (!pEventItem) return PDU_ERR_INVALID_PARAMETERS;
    *pEventItem = NULL;

    sched_event_t e = {0};
    uint64_t now_us = GetTickCount64() * 1000ULL;
    if (!scheduler_pop_due(g_scheduler, hMod, hCLL, now_us, &e)) {
        return PDU_ERR_EVENT_QUEUE_EMPTY;
    }

    // Allocate event_item + result_data + data_bytes contiguously so a
    // single free in PDUDestroyItem cleans everything up.
    size_t total = sizeof(PDU_EVENT_ITEM_MIN) + sizeof(PDU_RESULT_DATA_MIN)
                 + e.num_data;
    uint8_t* blob = (uint8_t*)calloc(1, total);
    if (!blob) {
        free(e.data_bytes);
        return PDU_ERR_FCT_FAILED;
    }
    PDU_EVENT_ITEM_MIN*  ev = (PDU_EVENT_ITEM_MIN*)blob;
    PDU_RESULT_DATA_MIN* rd = (PDU_RESULT_DATA_MIN*)(blob + sizeof(*ev));
    uint8_t*             db = blob + sizeof(*ev) + sizeof(*rd);

    ev->ItemType  = e.item_type;
    ev->hCop      = e.h_cop;
    ev->pCoPTag   = e.cop_tag;
    ev->Timestamp = (UNUM32)(now_us & 0xFFFFFFFF);
    ev->pData     = rd;

    rd->NumDataBytes = e.num_data;
    rd->pDataBytes   = db;
    memcpy(db, e.data_bytes, e.num_data);
    free(e.data_bytes);

    *pEventItem = blob;
    return PDU_STATUS_NOERROR;
}

T_PDU_ERROR fremsoft_PDUDestroyItem(void* pItem) {
    if (!pItem) return PDU_STATUS_NOERROR;
    free(pItem);
    return PDU_STATUS_NOERROR;
}

// ---- Data path: PDURegisterEventCallback -------------------------------------

T_PDU_ERROR fremsoft_PDURegisterEventCallback(
    UNUM32 hMod, UNUM32 hCLL,
    void (__stdcall *cb)(UNUM32, UNUM32, void*))
{
    if (g_scheduler) {
        scheduler_set_callback(g_scheduler, hMod, hCLL,
                               (fremsoft_event_cb)cb);
        shim_log("FREM |RegisterCb|hMod=0x%X hCLL=0x%X cb=%p", hMod, hCLL, (void*)cb);
    }
    return PDU_STATUS_NOERROR;
}

// ---- TRIVIAL / STATEFUL stubs ------------------------------------------------
// MVP: return success. In PLAYBACK mode the wrappers fall through to the
// real DLL for these so its bookkeeping stays sane; we only get called for
// these in STANDALONE mode (which isn't built yet — tomorrow's work).

T_PDU_ERROR fremsoft_PDUConstruct(CHAR8* OptionStr, void* pAPITag) {
    (void)OptionStr; (void)pAPITag; return PDU_STATUS_NOERROR;
}
T_PDU_ERROR fremsoft_PDUDestruct(void) { return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUConnect(UNUM32 hMod) { (void)hMod; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUDisconnect(UNUM32 hMod) { (void)hMod; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUCreateComLogicalLink(
    UNUM32 hMod, UNUM32 ResId, UNUM32 ResData, void* pResIdItem,
    UNUM32* phCLL, UNUM32 flags)
{
    (void)hMod; (void)ResId; (void)ResData; (void)pResIdItem; (void)flags;
    if (phCLL) *phCLL = 0x1234;
    return PDU_STATUS_NOERROR;
}
T_PDU_ERROR fremsoft_PDUDestroyComLogicalLink(UNUM32 hMod, UNUM32 hCLL) {
    (void)hMod; (void)hCLL; return PDU_STATUS_NOERROR;
}
T_PDU_ERROR fremsoft_PDUSetComParam(UNUM32 hMod, UNUM32 hCLL, void* pParam) {
    (void)hMod; (void)hCLL; (void)pParam; return PDU_STATUS_NOERROR;
}
T_PDU_ERROR fremsoft_PDUGetComParam(UNUM32 hMod, UNUM32 hCLL, UNUM32 ParamId, void** pParam) {
    (void)hMod; (void)hCLL; (void)ParamId; if (pParam) *pParam = NULL;
    return PDU_STATUS_NOERROR;
}
T_PDU_ERROR fremsoft_PDUIoCtl(UNUM32 hMod, UNUM32 hCLL, UNUM32 IoCtlId,
                              void* pIn, void** pOut)
{
    (void)hMod; (void)hCLL; (void)IoCtlId; (void)pIn; if (pOut) *pOut = NULL;
    return PDU_STATUS_NOERROR;
}

// Discovery stubs (not used in PLAYBACK mode — real DLL handles them).
T_PDU_ERROR fremsoft_PDUGetVersion(UNUM32 hMod, void* pV) { (void)hMod; (void)pV; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetModuleIds(void* pL) { (void)pL; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetResourceIds(UNUM32 hMod, void* pL) { (void)hMod; (void)pL; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetResourceStatus(void* pL) { (void)pL; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetConflictingResources(UNUM32 a, UNUM32 b, void* c, void** d) { (void)a;(void)b;(void)c;if(d)*d=NULL; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetObjectId(UNUM32 t, CHAR8* n, UNUM32* p) { (void)t;(void)n; if(p)*p=0; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetStatus(UNUM32 a, UNUM32 b, UNUM32 c, UNUM32* d, UNUM32* e, UNUM32* f) { (void)a;(void)b;(void)c; if(d)*d=0; if(e)*e=0; if(f)*f=0; return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetTimestamp(UNUM32 hMod, UNUM32* pT) { (void)hMod; if(pT)*pT=(UNUM32)GetTickCount(); return PDU_STATUS_NOERROR; }
T_PDU_ERROR fremsoft_PDUGetLastError(UNUM32 a, UNUM32 b, UNUM32* c, UNUM32* d, UNUM32* e, UNUM32* f) { (void)a;(void)b; if(c)*c=0;if(d)*d=0;if(e)*e=0;if(f)*f=0; return PDU_STATUS_NOERROR; }
