// FremSoft — JSON recording loader.
//
// Parses fremsoft/recordings/*.json (built by tools/build_recording.py)
// into an in-memory list of recorded_exchange_t. Lookup is linear scan;
// recordings are small (today's check_codes is 58 entries) so a real
// hashmap isn't worth the complexity for the MVP.

#include "recording.h"
#include "fremsoft.h"

// Single-TU jsmn: defining JSMN_STATIC pulls the implementation inline
// (and marks the symbols static) so this is the only translation unit
// that needs jsmn. No JSMN_HEADER guard.
#define JSMN_STATIC
#include "vendor/jsmn.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct recording_t {
    char                  recording_id[128];
    char                  vehicle_profile[128];
    uint32_t              num_exchanges;
    recorded_exchange_t*  exchanges;
};

// Forward decl — the shim's logger is linked in alongside us.
extern void shim_log(const char* fmt, ...);

// ---- helpers ----------------------------------------------------------------

static int json_eq(const char* json, const jsmntok_t* tok, const char* s) {
    int slen = (int)strlen(s);
    return tok->type == JSMN_STRING
        && (tok->end - tok->start) == slen
        && strncmp(json + tok->start, s, slen) == 0;
}

static char* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

// Parse a "0x07E0" or "07E0" or "0x07e0" string into uint32. Returns 0 on bad.
static uint32_t parse_can_id_str(const char* s, int slen) {
    char buf[16] = {0};
    if (slen <= 0 || slen >= (int)sizeof(buf)) return 0;
    int off = 0;
    if (slen > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) off = 2;
    memcpy(buf, s + off, (size_t)(slen - off));
    return (uint32_t)strtoul(buf, NULL, 16);
}

