// CSTech2Win shim — common header.
//
// All translation units include this. Keeps the Win32 surface area minimal so
// we stay portable between MinGW-w64 and MSVC.

#ifndef CSTECH2WIN_SHIM_H
#define CSTECH2WIN_SHIM_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

// ---- ISO 22900-2 D-PDU API ---------------------------------------------------
typedef uint32_t T_PDU_ERROR;
typedef uint32_t UNUM32;
typedef uint8_t  UNUM8;
typedef char     CHAR8;

// Calling convention for D-PDU API on 32-bit Windows is __stdcall.
#define PDUAPI __stdcall

// ---- Logging -----------------------------------------------------------------
// One log file per process; opened lazily on first call.
// Logs are pipe-delimited: timestamp_ms | tid | event | function | detail
extern void shim_log_init(void);
extern void shim_log(const char* fmt, ...);
extern void shim_log_hex(const char* tag, const void* buf, size_t len);

// ---- Real-DLL handle ---------------------------------------------------------
extern HMODULE g_hRealDll;

#endif // CSTECH2WIN_SHIM_H
