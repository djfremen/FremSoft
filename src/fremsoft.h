// FremSoft — internal API for the playback / standalone implementation.
//
// Linked into the existing CSTech2Win shim. When the shim's mode flag is
// "playback" or "standalone", the wrapper functions in shim/cstech2win/
// route to fremsoft_* instead of forwarding to the real DLL.
//
// "playback":   real DLL is loaded and stays available as a fallback for
//               IoCtls / discovery; the data path is served by FremSoft.
// "standalone": real DLL is not loaded at all. FremSoft answers every
//               PDU* call. Useful for machines without Chipsoft installed.

#ifndef FREMSOFT_H
#define FREMSOFT_H

#include <stdint.h>
#include <windows.h>

// Same primitive types as the CSTech2Win shim uses.
typedef uint32_t T_PDU_ERROR;
typedef uint32_t UNUM32;
typedef uint8_t  UNUM8;
typedef char     CHAR8;

// PDU_ERROR codes we use (subset of ISO 22900-2 Table 25).
#define PDU_STATUS_NOERROR              0x00000000
#define PDU_ERR_INVALID_PARAMETERS      0x00000007
#define PDU_ERR_EVENT_QUEUE_EMPTY       0x00000018
#define PDU_ERR_FCT_FAILED              0x0000000B

// Mode chosen at DllMain attach via HKLM\SOFTWARE\OpenSAAB\Collector\Mode.
typedef enum {
    FREMSOFT_MODE_PASSTHROUGH = 0,  // forward to real DLL (default)
    FREMSOFT_MODE_RECORD      = 1,  // forward + log (today's behavior)
    FREMSOFT_MODE_PLAYBACK    = 2,  // serve from recording, real DLL still loaded
    FREMSOFT_MODE_STANDALONE  = 3,  // serve from recording, real DLL NOT loaded
} fremsoft_mode_t;

// Lifecycle — called from dllmain.c at process attach/detach.
//
// Returns 0 on success. Reads the recording path from the registry
// (HKLM\SOFTWARE\OpenSAAB\Collector\PlaybackRecording). Also opens
// the fremsoft activity log at %TEMP%\fremsoft_<wall_ms>.log — same
// line format as the cstech2win shim so the bundled scapy decoder
// (fremsoft-decoder.exe in the OpenSAAB Collector tray) can tail-pipe
// it without special-casing.
int  fremsoft_init(fremsoft_mode_t mode);
void fremsoft_shutdown(void);

// Per-call activity logging (matching the cstech2win shim's format).
// fremsoft.c emits these around every PDUStartComPrimitive +
// scheduled-event delivery so the decoder console sees a live UDS
// timeline even though FremSoft is fabricating the responses.
void fremsoft_log_req_pdu(const uint8_t* bytes, uint32_t len);
void fremsoft_log_rsp_uds(const uint8_t* bytes, uint32_t len);

// Mode accessor — wrappers consult this to decide route at call time.
fremsoft_mode_t fremsoft_get_mode(void);

// ---- Data path replacement entry points ----
// These are called from shim/cstech2win/src/wrappers.c when the active
// mode is PLAYBACK or STANDALONE. Same signatures as the real PDU* fns.

T_PDU_ERROR fremsoft_PDUStartComPrimitive(
    UNUM32 hMod, UNUM32 hCLL, UNUM32 CoPType,
    UNUM32 CoPDataSize, UNUM8* pCoPData,
    void* pCoPCtrlData, void* pCoPTag, UNUM32* phCoP);

T_PDU_ERROR fremsoft_PDUGetEventItem(
    UNUM32 hMod, UNUM32 hCLL, void** pEventItem);

T_PDU_ERROR fremsoft_PDURegisterEventCallback(
    UNUM32 hMod, UNUM32 hCLL,
    void (__stdcall *cb)(UNUM32, UNUM32, void*));

T_PDU_ERROR fremsoft_PDUDestroyItem(void* pItem);

// ---- Stateful stubs (called when standalone mode and we can't forward) ----

T_PDU_ERROR fremsoft_PDUConstruct(CHAR8* OptionStr, void* pAPITag);
T_PDU_ERROR fremsoft_PDUDestruct(void);
T_PDU_ERROR fremsoft_PDUConnect(UNUM32 hMod);
T_PDU_ERROR fremsoft_PDUDisconnect(UNUM32 hMod);
T_PDU_ERROR fremsoft_PDUCreateComLogicalLink(
    UNUM32 hMod, UNUM32 ResourceId, UNUM32 ResourceData,
    void* pResourceIdItem, UNUM32* phCLL, UNUM32 pCllCreateFlag);
T_PDU_ERROR fremsoft_PDUDestroyComLogicalLink(UNUM32 hMod, UNUM32 hCLL);
T_PDU_ERROR fremsoft_PDUSetComParam(UNUM32 hMod, UNUM32 hCLL, void* pParam);
T_PDU_ERROR fremsoft_PDUGetComParam(UNUM32 hMod, UNUM32 hCLL, UNUM32 ParamId, void** pParam);
T_PDU_ERROR fremsoft_PDUIoCtl(UNUM32 hMod, UNUM32 hCLL,
                              UNUM32 IoCtlCommandId, void* pInput, void** pOutput);

// ---- Discovery stubs ----

T_PDU_ERROR fremsoft_PDUGetVersion(UNUM32 hMod, void* pVersionData);
T_PDU_ERROR fremsoft_PDUGetModuleIds(void* pModuleIdList);
T_PDU_ERROR fremsoft_PDUGetResourceIds(UNUM32 hMod, void* pResourceIdList);
T_PDU_ERROR fremsoft_PDUGetResourceStatus(void* pResourceStatusList);
T_PDU_ERROR fremsoft_PDUGetConflictingResources(UNUM32 hMod, UNUM32 ResourceId, void* pInputModuleList, void** pOutputConflictList);
T_PDU_ERROR fremsoft_PDUGetObjectId(UNUM32 PdtObjectType, CHAR8* pShortName, UNUM32* pPduObjectId);
T_PDU_ERROR fremsoft_PDUGetStatus(UNUM32 hMod, UNUM32 hCLL, UNUM32 hCop, UNUM32* pStatusCode, UNUM32* pTimestamp, UNUM32* pExtraInfo);
T_PDU_ERROR fremsoft_PDUGetTimestamp(UNUM32 hMod, UNUM32* pTimestamp);
T_PDU_ERROR fremsoft_PDUGetLastError(UNUM32 hMod, UNUM32 hCLL, UNUM32* pErrorCode, UNUM32* phCop, UNUM32* pTimestamp, UNUM32* pExtraErrorInfo);

#endif // FREMSOFT_H
