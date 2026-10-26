// SPDX-License-Identifier: GPL-3.0-only
// See vehicle_switches.h. The comparison follows SoylentGreed's interface
// switches (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "vehicle_switches.h"
#include "commands.h"
#include "world_refs.h"
#include "../session/two_states.h"
#include "../core/diag_log.h"
#include <string.h>
#include <vector>

#define WORLD_RVA 0x9D4F10
#define VEHICLES_OFFSET 0x12810
#define DOWN_RVA 0xA54E90        // the left button held, as the interface reads it
#define PRESS_RVA 0xA54E91       // the left button pressed this frame
#define ENTER_RVA 0xA54CAC       // Enter accepts a window's text field (a rename)
#define KEYPAD_ENTER_RVA 0xA54D2C
#define VEHICLE_BYTES 0x1AD8
#define STOPS_LIST 0x638         // stop records, 0x198 each
#define STOP_BYTES 0x198
#define CARGO_LIST 0x3B8         // cargo records, 0x10 each
#define CARGO_BYTES 0x10
#define MAX_RECORDS 256
#define MAX_VEHICLES 16384
#define MAX_WORDS 60
#define NAME_FLAG 0x108          // a vehicle's custom-name flag and its name (0x40 characters at +0x10a): text
#define NAME_END ((0x10A + 0x40 + 3) & ~3u)
#define SAMPLE_EVERY 16

enum { KIND_VEHICLE = 0, KIND_STOP = 1, KIND_CARGO = 2, KIND_COUNT = 3 };
static const unsigned kListAt[KIND_COUNT] = { 0, STOPS_LIST, CARGO_LIST };
static const unsigned kRecordBytes[KIND_COUNT] = { VEHICLE_BYTES, STOP_BYTES, CARGO_BYTES };
static const char* const kKindText[KIND_COUNT] = { "vehicle", "stop record", "cargo record" };

#pragma pack(push, 1)
struct SwitchWord {
    unsigned offset, old, now;
};
struct SwitchMsg {
    RouteRef vehicle;
    unsigned kind;
    unsigned records; // record kinds: the list's length (the shape must match)
    unsigned count;
    SwitchWord words[MAX_WORDS];
};
#pragma pack(pop)

void VehicleSwitches_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