// Parse "27 01 7e 11" or "270171e11" into bytes. Returns malloc'd buffer.
static uint8_t* parse_hex_bytes(const char* s, int slen, uint32_t* out_len) {
    uint8_t* out = (uint8_t*)malloc((size_t)slen / 2 + 4);
    if (!out) { *out_len = 0; return NULL; }
    uint32_t n = 0;
    int hi = -1;
    for (int i = 0; i < slen; i++) {
        char c = s[i];
        int v = -1;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
        else continue;
        if (hi < 0) { hi = v; }
        else { out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
    }
    *out_len = n;
    return out;
}

// ---- public API -------------------------------------------------------------

recording_t* recording_load(const char* path) {
    size_t json_len = 0;
    char* json = read_file(path, &json_len);
    if (!json) {
        shim_log("FATAL|recording_load|cannot read %s", path);
        return NULL;
    }

    jsmn_parser p;
    jsmn_init(&p);
    int total = jsmn_parse(&p, json, json_len, NULL, 0);
    if (total < 1) {
        shim_log("FATAL|recording_load|jsmn pre-pass failed (%d) for %s", total, path);
        free(json); return NULL;
    }
    jsmntok_t* tok = (jsmntok_t*)malloc((size_t)total * sizeof(jsmntok_t));
    if (!tok) { free(json); return NULL; }
    jsmn_init(&p);
    int n = jsmn_parse(&p, json, json_len, tok, total);
    if (n < 1 || tok[0].type != JSMN_OBJECT) {
        shim_log("FATAL|recording_load|json not an object (n=%d)", n);
        free(tok); free(json); return NULL;
    }

    recording_t* r = (recording_t*)calloc(1, sizeof(*r));
    if (!r) { free(tok); free(json); return NULL; }

    // Walk top-level object: recording_id, vehicle_profile, exchanges.
    // We do a single forward pass; the field-order of our generated JSON
    // is stable so this works.
    int i = 1;  // skip outer object
    while (i < n) {
        if (json_eq(json, &tok[i], "recording_id") && i + 1 < n) {
            int len = tok[i+1].end - tok[i+1].start;
            if (len > (int)sizeof(r->recording_id) - 1) len = sizeof(r->recording_id) - 1;
            memcpy(r->recording_id, json + tok[i+1].start, (size_t)len);
            i += 2;
        }
        else if (json_eq(json, &tok[i], "vehicle_profile") && i + 1 < n) {
            int len = tok[i+1].end - tok[i+1].start;
            if (len > (int)sizeof(r->vehicle_profile) - 1) len = sizeof(r->vehicle_profile) - 1;
            memcpy(r->vehicle_profile, json + tok[i+1].start, (size_t)len);
            i += 2;
        }
        else if (json_eq(json, &tok[i], "exchanges") && i + 1 < n
                                                    && tok[i+1].type == JSMN_ARRAY) {
            int arr_size = tok[i+1].size;
            r->exchanges = (recorded_exchange_t*)calloc((size_t)arr_size, sizeof(*r->exchanges));
            r->num_exchanges = (uint32_t)arr_size;
            int j = i + 2;  // first element of array
            for (int e = 0; e < arr_size && j < n; e++) {
                // Each element is { "tx": {can_id,uds}, "rx": [{can_id,uds,delay_ms},...] }
                if (tok[j].type != JSMN_OBJECT) { j++; continue; }
                int obj_size = tok[j].size;
                j++;
                for (int k = 0; k < obj_size && j < n; k++) {
                    if (json_eq(json, &tok[j], "tx") && j + 1 < n
                                                    && tok[j+1].type == JSMN_OBJECT) {
                        int tx_size = tok[j+1].size;
                        int t = j + 2;
                        for (int q = 0; q < tx_size && t < n; q++) {
                            if (json_eq(json, &tok[t], "can_id") && t + 1 < n) {
                                r->exchanges[e].can_id = parse_can_id_str(
                                    json + tok[t+1].start,
                                    tok[t+1].end - tok[t+1].start);
                                t += 2;
                            } else if (json_eq(json, &tok[t], "uds") && t + 1 < n) {
                                r->exchanges[e].uds_bytes = parse_hex_bytes(
                                    json + tok[t+1].start,
                                    tok[t+1].end - tok[t+1].start,
                                    &r->exchanges[e].uds_len);
                                t += 2;
                            } else { t += 2; }
                        }
                        j = t;
                    } else if (json_eq(json, &tok[j], "rx") && j + 1 < n
                                                           && tok[j+1].type == JSMN_ARRAY) {
                        int rx_size = tok[j+1].size;
                        r->exchanges[e].num_responses = (uint32_t)rx_size;
                        r->exchanges[e].responses = (recorded_response_t*)calloc(
                            (size_t)rx_size, sizeof(recorded_response_t));
                        int t = j + 2;
                        for (int rx = 0; rx < rx_size && t < n; rx++) {
                            if (tok[t].type != JSMN_OBJECT) { t++; continue; }
                            int o = tok[t].size;
                            t++;
                            for (int q = 0; q < o && t < n; q++) {
                                if (json_eq(json, &tok[t], "can_id") && t + 1 < n) {
                                    r->exchanges[e].responses[rx].can_id = parse_can_id_str(
                                        json + tok[t+1].start,
                                        tok[t+1].end - tok[t+1].start);
                                    t += 2;
                                } else if (json_eq(json, &tok[t], "uds") && t + 1 < n) {
                                    r->exchanges[e].responses[rx].uds_bytes = parse_hex_bytes(
                                        json + tok[t+1].start,
                                        tok[t+1].end - tok[t+1].start,
                                        &r->exchanges[e].responses[rx].uds_len);
                                    t += 2;
                                } else if (json_eq(json, &tok[t], "delay_ms") && t + 1 < n) {
                                    char dbuf[16] = {0};
                                    int dl = tok[t+1].end - tok[t+1].start;
                                    if (dl > 15) dl = 15;
                                    memcpy(dbuf, json + tok[t+1].start, (size_t)dl);
                                    r->exchanges[e].responses[rx].delay_ms =
                                        (uint32_t)strtoul(dbuf, NULL, 10);
                                    t += 2;
                                } else { t += 2; }
                            }
                        }
                        j = t;
                    } else { j += 2; }
                }
            }
            i = j;
        } else {
            i += 2;  // skip unrecognized
        }
    }

    free(tok);
    free(json);
    shim_log("INIT |recording_load|loaded %u exchanges from %s (id=%s)",
             r->num_exchanges, path, r->recording_id);
    return r;
}

void recording_free(recording_t* r) {
    if (!r) return;
    for (uint32_t i = 0; i < r->num_exchanges; i++) {
        free(r->exchanges[i].uds_bytes);
        for (uint32_t j = 0; j < r->exchanges[i].num_responses; j++) {
            free(r->exchanges[i].responses[j].uds_bytes);
        }
        free(r->exchanges[i].responses);
    }
    free(r->exchanges);
    free(r);
}

const recorded_exchange_t* recording_lookup(
    const recording_t* r, uint32_t can_id,
    const uint8_t* uds, uint32_t uds_len)
{
    if (!r) return NULL;
    for (uint32_t i = 0; i < r->num_exchanges; i++) {
        recorded_exchange_t* e = &r->exchanges[i];
        if (e->can_id != can_id) continue;
        if (e->uds_len != uds_len) continue;
        if (memcmp(e->uds_bytes, uds, uds_len) == 0) return e;
    }
    return NULL;
}

uint32_t recording_size(const recording_t* r) {
    return r ? r->num_exchanges : 0;
}

void recording_log_summary(const recording_t* r) {
    if (!r) { shim_log("INFO |recording=NULL"); return; }
    shim_log("INFO |recording id=%s vehicle=%s exchanges=%u",
             r->recording_id, r->vehicle_profile, r->num_exchanges);
}
