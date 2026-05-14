// CSTech2Win shim — instrumented exports.
//
// These six functions get hand-written __stdcall wrappers that log args and
// output buffers around a passthrough call to the real DLL.
//
// Function signatures are from ISO 22900-2 (D-PDU API). We only declare the
// minimum struct layout needed to extract the bytes we care about — full
// struct definitions live in ISO22900-2 if/when we need them.
//
// The most valuable byte capture is in PDUStartComPrimitive: pCoPData carries
// the raw request bytes Tech2Win wants sent to the ECU (e.g. $27 $01 for
// SecurityAccess requestSeed). That's where the SAS/IMMO custom $Level will
// surface, if it exists.
//
// Phase 1 (this file): log args + request buffer. Sufficient to identify which
// $Level Tech2Win invokes and prove the shim wiring works.
//
// Phase 2 (TODO): deep-decode PDUGetEventItem's PDU_RESULT_DATA->pDataBytes
// to capture seed/key response bytes.

#include "shim.h"
#include "wrappers.h"
#include "../../../src/fremsoft.h"

// ---- typedefs for the 7 instrumented exports --------------------------------

// T_PDU_PARAM (ISO 22900-2 §10.7) — passed to PDUSetComParam to configure
// per-CLL communication parameters (ProtocolID, baud rate, J1962 pin map, etc).
// ComParamDataType selects the layout of *pComParamData:
//   0x8001 PDU_PT_UNUM32      — 4-byte unsigned int (e.g. baud rate)
//   0x8002 PDU_PT_SNUM32      — 4-byte signed int
//   0x8003 PDU_PT_BYTEFIELD   — { UNUM32 NumBytes; UNUM8* pBytes; } (e.g. PINS map)
//   0x8004 PDU_PT_STRUCTFIELD — implementation-specific struct
//   0x8005 PDU_PT_LONGFIELD   — { UNUM32 NumLongs; UNUM32* pLongs; }
typedef struct {
    UNUM32 ComParamId;        // offset 0
    UNUM32 ComParamDataType;  // offset 4
    UNUM32 ComParamClass;     // offset 8
    void*  pComParamData;     // offset 12
} T_PDU_PARAM_MIN;            // 16 bytes

typedef T_PDU_ERROR (PDUAPI *fn_PDUConstruct)(CHAR8* OptionStr, void* pAPITag);
typedef T_PDU_ERROR (PDUAPI *fn_PDUDestruct)(void);
typedef T_PDU_ERROR (PDUAPI *fn_PDUIoCtl)(UNUM32 hMod, UNUM32 hCLL, UNUM32 IoCtlCommandId,
                                          void* pInputData, void** pOutputData);
typedef T_PDU_ERROR (PDUAPI *fn_PDUSetComParam)(UNUM32 hMod, UNUM32 hCLL,
                                                T_PDU_PARAM_MIN* pParam);
typedef T_PDU_ERROR (PDUAPI *fn_PDUStartComPrimitive)(UNUM32 hMod, UNUM32 hCLL,
                                                     UNUM32 CoPType, UNUM32 CoPDataSize,
                                                     UNUM8* pCoPData, void* pCopCtrlData,
                                                     void* pCoPTag, UNUM32* phCoP);
typedef T_PDU_ERROR (PDUAPI *fn_PDUGetEventItem)(UNUM32 hMod, UNUM32 hCLL, void** pEventItem);
typedef T_PDU_ERROR (PDUAPI *fn_PDURegisterEventCallback)(UNUM32 hMod, UNUM32 hCLL,
                                                         void (PDUAPI *cb)(UNUM32, UNUM32, void*));

FARPROC g_real_PDUConstruct = NULL;
FARPROC g_real_PDUDestruct = NULL;
FARPROC g_real_PDUIoCtl = NULL;
FARPROC g_real_PDUSetComParam = NULL;
FARPROC g_real_PDUStartComPrimitive = NULL;
FARPROC g_real_PDUGetEventItem = NULL;
FARPROC g_real_PDURegisterEventCallback = NULL;