static bool Copy(void* to, const void* from, size_t n)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        memcpy(to, from, n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

// A vehicle's record list: begin and count (0 when unreadable or too long).
static U64 Records(U64 vehicle, int kind, unsigned* count)
{
    U64 v[2] = {};
    *count = 0;
    if (!Copy(v, (const void*)(vehicle + kListAt[kind]), 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) % kRecordBytes[kind])
        return 0;
    U64 n = (v[1] - v[0]) / kRecordBytes[kind];
    if (n > MAX_RECORDS) return 0;
    *count = (unsigned)n;
    return v[0];
}

struct Snap {
    U64 vehicle;
    unsigned records[KIND_COUNT]; // records per kind (the vehicle itself: 1)
    size_t at[KIND_COUNT];        // where each kind's bytes start in g_bytes
};

static std::vector<Snap> g_snaps;
static std::vector<BYTE> g_bytes;
static bool g_taken = false, g_click = false;
static unsigned long long g_epoch = 0;
static unsigned g_frame = 0;
// Words the interface changes without a click, per kind (word index within one record).
static std::vector<bool> g_noise[KIND_COUNT];
static int g_noiseLogged = 0, g_heapLogged = 0;

static bool Clicked()
{
    return g_exeBase[DOWN_RVA] || g_exeBase[PRESS_RVA] || g_exeBase[ENTER_RVA] || g_exeBase[KEYPAD_ENTER_RVA];
}

static bool TakeSnapshot()
{
    g_snaps.clear();
    g_bytes.clear();
    U64 v[2] = {};
    if (!Copy(v, World() + VEHICLES_OFFSET, 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) / 8 > MAX_VEHICLES) return false;
    size_t n = (size_t)((v[1] - v[0]) / 8);
    std::vector<U64> list(n);
    if (n && !Copy(list.data(), (const void*)v[0], n * 8)) return false;
    g_snaps.reserve(n);
    for (U64 vehicle : list) {
        if (!vehicle) continue;
        Snap s = {};
        s.vehicle = vehicle;
        U64 begin[KIND_COUNT] = { vehicle, 0, 0 };
        s.records[KIND_VEHICLE] = 1;
        for (int k = KIND_STOP; k < KIND_COUNT; k++) begin[k] = Records(vehicle, k, &s.records[k]);
        bool ok = true;
        for (int k = 0; k < KIND_COUNT && ok; k++) {
            s.at[k] = g_bytes.size();
            size_t bytes = (size_t)s.records[k] * kRecordBytes[k];
            if (!bytes) continue;
            g_bytes.resize(s.at[k] + bytes);
            ok = Copy(g_bytes.data() + s.at[k], (const void*)begin[k], bytes);
        }
        if (ok) g_snaps.push_back(s);
        else g_bytes.resize(s.at[KIND_VEHICLE]);
    }
    return true;
}

static bool GuardedRef(U64 vehicle, RouteRef& r)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        ok = Ref_Vehicle(vehicle, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static bool PointerLike(U64 v)
{
    unsigned high = (unsigned)(v >> 32);
    if (high >= 0x100 && high <= 0x7FFF) return true;
    // Windows x64 low-heap segments (4 GB .. 1 TB): any 4-byte aligned value
    // with high in 1..0xFF that points to readable memory is a heap pointer.
    if (high >= 1 && high < 0x100 && (v & 3) == 0) {
        U64 probe = 0;
        if (Copy(&probe, (const void*)v, 8)) return true;
    }
    return false;
}

static bool TextWord(int kind, unsigned offset)
{
    return kind == KIND_VEHICLE && offset >= NAME_FLAG && offset < NAME_END;
}

// A stop record's lists (each half's +8 pairs and +38..+c8 lists): heap links,
// their contents are cargo_settings.cpp's. The vehicle's internal vectors and
// object pointers (+3a0..+3d0, +458..+4a0, +4f0..+500, +600..+608, +638..+658,
// +680..+6b8, +738..+750, +1708..+1710, +1ad0..+1ad8) and dynamic simulation/render
// fields (+770, +d24, +d60..+1140, +17e8..+17f0, +1a54..+1a5c, +1a7c..+1a84)
// must never be overwritten as switches.
#define HALF_BYTES 0xC8
static bool ListWord(int kind, unsigned offset)
{
    if (kind == KIND_VEHICLE) {
        return (offset >= 0x3A0 && offset < 0x3D0) ||
               (offset >= 0x458 && offset < 0x4A0) ||
               (offset >= 0x4F0 && offset < 0x500) ||
               (offset >= 0x600 && offset < 0x608) ||
               (offset >= 0x638 && offset < 0x658) ||
               (offset >= 0x680 && offset < 0x6B8) ||
               (offset >= 0x738 && offset < 0x750) ||
               (offset >= 0x770 && offset < 0x774) ||
               (offset >= 0xD24 && offset < 0xD28) ||
               (offset >= 0xD60 && offset < 0x1140) ||
               (offset >= 0x1708 && offset < 0x1710) ||
               (offset >= 0x17E8 && offset < 0x17F0) ||
               (offset >= 0x1A54 && offset < 0x1A5C) ||
               (offset >= 0x1A7C && offset < 0x1A84) ||
               (offset >= 0x1AD0 && offset < 0x1AD8);
    }
    if (kind == KIND_CARGO) return offset < 8; // Resource* pointer at cargo entry +0x00
    if (kind != KIND_STOP || offset >= 2 * HALF_BYTES) return false;
    unsigned l = offset % HALF_BYTES;
    return (l >= 0x8 && l < 0x20) || l >= 0x38;
}

static void Submit(const SwitchMsg& m)
{
    Commands_Submit(LS_CMD_VEHICLE_SWITCH, &m, sizeof(m));
}

// Compares one record kind of one vehicle; on a click the changes are put back and carried.
static void Compare(const Snap& s, int kind, bool click)
{
    unsigned now = 0;
    U64 begin = kind == KIND_VEHICLE ? s.vehicle : Records(s.vehicle, kind, &now);
    if (kind == KIND_VEHICLE) now = 1;
    if (!s.records[kind] || now != s.records[kind] || !begin) return; // reshaped lists are not compared
    size_t bytes = (size_t)now * kRecordBytes[kind];
    std::vector<BYTE> cur(bytes);
    if (!Copy(cur.data(), (const void*)begin, bytes)) return;
    const BYTE* old = g_bytes.data() + s.at[kind];
    if (memcmp(cur.data(), old, bytes) == 0) return;
    std::vector<bool>& noise = g_noise[kind];
    if (noise.empty()) noise.assign(kRecordBytes[kind] / 4, false);

    SwitchMsg m;
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    m.records = kind == KIND_VEHICLE ? 0 : now;
    bool haveRef = false;
    for (size_t q = 0; q + 8 <= bytes; q += 8) {
        U64 a = *(const U64*)(old + q), b = *(const U64*)(cur.data() + q);
        if (a == b) continue;
        unsigned inRecord = (unsigned)(q % kRecordBytes[kind]);
        bool heap = (PointerLike(a) || PointerLike(b)) && !TextWord(kind, inRecord);
        for (unsigned h = 0; h < 8; h += 4) {
            unsigned wa = *(const unsigned*)(old + q + h), wb = *(const unsigned*)(cur.data() + q + h);
            if (wa == wb) continue;
            unsigned offset = (unsigned)(q + h);
            unsigned word = (offset % kRecordBytes[kind]) / 4;
            if (ListWord(kind, offset % kRecordBytes[kind])) continue;
            if (!click) {
                if (!noise[word]) {
                    noise[word] = true;
                    if (g_noiseLogged < 40) {
                        ++g_noiseLogged;
                        Logf("republicmp  SWITCH: the interface changes %s +0x%X without a click - left out", kKindText[kind],
                             offset % kRecordBytes[kind]);
                    }
                }
                continue;
            }
            if (noise[word]) continue;
            if (heap) {
                if (g_heapLogged < 20) {
                    ++g_heapLogged;
                    Logf("republicmp  SWITCH: a click changed %s +0x%X to a heap address - not carried", kKindText[kind],
                         offset % kRecordBytes[kind]);
                }
                continue;
            }
            if (!haveRef) {
                if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)s.vehicle, "изменение настроек чужого транспорта")) {
                    Copy((void*)begin, old, bytes);
                    return;
                }
                haveRef = GuardedRef(s.vehicle, m.vehicle);
                if (!haveRef) {
                    Logf("republicmp  SWITCH: a click changed a vehicle that is not in the world's list - not carried");
                    return;
                }
            }
            // Put back at once: the clicking PC's simulation never sees it before the command.
            *(unsigned*)(begin + offset) = wa;
            m.words[m.count++] = { offset, wa, wb };
            if (m.count == MAX_WORDS) {
                Submit(m);
                m.count = 0;
            }
        }
    }
    if (m.count) Submit(m);
}

