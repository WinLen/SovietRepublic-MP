// SPDX-License-Identifier: GPL-3.0-only
// See cargo_settings.h. Follows SoylentGreed's stop settings watch
// (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "cargo_settings.h"
#include "commands.h"
#include "world_refs.h"
#include "../core/safe_mem.h"
#include "../session/two_states.h"
#include "../ui/lang.h"
#include <algorithm>
#include <string.h>
#include <vector>

#define WORLD_RVA 0x9D4F10
#define WINDOWS_RVA 0x9E19D0     // the open windows
#define WINDOW_OBJECT 0x240
#define WINDOW_KIND 0x248
#define WINDOW_PARENT 0x5C8      // a selection window's parent window
#define VEHICLE_WINDOW 1
#define PICKER_WINDOW 0xD
#define DOWN_RVA 0xA54E90
#define PRESS_RVA 0xA54E91
#define ENTER_RVA 0xA54CAC
#define KEYPAD_ENTER_RVA 0xA54D2C
#define VEHICLES_OFFSET 0x12810
#define BUILDINGS_OFFSET 0x11B08
#define LINES_OFFSET 0x12800
#define VEHICLE_LINE 0x650
#define STOPS_LIST 0x638         // a vehicle's stop records, 0x198 each
#define LINKS_LIST 0xD38         // a building's links (pointers), link +10 its record
#define LINK_RECORD 0x10
#define RECORD_BYTES 0x198
#define HALF_BYTES 0xC8
#define HALVES 2
#define RESOURCES 0xC2B0
#define RESOURCE_STRIDE 0x340
#define TYPES 0x12840
#define TYPE_STRIDE 0x9F08
#define MAX_OBJECTS 8            // per kind
#define MAX_WINDOWS 4096
#define MAX_LIST 1000000
#define MAX_RECORDS 256
#define MAX_ELEMENTS 4096
#define DESTROY_RVA 0x1513F0
#define COPY_RVA 0x204EF0
#define REFRESH_RVA 0x499790
#define MAGIC 0x31534743

enum { KIND_STOP = 0, KIND_LINK = 1 };
static const char* const kObjectText[2] = { "vehicle", "building" };
static const char* const kRecordText[2] = { "stop", "link" };

// A half's lists: the pairs (+8, 0x10 each), the int lists and the definition lists.
#define VECTORS 7
static const unsigned kVecAt[VECTORS] = { 0x08, 0x38, 0x50, 0x68, 0x80, 0x98, 0xB0 };
static const unsigned kVecElem[VECTORS] = { 16, 4, 4, 8, 8, 8, 8 };
// The scalars 204ef0 copies (the rest of +0..+37 is padding).
struct Span {
    unsigned at, bytes;
};
static const Span kScalars[] = { { 0x00, 1 }, { 0x04, 4 }, { 0x20, 9 }, { 0x2C, 8 } };
#define SCALAR_BYTES 22

static const TsmRoutineSig kRoutines[] = {
    { DESTROY_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x33, 0xff, 0x48 }, 16 },
    { COPY_RVA, { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe }, 16 },
    { REFRESH_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 }, 16 },
};
static int g_ready = -1;