void resolve_instrumented_exports(HMODULE hReal) {
    g_real_PDUConstruct           = GetProcAddress(hReal, "PDUConstruct");
    g_real_PDUDestruct            = GetProcAddress(hReal, "PDUDestruct");
    g_real_PDUIoCtl               = GetProcAddress(hReal, "PDUIoCtl");
    g_real_PDUSetComParam         = GetProcAddress(hReal, "PDUSetComParam");
    g_real_PDUStartComPrimitive   = GetProcAddress(hReal, "PDUStartComPrimitive");
    g_real_PDUGetEventItem        = GetProcAddress(hReal, "PDUGetEventItem");
    g_real_PDURegisterEventCallback = GetProcAddress(hReal, "PDURegisterEventCallback");
}

// ---- Wrappers ---------------------------------------------------------------

T_PDU_ERROR PDUAPI PDUConstruct(CHAR8* OptionStr, void* pAPITag) {
    shim_log("CALL |PDUConstruct|opt='%s' tag=%p", OptionStr ? OptionStr : "(null)", pAPITag);
    T_PDU_ERROR r = ((fn_PDUConstruct)g_real_PDUConstruct)(OptionStr, pAPITag);
    shim_log("RET  |PDUConstruct|err=%u", r);
    return r;
}

T_PDU_ERROR PDUAPI PDUDestruct(void) {
    shim_log("CALL |PDUDestruct|");
    T_PDU_ERROR r = ((fn_PDUDestruct)g_real_PDUDestruct)();
    shim_log("RET  |PDUDestruct|err=%u", r);
    return r;
}

T_PDU_ERROR PDUAPI PDUIoCtl(UNUM32 hMod, UNUM32 hCLL, UNUM32 IoCtlCommandId,
                            void* pInputData, void** pOutputData) {
    shim_log("CALL |PDUIoCtl|hMod=0x%08X hCLL=0x%08X cmd=0x%08X pIn=%p",
             hMod, hCLL, IoCtlCommandId, pInputData);
    // Per ISO 22900-2 §9.4 most IoCtls take a T_PDU_DATA_ITEM* whose first
    // 12 bytes are {ItemType, NumDataBytes, pDataBytes}. Some take NULL
    // (PDU_IOCTL_RESET, the 0x000C000x family seen on this firmware). Dump
    // the first 32 bytes blindly under SEH so per-cmd struct knowledge is
    // not required — analysis tooling can decode after the fact.
    if (pInputData) {
        __try {
            shim_log_hex("IOCTL-IN", pInputData, 32);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            shim_log("ERR  |PDUIoCtl|pInputData fault pIn=%p", pInputData);
        }
    }
    T_PDU_ERROR r = ((fn_PDUIoCtl)g_real_PDUIoCtl)(hMod, hCLL, IoCtlCommandId,
                                                    pInputData, pOutputData);
    shim_log("RET  |PDUIoCtl|err=%u", r);
    return r;
}