void VehicleSwitches_Reset()
{
    g_taken = false;
    g_snaps.clear();
    g_bytes.clear();
}

void VehicleSwitches_Before(bool simBusy)
{
    g_taken = false;
    if (!Commands_TakeUi() || simBusy) return;
    g_click = Clicked();
    // Frames without a click are compared now and then, to learn the interface's own changes.
    if (!g_click && (++g_frame % SAMPLE_EVERY) != 0) return;
    g_epoch = Commands_ApplyEpoch();
    g_taken = TakeSnapshot();
}

void VehicleSwitches_After(bool simBusy)
{
    if (!g_taken) return;
    g_taken = false;
    if (simBusy || !Commands_TakeUi() || Commands_ApplyEpoch() != g_epoch) return;
    for (const Snap& s : g_snaps)
        for (int k = 0; k < KIND_COUNT; k++) Compare(s, k, g_click);
}

bool VehicleSwitches_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SwitchMsg)) return false;
    SwitchMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.kind >= KIND_COUNT || !m.count || m.count > MAX_WORDS) return false;
    const char* why = nullptr;
    unsigned differing = 0;
    DiagLog_BeginSafeSeh();
    __try {
        U64 vehicle = Ref_VehicleAt(m.vehicle);
        unsigned records = 1;
        U64 begin = !vehicle ? 0 : m.kind == KIND_VEHICLE ? vehicle : Records(vehicle, m.kind, &records);
        if (!vehicle) why = "the vehicle is gone";
        else if (!begin || (m.kind != KIND_VEHICLE && records != m.records)) why = "its records differ here";
        else {
            for (unsigned i = 0; i < m.count && !why; i++)
                if (m.words[i].offset % 4 || m.words[i].offset + 4 > (U64)records * kRecordBytes[m.kind]) why = "a word is outside";
            for (unsigned i = 0; i < m.count && !why; i++) {
                unsigned off = m.words[i].offset;
                unsigned inRec = off % kRecordBytes[m.kind];
                if (ListWord(m.kind, inRec)) continue;
                U64 qAddr = (begin + off) & ~7ull;
                if (!TextWord(m.kind, inRec) && PointerLike(*(U64*)qAddr)) continue;
                unsigned* p = (unsigned*)(begin + off);
                if (*p != m.words[i].old && *p != m.words[i].now) differing++;
                *p = m.words[i].now;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while writing";
    }
    DiagLog_EndSafeSeh();
    Logf("republicmp  SWITCH: %s of vehicle #%d, %u word(s) from +0x%X%s: %s", kKindText[m.kind], m.vehicle.place, m.count,
         m.words[0].offset, differing ? " (some read neither value)" : "", why ? why : "made");
    return why == nullptr;
}