void CargoSettings_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static bool Ready()
{
    return Ref_CheckRoutineTable(&g_ready, kRoutines, sizeof(kRoutines) / sizeof(kRoutines[0]), "CARGO");
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

static bool ReadQ(U64 at, U64& v) { return at && SafeMemcpyPod(&v, (const void*)at, 8); }

static bool ReadVec(U64 at, unsigned elem, std::vector<BYTE>& out)
{
    U64 v[2] = {};
    out.clear();
    if (!SafeMemcpyPod(v, (const void*)at, 16)) return false;
    if (!v[0] && !v[1]) return true;
    if (!v[0] || v[1] < v[0] || (v[1] - v[0]) % elem || (v[1] - v[0]) / elem > MAX_ELEMENTS) return false;
    out.resize((size_t)(v[1] - v[0]));
    return SafeMemcpyPod(out.data(), (const void*)v[0], out.size());
}

struct Half {
    BYTE scalars[SCALAR_BYTES];
    std::vector<BYTE> vec[VECTORS];
    bool operator==(const Half& o) const
    {
        if (memcmp(scalars, o.scalars, SCALAR_BYTES) != 0) return false;
        for (int i = 0; i < VECTORS; i++)
            if (vec[i] != o.vec[i]) return false;
        return true;
    }
};

static bool ReadHalf(U64 at, Half& h)
{
    if (!at) return false;
    size_t k = 0;
    for (const Span& s : kScalars) {
        if (!SafeMemcpyPod(h.scalars + k, (const void*)(at + s.at), s.bytes)) return false;
        k += s.bytes;
    }
    for (int i = 0; i < VECTORS; i++)
        if (!ReadVec(at + kVecAt[i], kVecElem[i], h.vec[i])) return false;
    return true;
}

// A stop list's records, or a link list's links (0: unreadable or too long).
static unsigned RecordCount(int kind, U64 object)
{
    U64 v[2] = {};
    if (!object || !SafeMemcpyPod(v, (const void*)(object + (kind == KIND_STOP ? STOPS_LIST : LINKS_LIST)), 16) || !v[0] || v[1] < v[0])
        return 0;
    U64 each = kind == KIND_STOP ? RECORD_BYTES : 8;
    if ((v[1] - v[0]) % each) return 0;
    U64 n = (v[1] - v[0]) / each;
    return n <= MAX_RECORDS ? (unsigned)n : 0;
}

static U64 HalfAt(int kind, U64 object, unsigned record, unsigned half)
{
    U64 begin = 0, at = 0;
    if (half >= HALVES || record >= RecordCount(kind, object)) return 0;
    if (!ReadQ(object + (kind == KIND_STOP ? STOPS_LIST : LINKS_LIST), begin) || !begin) return 0;
    if (kind == KIND_STOP) at = begin + (U64)record * RECORD_BYTES;
    else {
        U64 link = 0;
        if (!ReadQ(begin + (U64)record * 8, link) || !ReadQ(link + LINK_RECORD, at) || !at) return 0;
    }
    return at + (U64)half * HALF_BYTES;
}

static bool Natives(U64 at, const BYTE* standIn)
{
    __try {
        ((void (*)(U64, U64))(g_exeBase + DESTROY_RVA))(at, 0);
        ((U64(*)(U64, U64))(g_exeBase + COPY_RVA))(at, (U64)standIn);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The live half takes these scalars and lists: the game's destroy, then its copy from a
// stand-in (the live half's bytes with the scalars and the lists in mod memory).
static bool Replace(U64 at, const Half& h)
{
    BYTE standIn[HALF_BYTES];
    if (!SafeMemcpyPod(standIn, (const void*)at, HALF_BYTES)) return false;
    size_t k = 0;
    for (const Span& s : kScalars) {
        memcpy(standIn + s.at, h.scalars + k, s.bytes);
        k += s.bytes;
    }
    for (int i = 0; i < VECTORS; i++)
        Ref_SetStandInVector(standIn + kVecAt[i], h.vec[i].empty() ? nullptr : h.vec[i].data(), h.vec[i].size());
    return Natives(at, standIn);
}

static bool GuardedRef(int kind, U64 object, RouteRef& r)
{
    __try {
        return kind == KIND_STOP ? Ref_Vehicle(object, r) : Ref_Building(object, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static U64 GuardedAt(int kind, const RouteRef& r)
{
    __try {
        return kind == KIND_STOP ? Ref_VehicleAt(r) : Ref_BuildingAt(r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct Writer {
    std::vector<BYTE> b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const BYTE*)p, (const BYTE*)p + n); }
    void u8(BYTE v) { raw(&v, 1); }
    void u16(unsigned short v) { raw(&v, 2); }
    void u32(unsigned v) { raw(&v, 4); }
    void u64(U64 v) { raw(&v, 8); }
};

struct Reader {
    const BYTE* p;
    size_t n, at;
    bool ok;
    bool raw(void* o, size_t k)
    {
        if (!ok || k > n - at) return ok = false;
        memcpy(o, p + at, k);
        at += k;
        return true;
    }
    BYTE u8() { BYTE v = 0; raw(&v, 1); return v; }
    unsigned short u16() { unsigned short v = 0; raw(&v, 2); return v; }
    unsigned u32() { unsigned v = 0; raw(&v, 4); return v; }
    U64 u64() { U64 v = 0; raw(&v, 8); return v; }
    unsigned count(unsigned max) { unsigned c = u32(); if (c > max) ok = false; return ok ? c : 0; }
};

// The message: magic, kind, half, record, the list's length, the object, the scalars,
// then each list (pairs and definition lists with their definitions by table index).
static const char* Encode(int kind, const RouteRef& ref, unsigned records, unsigned record, unsigned half, const Half& h, Writer& w)
{
    w.u32(MAGIC);
    w.u8((BYTE)kind);
    w.u8((BYTE)half);
    w.u16((unsigned short)record);
    w.u32(records);
    w.raw(&ref, sizeof(ref));
    w.raw(h.scalars, SCALAR_BYTES);
    for (int i = 0; i < VECTORS; i++) {
        const std::vector<BYTE>& v = h.vec[i];
        w.u32((unsigned)(v.size() / kVecElem[i]));
        if (kVecElem[i] == 4) {
            w.raw(v.data(), v.size());
            continue;
        }
        for (size_t k = 0; k < v.size(); k += kVecElem[i]) {
            BYTE tag;
            unsigned index;
            if (!Ref_DefPtr(*(const U64*)&v[k], tag, index)) return "a resource or vehicle type is unknown";
            w.u8(tag);
            w.u32(index);
            if (kVecElem[i] == 16) w.u64(*(const U64*)&v[k + 8]);
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

struct Watched {
    int kind;
    U64 object;
    unsigned records;
    std::vector<Half> halves;
};
static std::vector<Watched> g_watched;
static bool g_taken = false;
static unsigned long long g_epoch = 0;

static bool Clicked()
{
    return g_exeBase[DOWN_RVA] || g_exeBase[PRESS_RVA] || g_exeBase[ENTER_RVA] || g_exeBase[KEYPAD_ENTER_RVA];
}

static bool SortedList(U64 at, std::vector<U64>& out)
{
    U64 v[2] = {};
    out.clear();
    if (!SafeMemcpyPod(v, (const void*)at, 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) % 8 || (v[1] - v[0]) / 8 > MAX_LIST) return false;
    out.resize((size_t)((v[1] - v[0]) / 8));
    if (!out.empty() && !SafeMemcpyPod(out.data(), (const void*)v[0], out.size() * 8)) return false;
    std::sort(out.begin(), out.end());
    return true;
}

// The vehicles of open vehicle windows and the buildings of any open window (a
// selection window counts as its parent), at most MAX_OBJECTS of each.
static void Shown(std::vector<Watched>& out)
{
    U64 w[2] = {};
    if (!SafeMemcpyPod(w, g_exeBase + WINDOWS_RVA, 16) || !w[0] || w[1] < w[0] || (w[1] - w[0]) / 8 > MAX_WINDOWS) return;
    std::vector<U64> windows((size_t)((w[1] - w[0]) / 8));
    if (windows.empty() || !SafeMemcpyPod(windows.data(), (const void*)w[0], windows.size() * 8)) return;
    std::vector<U64> vehicles, buildings;
    SortedList((U64)World() + VEHICLES_OFFSET, vehicles);
    SortedList((U64)World() + BUILDINGS_OFFSET, buildings);
    int taken[2] = {};
    for (U64 win : windows) {
        U64 objects[2] = {}, parent = 0;
        unsigned kinds[2] = {};
        if (!win || !SafeMemcpyPod(&kinds[0], (const void*)(win + WINDOW_KIND), 4) || !ReadQ(win + WINDOW_OBJECT, objects[0])) continue;
        if (kinds[0] == PICKER_WINDOW && ReadQ(win + WINDOW_PARENT, parent) && parent) {
            SafeMemcpyPod(&kinds[1], (const void*)(parent + WINDOW_KIND), 4);
            ReadQ(parent + WINDOW_OBJECT, objects[1]);
        }
        for (int i = 0; i < 2; i++) {
            U64 object = objects[i];
            int kind = -1;
            if (!object) continue;
            if (kinds[i] == VEHICLE_WINDOW && std::binary_search(vehicles.begin(), vehicles.end(), object)) kind = KIND_STOP;
            else if (std::binary_search(buildings.begin(), buildings.end(), object)) kind = KIND_LINK;
            if (kind < 0 || taken[kind] >= MAX_OBJECTS) continue;
            bool listed = false;
            for (const Watched& s : out) listed |= s.object == object;
            unsigned records = RecordCount(kind, object);
            if (listed || !records) continue;
            Watched s;
            s.kind = kind;
            s.object = object;
            s.records = records;
            s.halves.resize((size_t)records * HALVES);
            bool ok = true;
            for (unsigned k = 0; k < records * HALVES && ok; k++) ok = ReadHalf(HalfAt(kind, object, k / HALVES, k % HALVES), s.halves[k]);
            if (!ok) continue;
            out.push_back(std::move(s));
            taken[kind]++;
        }
    }
}

void CargoSettings_Before(bool simBusy)
{
    g_taken = false;
    g_watched.clear();
    if (simBusy || !Commands_TakeUi() || !Clicked() || !Ready()) return;
    Shown(g_watched);
    g_epoch = Commands_ApplyEpoch();
    g_taken = !g_watched.empty();
}

static void Take(const Watched& s, unsigned record, unsigned half, U64 at, const Half& now)
{
    RouteRef ref = kNoRef;
    Writer w;
    const char* why = !GuardedRef(s.kind, s.object, ref) ? "it is not in the world's list"
                                                         : Encode(s.kind, ref, s.records, record, half, now, w);
    // Put back at once: the command makes the change on every PC.
    bool back = Replace(at, s.halves[(size_t)record * HALVES + half]);
    bool allowed = s.kind == KIND_STOP ? TwoStates_CheckVehicleControlAllowed((const BYTE*)s.object, Tr("настройка груза чужого транспорта", "configuring foreign vehicle cargo"))
                                       : TwoStates_CheckBuildingControlAllowed((const BYTE*)s.object, Tr("настройка груза чужого здания", "configuring foreign building cargo"));
    if (!allowed) return;
    if (why || !back) {
        Logf("republicmp  CARGO: %s %u (%s) of a %s not shared - %s", kRecordText[s.kind], record, half ? "unloading" : "loading",
             kObjectText[s.kind], why ? why : "it could not be put back", back ? "nothing is done" : "it stays on this PC");
        return;
    }
    Commands_Submit(LS_CMD_CARGO_SETTINGS, w.b.data(), w.b.size());
}

void CargoSettings_After(bool simBusy)
{
    if (!g_taken) return;
    g_taken = false;
    if (!simBusy && Commands_TakeUi() && Commands_ApplyEpoch() == g_epoch) {
        for (const Watched& s : g_watched) {
            if (RecordCount(s.kind, s.object) != s.records) continue; // a record added or removed: not compared
            for (unsigned k = 0; k < s.records * HALVES; k++) {
                U64 at = HalfAt(s.kind, s.object, k / HALVES, k % HALVES);
                Half now;
                if (!ReadHalf(at, now) || now == s.halves[k]) continue;
                Take(s, k / HALVES, k % HALVES, at, now);
            }
        }
    }
    g_watched.clear();
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

static const char* Decode(Reader& in, Half& h)
{
    in.raw(h.scalars, SCALAR_BYTES);
    for (int i = 0; i < VECTORS && in.ok; i++) {
        unsigned n = in.count(MAX_ELEMENTS);
        std::vector<BYTE>& v = h.vec[i];
        v.assign((size_t)n * kVecElem[i], 0);
        if (kVecElem[i] == 4) {
            in.raw(v.data(), v.size());
            continue;
        }
        for (unsigned k = 0; k < n && in.ok; k++) {
            BYTE tag = in.u8();
            unsigned index = in.u32();
            U64 p = 0;
            if (in.ok && !Ref_DefPtrAt(tag, index, p)) return "a resource or vehicle type is not here";
            memcpy(&v[(size_t)k * kVecElem[i]], &p, 8);
            if (kVecElem[i] == 16) {
                U64 value = in.u64();
                memcpy(&v[(size_t)k * 16 + 8], &value, 8);
            }
        }
    }
    return in.ok && in.at == in.n ? nullptr : "unknown layout";
}

static bool Refresh(U64 vehicle)
{
    __try {
        U64 line = *(U64*)(vehicle + VEHICLE_LINE), lines = *(U64*)(World() + LINES_OFFSET);
        if (line && lines) ((void (*)(U64, U64))(g_exeBase + REFRESH_RVA))(lines, vehicle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CargoSettings_ApplyCommand(const void* data, size_t size)
{
    Reader in = { (const BYTE*)data, size, 0, true };
    RouteRef ref = kNoRef;
    if (in.u32() != MAGIC) return false;
    int kind = in.u8();
    unsigned half = in.u8(), record = in.u16(), records = in.u32();
    in.raw(&ref, sizeof(ref));
    if (!in.ok || kind > KIND_LINK || half >= HALVES) return false;
    const char* why = nullptr;
    Half h;
    U64 object = GuardedAt(kind, ref), at = 0;
    if (!Ready()) why = "a game routine differs";
    else if (!object) why = kind == KIND_STOP ? "the vehicle is gone" : "the building is gone";
    else if (RecordCount(kind, object) != records) why = "its records differ here";
    else if (!(at = HalfAt(kind, object, record, half))) why = "the record cannot be read";
    else if (!(why = Decode(in, h)) && !Replace(at, h)) why = "the game's half copy faulted";
    else if (!why && kind == KIND_STOP && !Refresh(object)) why = "the line refresh faulted";
    Logf("republicmp  CARGO: %s %u (%s) of %s #%d: %s", kRecordText[kind], record, half ? "unloading" : "loading", kObjectText[kind],
         ref.place, why ? why : "made");
    return why == nullptr;
}