// PDUSetComParam (ISO 22900-2 §6.4.5) — host configures per-CLL communication
// parameters: protocol ID, baud rate, J1962 pin map, timing windows, etc.
// This is the D-PDU equivalent of J2534's PassThruIoctl SET_CONFIG and is the
// missing piece for reproducing Tech2Win's init recipe in the Android client.
//
// We log the {ComParamId, DataType, Class} triple verbatim, plus 32 bytes of
// pComParamData under SEH. For PT_UNUM32/PT_SNUM32 (DataType 0x8001/0x8002)
// the first 4 bytes are the value. For PT_BYTEFIELD (0x8003) the first 4
// bytes are NumBytes followed by a pointer — analysis tooling can chase that
// pointer offline.
T_PDU_ERROR PDUAPI PDUSetComParam(UNUM32 hMod, UNUM32 hCLL,
                                  T_PDU_PARAM_MIN* pParam) {
    shim_log("CALL |PDUSetComParam|hMod=0x%08X hCLL=0x%08X pParam=%p",
             hMod, hCLL, pParam);
    if (pParam) {
        __try {
            shim_log("PARAM|ComParamId=0x%08X DataType=0x%08X Class=0x%08X pData=%p",
                     pParam->ComParamId, pParam->ComParamDataType,
                     pParam->ComParamClass, pParam->pComParamData);
            if (pParam->pComParamData) {
                shim_log_hex("PARAM-DATA", pParam->pComParamData, 32);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            shim_log("ERR  |PDUSetComParam|param decode fault pParam=%p", pParam);
        }
    }
    T_PDU_ERROR r = ((fn_PDUSetComParam)g_real_PDUSetComParam)(hMod, hCLL, pParam);
    shim_log("RET  |PDUSetComParam|err=%u", r);
    return r;
}

T_PDU_ERROR PDUAPI PDUStartComPrimitive(UNUM32 hMod, UNUM32 hCLL,
                                        UNUM32 CoPType, UNUM32 CoPDataSize,
                                        UNUM8* pCoPData, void* pCopCtrlData,
                                        void* pCoPTag, UNUM32* phCoP) {
    // CoPType 0x8001=PDU_COPT_STARTCOMM, 0x8002=PDU_COPT_STOPCOMM,
    //         0x8010=PDU_COPT_SENDRECV (this is what carries diag requests),
    //         0x8011=PDU_COPT_DELAY, 0x8020=PDU_COPT_UPDATEPARAM
    shim_log("CALL |PDUStartComPrimitive|hMod=0x%08X hCLL=0x%08X CoPType=0x%04X size=%u",
             hMod, hCLL, CoPType, CoPDataSize);
    if (pCoPData && CoPDataSize > 0 && CoPDataSize <= 4096) {
        shim_log_hex("REQ-PDU", pCoPData, CoPDataSize);
    }
    // FremSoft dispatch: in PLAYBACK / STANDALONE we serve replies from a
    // recording instead of forwarding to the real DLL. The wrapper still
    // logs above for cross-checking against the real-bus shim format.
    if (fremsoft_get_mode() >= FREMSOFT_MODE_PLAYBACK) {
        T_PDU_ERROR r = fremsoft_PDUStartComPrimitive(
            hMod, hCLL, CoPType, CoPDataSize, pCoPData, pCopCtrlData, pCoPTag, phCoP);
        shim_log("RET  |PDUStartComPrimitive[FREM]|err=%u hCoP=0x%08X",
                 r, (phCoP && r == 0) ? *phCoP : 0);
        return r;
    }
    T_PDU_ERROR r = ((fn_PDUStartComPrimitive)g_real_PDUStartComPrimitive)(
        hMod, hCLL, CoPType, CoPDataSize, pCoPData, pCopCtrlData, pCoPTag, phCoP);
    shim_log("RET  |PDUStartComPrimitive|err=%u hCoP=0x%08X",
             r, (phCoP && r == 0) ? *phCoP : 0);
    return r;
}

// Canonical ISO 22900-2 D-PDU API struct layouts.
// Reference: pdu_api.h from github.com/JohnJocke/dpdu-passthru (GPL-3.0 project,
// header is the standardized D-PDU API spec; struct ABI is canonical).
//
// Earlier versions of this file had an incorrect PDU_EVENT_ITEM with a phantom
// "EventType" field at offset 4, leading us to mis-decode several runs. The
// values we logged as "EventType=0xF3 / 0x114" were actually the hCop field.
// pData is at offset 16 of the event item and points to PDU_RESULT_DATA, whose
// pDataBytes (the actual UDS bytes, including 4-byte CAN-ID prefix for
// ISO15765) lives at offset 40 of THAT struct, with NumDataBytes at offset 36.

typedef struct {
    UNUM32  ItemType;      // offset 0  : 0x1300=PDU_IT_RESULT, 0x1301=PDU_IT_STATUS,
                           //             0x1304=PDU_IT_ERROR, 0x1305=PDU_IT_INFO
    UNUM32  hCop;          // offset 4  : ComPrimitive handle from PDUStartComPrimitive
    void*   pCoPTag;       // offset 8  : caller-supplied tag (per-CLL constant for Tech2)
    UNUM32  Timestamp;     // offset 12 : microseconds
    void*   pData;         // offset 16 : for RESULT items: PDU_RESULT_DATA*
} PDU_EVENT_ITEM_MIN;      // 20 bytes total

// PDU_FLAG_DATA appears inline in PDU_RESULT_DATA twice (RxFlag + TimestampFlags).
// 8 bytes per: { UNUM32 NumFlagBytes; UNUM8* pFlagData; }
typedef struct {
    UNUM32  RxFlag_NumFlagBytes;       // offset 0
    UNUM8*  RxFlag_pFlagData;          // offset 4
    UNUM32  UniqueRespIdentifier;      // offset 8
    UNUM32  AcceptanceId;              // offset 12
    UNUM32  TimestampFlags_NumBytes;   // offset 16
    UNUM8*  TimestampFlags_pFlagData;  // offset 20
    UNUM32  TxMsgDoneTimestamp;        // offset 24
    UNUM32  StartMsgTimestamp;         // offset 28
    void*   pExtraInfo;                // offset 32
    UNUM32  NumDataBytes;              // offset 36 — length of pDataBytes
    UNUM8*  pDataBytes;                // offset 40 — actual UDS bytes (CAN-ID + payload)
} PDU_RESULT_DATA_MIN;     // 44 bytes total

T_PDU_ERROR PDUAPI PDUGetEventItem(UNUM32 hMod, UNUM32 hCLL, void** pEventItem) {
    if (fremsoft_get_mode() >= FREMSOFT_MODE_PLAYBACK) {
        return fremsoft_PDUGetEventItem(hMod, hCLL, pEventItem);
    }
    T_PDU_ERROR r = ((fn_PDUGetEventItem)g_real_PDUGetEventItem)(hMod, hCLL, pEventItem);
    if (r == 0 && pEventItem && *pEventItem) {
        PDU_EVENT_ITEM_MIN* ev = (PDU_EVENT_ITEM_MIN*)*pEventItem;
        shim_log("EVT  |PDUGetEventItem|hMod=0x%08X hCLL=0x%08X "
                 "ItemType=0x%X hCop=0x%X pCoPTag=%p ts=%u pData=%p",
                 hMod, hCLL, ev->ItemType, ev->hCop, ev->pCoPTag,
                 ev->Timestamp, ev->pData);

        // PDU_IT_RESULT items carry the response in pData -> PDU_RESULT_DATA.
        // pDataBytes (offset 40 of PDU_RESULT_DATA) points to the UDS frame:
        // for ISO 15765 the first 4 bytes are CAN ID (BE), then UDS payload.
        // NumDataBytes (offset 36) is the length.
        if (ev->ItemType == 0x1300 /* PDU_IT_RESULT */ && ev->pData) {
            __try {
                PDU_RESULT_DATA_MIN* rd = (PDU_RESULT_DATA_MIN*)ev->pData;
                UNUM32 n = rd->NumDataBytes;
                UNUM8* p = rd->pDataBytes;
                shim_log("RSP  |hCop=0x%X RxFlagBytes=%u UniqueRespId=0x%X "
                         "AcceptanceId=0x%X TxDone=%u StartMsg=%u NumDataBytes=%u pDataBytes=%p",
                         ev->hCop,
                         rd->RxFlag_NumFlagBytes,
                         rd->UniqueRespIdentifier,
                         rd->AcceptanceId,
                         rd->TxMsgDoneTimestamp,
                         rd->StartMsgTimestamp,
                         n, p);
                if (p && n > 0 && n <= 4096) {
                    shim_log_hex("RSP-UDS", p, n);
                }
                // Also dump RxFlag bytes if present — useful for diagnosing
                // why a $27 response might be flagged differently than $1A.
                if (rd->RxFlag_pFlagData && rd->RxFlag_NumFlagBytes > 0
                                         && rd->RxFlag_NumFlagBytes <= 64) {
                    shim_log_hex("RSP-RxFlag", rd->RxFlag_pFlagData,
                                 rd->RxFlag_NumFlagBytes);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                shim_log("ERR  |PDUGetEventItem|RSP decode fault pData=%p",
                         ev->pData);
            }
        }
        // PDU_IT_ERROR items carry diagnostic error info — log it so we know
        // when Tech2 sees a stack-level problem.
        else if (ev->ItemType == 0x1304 /* PDU_IT_ERROR */ && ev->pData) {
            __try {
                UNUM32 errCodeId = ((UNUM32*)ev->pData)[0];
                UNUM32 extraInfo = ((UNUM32*)ev->pData)[1];
                shim_log("ERR-EVT|hCop=0x%X ErrorCodeId=0x%X ExtraErrorInfo=0x%X",
                         ev->hCop, errCodeId, extraInfo);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                shim_log("ERR  |PDUGetEventItem|err-evt decode fault");
            }
        }
    }
    return r;
}

// ---- Callback trampolines ---------------------------------------------------
//
// 2026-05-07 finding (HANDOFF.md option 3): SecurityAccess responses ($27 $0B
// seed/key) do NOT surface in PDUGetEventItem result events. They arrive via
// the callback Tech2 registers through PDURegisterEventCallback. The callback
// signature is __stdcall void (UNUM32 hMod, UNUM32 hCLL, void* pData).
//
// To intercept, we substitute the caller's cb with one of N pre-generated
// trampolines. Each trampoline knows its slot index at compile time and uses
// it to look up the real cb in g_cb_slots[]. The dispatcher logs args + the
// pData buffer (which carries the response bytes), then forwards to the real
// cb. Tech2 only registered 3 callbacks in the run-5 capture, so 16 slots is
// generous.

typedef void (PDUAPI *cb_t)(UNUM32 hMod, UNUM32 hCLL, void* pData);

#define MAX_CB_SLOTS 16

static struct {
    cb_t   real_cb;
    UNUM32 hMod;
    UNUM32 hCLL;
    int    used;
} g_cb_slots[MAX_CB_SLOTS];
static int g_cb_count = 0;
static CRITICAL_SECTION g_cb_lock;
static int g_cb_lock_initialized = 0;

static void common_cb_dispatch(int slot, UNUM32 hMod, UNUM32 hCLL, void* pData) {
    shim_log("CB   |fired|slot=%d hMod=0x%08X hCLL=0x%08X pData=%p",
             slot, hMod, hCLL, pData);
    // Dump the pData buffer — this is where the seed bytes live for $27 $0B.
    // Size unknown; 64 is generous enough to contain any UDS response plus
    // whatever metadata Chipsoft prepends/appends.
    if (pData) {
        __try {
            shim_log_hex("CB-DATA", pData, 64);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            shim_log("ERR  |CB|data fault pData=%p", pData);
        }
    }
    cb_t real = g_cb_slots[slot].real_cb;
    if (real) {
        real(hMod, hCLL, pData);
    }
}

#define DEFINE_CB_TRAMP(N) \
    static void PDUAPI cb_tramp_##N(UNUM32 hMod, UNUM32 hCLL, void* pData) { \
        common_cb_dispatch(N, hMod, hCLL, pData); \
    }

DEFINE_CB_TRAMP(0)  DEFINE_CB_TRAMP(1)  DEFINE_CB_TRAMP(2)  DEFINE_CB_TRAMP(3)
DEFINE_CB_TRAMP(4)  DEFINE_CB_TRAMP(5)  DEFINE_CB_TRAMP(6)  DEFINE_CB_TRAMP(7)
DEFINE_CB_TRAMP(8)  DEFINE_CB_TRAMP(9)  DEFINE_CB_TRAMP(10) DEFINE_CB_TRAMP(11)
DEFINE_CB_TRAMP(12) DEFINE_CB_TRAMP(13) DEFINE_CB_TRAMP(14) DEFINE_CB_TRAMP(15)

static cb_t g_tramps[MAX_CB_SLOTS] = {
    cb_tramp_0,  cb_tramp_1,  cb_tramp_2,  cb_tramp_3,
    cb_tramp_4,  cb_tramp_5,  cb_tramp_6,  cb_tramp_7,
    cb_tramp_8,  cb_tramp_9,  cb_tramp_10, cb_tramp_11,
    cb_tramp_12, cb_tramp_13, cb_tramp_14, cb_tramp_15,
};

T_PDU_ERROR PDUAPI PDURegisterEventCallback(UNUM32 hMod, UNUM32 hCLL,
                                            void (PDUAPI *cb)(UNUM32, UNUM32, void*)) {
    shim_log("CALL |PDURegisterEventCallback|hMod=0x%08X hCLL=0x%08X cb=%p",
             hMod, hCLL, (void*)cb);

    if (fremsoft_get_mode() >= FREMSOFT_MODE_PLAYBACK) {
        return fremsoft_PDURegisterEventCallback(hMod, hCLL, cb);
    }

    // Lazy-init the lock on first call (DllMain isn't always a safe place
    // to do this; this gets us the same effect with fewer constraints).
    if (!g_cb_lock_initialized) {
        InitializeCriticalSection(&g_cb_lock);
        g_cb_lock_initialized = 1;
    }

    cb_t passed_cb = (cb_t)cb;
    int slot = -1;
    EnterCriticalSection(&g_cb_lock);
    if (g_cb_count < MAX_CB_SLOTS && cb != NULL) {
        slot = g_cb_count++;
        g_cb_slots[slot].real_cb = passed_cb;
        g_cb_slots[slot].hMod = hMod;
        g_cb_slots[slot].hCLL = hCLL;
        g_cb_slots[slot].used = 1;
    }
    LeaveCriticalSection(&g_cb_lock);

    if (slot >= 0) {
        // 2026-05-07: trampoline substitution DISABLED. Our 3-arg trampoline
        // declaration mismatched Chipsoft's actual __stdcall callback signature
        // (per pdu_api.h:723 it's 5 args: T_PDU_EVT_DATA, UNUM32 hMod,
        // UNUM32 hCLL, void* pCllTag, void* pAPITag — total 20 bytes pushed by
        // caller, but our trampoline pops 12, drifting Tech2's stack 8 bytes
        // per call until crash). Slot tracking + registration logging stays so
        // we can re-enable once the trampoline signature is corrected. Until
        // then, the struct-layout-fixed PDUGetEventItem queue path is the
        // primary mechanism. See HANDOFF.md.
        // passed_cb = g_tramps[slot];     // <-- intentionally skipped
        shim_log("CB   |register|slot=%d real=%p tramp=DISABLED",
                 slot, (void*)cb);
    } else if (cb != NULL) {
        shim_log("WARN |register|no free slot, passing real cb through (cb=%p)", (void*)cb);
    }

    T_PDU_ERROR r = ((fn_PDURegisterEventCallback)g_real_PDURegisterEventCallback)(
        hMod, hCLL, (void (PDUAPI*)(UNUM32, UNUM32, void*))passed_cb);
    shim_log("RET  |PDURegisterEventCallback|err=%u", r);
    return r;
}
