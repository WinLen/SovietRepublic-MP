// SPDX-License-Identifier: GPL-3.0-only
// See line_accept.h. The copy's layout and the block's calls follow
// SoylentGreed's line accept route (RepublicLAN, read against the disassembly
// of 1.1.1.9).
#include "line_accept.h"
#include "commands.h"
#include "world_refs.h"
#include "../core/safe_mem.h"
#include <string.h>
#include <vector>

#define WORLD_RVA 0x9D4F10
#define LINES_OFFSET 0x12800
#define LINE_WINDOWS_RVA 0x9E19D0
#define WINDOW_LINE 0x240
#define WINDOW_COPY 0x270
#define LINE_BYTES 0x290
#define LINE_NAME 0xC
#define LINE_NAME_CHARS 0x100
#define LINE_STATIONS 0x210
#define LINE_RECORDS 0x228
#define LINE_VEHICLES 0x240
#define LINE_MEASURES 0x260
#define LINE_COUNTS 0x278
#define RECORD_BYTES 0x198
#define HALF_BYTES 0xC8
#define HALF_PAIRS 0x8
#define RECORD_NODE 0x190
#define BORDER_SOVIET 0x11B00
#define BORDER_WESTERN 0x11AF8
#define CACHE_BEGIN 0xC6B0
#define CACHE_END 0xC6B8
#define MAX_ELEMENTS 4096
#define MAX_STOPS 1024
#define MAGIC 0x31414E4C

static const unsigned kHalfInts[2] = { 0x38, 0x50 };
static const unsigned kHalfPtrs[4] = { 0x68, 0x80, 0x98, 0xB0 };

#define COPY_RVA 0x495830
#define MERGE_RVA 0x4945A0
#define DIRTY_RVA 0x22A6D0
#define VEHICLES_RVA 0x497650
#define NAME_RVA 0x499600

static const TsmRoutineSig kRoutines[] = {
    { COPY_RVA, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x11, 0x05, 0x00, 0x00, 0x48, 0x8b, 0xc4, 0x44, 0x88, 0x48, 0x20 }, 16 },
    { MERGE_RVA, { 0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41 }, 16 },
    { DIRTY_RVA, { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 }, 16 },
    { VEHICLES_RVA, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xf5, 0x02, 0x00, 0x00, 0x53, 0x41, 0x56, 0x48, 0x83, 0xec, 0x38 }, 16 },
    { NAME_RVA, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x78, 0x01, 0x00, 0x00, 0x4c, 0x8b, 0xdc, 0x56, 0x48, 0x81, 0xec }, 16 },
};

static int g_ready = -1;

void LineAccept_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static bool Ready()
{
    return Ref_CheckRoutineTable(&g_ready, kRoutines, sizeof(kRoutines) / sizeof(kRoutines[0]), "LINE");
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

// Reads checked with the host's ReadablePtr (no SEH around the vectors).
static bool Read(void* out, U64 at, size_t n)
{
    if (!n) return true;
    if (!at || !ReadablePtr((const void*)at, n)) return false;
    memcpy(out, (const void*)at, n);
    return true;
}

template <class T> static bool Get(U64 at, T& v) { return Read(&v, at, sizeof(T)); }

// A vector's elements (begin/end at `at`) as raw bytes.
static bool ReadVec(U64 at, size_t elem, std::vector<BYTE>& out, U64* begin = nullptr)
{
    U64 v[2] = {};
    out.clear();
    if (!Read(v, at, 16)) return false;
    if (begin) *begin = v[0];
    if (!v[0] && !v[1]) return true;
    if (!v[0] || v[1] < v[0] || (v[1] - v[0]) % elem || (v[1] - v[0]) / elem > MAX_ELEMENTS) return false;
    out.resize((size_t)(v[1] - v[0]));
    return Read(out.data(), v[0], out.size());
}

struct Writer {
    std::vector<BYTE> b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const BYTE*)p, (const BYTE*)p + n); }
    void u8(BYTE v) { raw(&v, 1); }
    void u16(unsigned short v) { raw(&v, 2); }
    void u32(unsigned v) { raw(&v, 4); }
    void u64(U64 v) { raw(&v, 8); }
    void ref(const RouteRef& r) { raw(&r, sizeof(r)); }
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
    RouteRef ref() { RouteRef r = kNoRef; raw(&r, sizeof(r)); return r; }
    unsigned count(unsigned max) { unsigned c = u32(); if (c > max) ok = false; return ok ? c : 0; }
};

static bool GuardedRef(bool (*fn)(U64, RouteRef&), U64 object, RouteRef& r)
{
    __try {
        return fn(object, r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static U64 GuardedAt(U64 (*fn)(const RouteRef&), const RouteRef& r)
{
    __try {
        return fn(r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---------------------------------------------------------------------------
// The clicking PC
// ---------------------------------------------------------------------------

static const char* Encode(U64 line, U64 copy, Writer& w)
{
    U64 world = (U64)World();
    RouteRef lineRef = kNoRef;
    if (!GuardedRef(Ref_Line, line, lineRef)) return "the line is not in the lines manager";
    w.u32(MAGIC);
    w.ref(lineRef);
    BYTE pub = 0;
    int vtype = 0, ttype = 0, icon = 0, measure = 0;
    if (!Get(copy + 1, pub) || !Get(copy + 4, vtype) || !Get(copy + 8, ttype) || !Get(copy + 0x258, icon) ||
        !Get(copy + 0x25C, measure))
        return "the copy cannot be read";
    w.u8(pub);
    w.u32((unsigned)vtype);
    w.u32((unsigned)ttype);
    w.u32((unsigned)icon);
    w.u32((unsigned)measure);
    unsigned short name[LINE_NAME_CHARS] = {};
    if (!Read(name, copy + LINE_NAME, sizeof(name))) return "the name cannot be read";
    unsigned short chars = 0;
    while (chars < LINE_NAME_CHARS && name[chars]) chars++;
    if (chars >= LINE_NAME_CHARS) return "the name is too long";
    w.u16(chars);
    w.raw(name, chars * 2);

    U64 borders[2] = {};
    Get(world + BORDER_SOVIET, borders[0]);
    Get(world + BORDER_WESTERN, borders[1]);
    std::vector<BYTE> bytes;
    if (!ReadVec(copy + LINE_STATIONS, 8, bytes)) return "the stations cannot be read";
    w.u32((unsigned)(bytes.size() / 8));
    for (size_t i = 0; i < bytes.size(); i += 8) {
        U64 s = *(U64*)&bytes[i];
        if (!s) w.u8(0);
        else if (s == borders[0]) w.u8(2);
        else if (s == borders[1]) w.u8(3);
        else {
            RouteRef r = kNoRef;
            if (!GuardedRef(Ref_Building, s, r)) return "a station is not in the building list";
            w.u8(1);
            w.ref(r);
        }
    }

    U64 records = 0;
    std::vector<BYTE> recs;
    if (!ReadVec(copy + LINE_RECORDS, RECORD_BYTES, recs, &records)) return "the stop records cannot be read";
    if (recs.size() / RECORD_BYTES > MAX_STOPS) return "too many stops";
    w.u32((unsigned)(recs.size() / RECORD_BYTES));
    for (size_t i = 0; i < recs.size() / RECORD_BYTES; i++) {
        const BYTE* r = recs.data() + i * RECORD_BYTES;
        U64 live = records + i * RECORD_BYTES;
        for (int h = 0; h < 2; h++) {
            unsigned o = h * HALF_BYTES;
            w.raw(r + o, 8);
            w.raw(r + o + 0x20, 0x18);
            if (!ReadVec(live + o + HALF_PAIRS, 16, bytes)) return "a stop's cargo list cannot be read";
            w.u32((unsigned)(bytes.size() / 16));
            for (size_t k = 0; k < bytes.size(); k += 16) {
                BYTE tag;
                unsigned index;
                if (!Ref_DefPtr(*(U64*)&bytes[k], tag, index)) return "a cargo definition is unknown";
                w.u8(tag);
                w.u32(index);
                w.u64(*(U64*)&bytes[k + 8]);
            }
            for (unsigned off : kHalfInts) {
                if (!ReadVec(live + o + off, 4, bytes)) return "a stop's list cannot be read";
                w.u32((unsigned)(bytes.size() / 4));
                w.raw(bytes.data(), bytes.size());
            }
            for (unsigned off : kHalfPtrs) {
                if (!ReadVec(live + o + off, 8, bytes)) return "a stop's list cannot be read";
                w.u32((unsigned)(bytes.size() / 8));
                for (size_t k = 0; k < bytes.size(); k += 8) {
                    BYTE tag;
                    unsigned index;
                    if (!Ref_DefPtr(*(U64*)&bytes[k], tag, index)) return "a definition is unknown";
                    w.u8(tag);
                    w.u32(index);
                }
            }
        }
        U64 node = *(U64*)(r + RECORD_NODE);
        RouteRef nr = kNoRef;
        if (node && !GuardedRef(Ref_Node, node, nr)) return "a stop's signal node is not in its network";
        w.u8(node ? 1 : 0);
        w.ref(nr);
    }

    if (!ReadVec(copy + LINE_MEASURES, 4, bytes)) return "the measures cannot be read";
    w.u32((unsigned)(bytes.size() / 4));
    w.raw(bytes.data(), bytes.size());
    if (!ReadVec(copy + LINE_COUNTS, 4, bytes)) return "the counts cannot be read";
    w.u32((unsigned)(bytes.size() / 4));
    w.raw(bytes.data(), bytes.size());
    return nullptr;
}

// The block's loop writes the line's vehicles' +66c on the clicking PC too: kept and put back.
static U64 g_keptLine = 0;
static std::vector<std::pair<U64, unsigned>> g_keptMeasures;

static void KeepMeasures(U64 line)
{
    g_keptMeasures.clear();
    g_keptLine = line;
    std::vector<BYTE> bytes;
    if (!ReadVec(line + LINE_VEHICLES, 8, bytes)) return;
    for (size_t i = 0; i < bytes.size(); i += 8) {
        U64 v = *(U64*)&bytes[i];
        unsigned m = 0;
        if (Get(v + 0x66C, m)) g_keptMeasures.push_back(std::make_pair(v, m));
    }
}

void LineAccept_RestoreMeasures()
{
    for (const auto& k : g_keptMeasures)
        if (ReadablePtr((const void*)(k.first + 0x66C), 4)) *(unsigned*)(k.first + 0x66C) = k.second;
    g_keptMeasures.clear();
    g_keptLine = 0;
}

bool LineAccept_Capture(U64 line, U64 copy)
{
    if (!Ready()) {
        Logf("republicmp  LINE: accept not shared - a game routine differs; nothing is done");
        return true;
    }
    Writer w;
    const char* why = Encode(line, copy, w);
    if (why) {
        Logf("republicmp  LINE: accept not shared - %s; nothing is done", why);
        return true;
    }
    KeepMeasures(line);
    Commands_Submit(LS_CMD_LINE_ACCEPT, w.b.data(), w.b.size());
    return true;
}

// ---------------------------------------------------------------------------
// Every PC, at the boundary
// ---------------------------------------------------------------------------

// The copy rebuilt in mod memory (495830 only reads it).
struct StandIn {
    std::vector<BYTE> line, records;
    std::vector<U64> stations;
    std::vector<BYTE> measures, counts;
    std::vector<std::vector<BYTE>> pools;
};

static const void* Pool(StandIn& s, const std::vector<BYTE>& data)
{
    if (data.empty()) return nullptr;
    s.pools.push_back(data);
    return s.pools.back().data();
}

static const char* Decode(const BYTE* data, size_t size, StandIn& s, U64& line)
{
    Reader in = { data, size, 0, true };
    U64 world = (U64)World();
    if (in.u32() != MAGIC) return "unknown layout";
    RouteRef lineRef = in.ref();
    s.line.assign(LINE_BYTES, 0);
    s.line[1] = in.u8();
    unsigned vtype = in.u32(), ttype = in.u32(), icon = in.u32(), measure = in.u32();
    memcpy(&s.line[4], &vtype, 4);
    memcpy(&s.line[8], &ttype, 4);
    memcpy(&s.line[0x258], &icon, 4);
    memcpy(&s.line[0x25C], &measure, 4);
    unsigned short chars = in.u16();
    if (chars >= LINE_NAME_CHARS) return "unknown layout";
    in.raw(&s.line[LINE_NAME], chars * 2);

    U64 borders[2] = {};
    Get(world + BORDER_SOVIET, borders[0]);
    Get(world + BORDER_WESTERN, borders[1]);
    unsigned ns = in.count(MAX_STOPS);
    for (unsigned i = 0; i < ns && in.ok; i++) {
        BYTE tag = in.u8();
        if (tag == 0) s.stations.push_back(0);
        else if (tag == 2 || tag == 3) {
            if (!borders[tag - 2]) return "no border building";
            s.stations.push_back(borders[tag - 2]);
        } else if (tag == 1) {
            RouteRef r = in.ref();
            U64 b = GuardedAt(Ref_BuildingAt, r);
            if (!b) return "a station is gone";
            s.stations.push_back(b);
        } else
            return "unknown layout";
    }

    unsigned nr = in.count(MAX_STOPS);
    s.records.assign((size_t)nr * RECORD_BYTES, 0);
    s.pools.reserve((size_t)nr * 14);
    std::vector<BYTE> tmp;
    for (unsigned i = 0; i < nr && in.ok; i++) {
        BYTE* r = s.records.data() + (size_t)i * RECORD_BYTES;
        for (int h = 0; h < 2; h++) {
            BYTE* o = r + h * HALF_BYTES;
            in.raw(o, 8);
            in.raw(o + 0x20, 0x18);
            unsigned np = in.count(MAX_ELEMENTS);
            tmp.assign((size_t)np * 16, 0);
            for (unsigned k = 0; k < np && in.ok; k++) {
                BYTE tag = in.u8();
                unsigned index = in.u32();
                U64 p = 0;
                if (!Ref_DefPtrAt(tag, index, p)) return "a cargo definition is unknown here";
                U64 value = in.u64();
                memcpy(&tmp[k * 16], &p, 8);
                memcpy(&tmp[k * 16 + 8], &value, 8);
            }
            Ref_SetStandInVector(o + HALF_PAIRS, Pool(s, tmp), tmp.size());
            for (unsigned off : kHalfInts) {
                unsigned n = in.count(MAX_ELEMENTS);
                tmp.assign((size_t)n * 4, 0);
                in.raw(tmp.data(), tmp.size());
                Ref_SetStandInVector(o + off, Pool(s, tmp), tmp.size());
            }
            for (unsigned off : kHalfPtrs) {
                unsigned n = in.count(MAX_ELEMENTS);
                tmp.assign((size_t)n * 8, 0);
                for (unsigned k = 0; k < n && in.ok; k++) {
                    BYTE tag = in.u8();
                    unsigned index = in.u32();
                    U64 p = 0;
                    if (!Ref_DefPtrAt(tag, index, p)) return "a definition is unknown here";
                    memcpy(&tmp[k * 8], &p, 8);
                }
                Ref_SetStandInVector(o + off, Pool(s, tmp), tmp.size());
            }
        }
        BYTE hasNode = in.u8();
        RouteRef nr2 = in.ref();
        U64 node = 0;
        if (hasNode && !(node = GuardedAt(Ref_NodeAt, nr2))) return "a stop's signal node is gone";
        memcpy(r + RECORD_NODE, &node, 8);
    }
    unsigned nm = in.count(MAX_STOPS);
    s.measures.assign((size_t)nm * 4, 0);
    in.raw(s.measures.data(), s.measures.size());
    unsigned nc = in.count(MAX_STOPS);
    s.counts.assign((size_t)nc * 4, 0);
    in.raw(s.counts.data(), s.counts.size());
    if (!in.ok || in.at != size) return "unknown layout";

    Ref_SetStandInVector(&s.line[LINE_STATIONS], s.stations.data(), s.stations.size() * 8);
    Ref_SetStandInVector(&s.line[LINE_RECORDS], s.records.data(), s.records.size());
    Ref_SetStandInVector(&s.line[LINE_MEASURES], s.measures.data(), s.measures.size());
    Ref_SetStandInVector(&s.line[LINE_COUNTS], s.counts.data(), s.counts.size());
    line = GuardedAt(Ref_LineAt, lineRef);
    return line ? nullptr : "the line is gone";
}

typedef U64 (*Call4)(U64, U64, U64, U64);

// The block's calls on this PC's line (no C++ objects here: SEH).
static const char* Block(U64 line, U64 copy, unsigned* vehicles)
{
    U64 world = (U64)World();
    __try {
        U64 mgr = *(U64*)(world + LINES_OFFSET);
        if (!mgr) return "no lines manager";
        ((Call4)(g_exeBase + COPY_RVA))(0, line, copy, 0);
        // 4945a0 refreshes the copy of every window on this line: one with no copy is left out.
        U64 masked[64];
        int nMasked = 0;
        U64* wb = *(U64**)(g_exeBase + LINE_WINDOWS_RVA);
        U64* we = *(U64**)(g_exeBase + LINE_WINDOWS_RVA + 8);
        for (U64* at = wb; at && at < we && nMasked < 64; ++at) {
            U64 win = *at;
            if (win && *(U64*)(win + WINDOW_LINE) == line && !*(U64*)(win + WINDOW_COPY)) {
                *(U64*)(win + WINDOW_LINE) = 0;
                masked[nMasked++] = win;
            }
        }
        ((Call4)(g_exeBase + MERGE_RVA))(mgr, line, 0, 0);
        for (int i = 0; i < nMasked; i++) *(U64*)(masked[i] + WINDOW_LINE) = line;
        U64* vb = *(U64**)(line + LINE_VEHICLES);
        U64* ve = *(U64**)(line + LINE_VEHICLES + 8);
        for (U64* at = vb; at && at < ve; ++at) {
            *(unsigned*)(*at + 0x66C) = 0xBF800000u;
            ++*vehicles;
        }
        ((Call4)(g_exeBase + DIRTY_RVA))(line, mgr + 0x48, 0, 0);
        ((Call4)(g_exeBase + VEHICLES_RVA))(mgr, line, 0, 0);
        if (!*(BYTE*)(line + 1)) ((Call4)(g_exeBase + NAME_RVA))(0, line, 0, 0);
        *(U64*)(world + CACHE_END) = *(U64*)(world + CACHE_BEGIN);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "the block faulted";
    }
    return nullptr;
}

bool LineAccept_ApplyCommand(const void* data, size_t size)
{
    if (!Ready()) return false;
    StandIn s;
    U64 line = 0;
    const char* why = Decode((const BYTE*)data, size, s, line);
    unsigned vehicles = 0;
    if (!why) why = Block(line, (U64)s.line.data(), &vehicles);
    Logf("republicmp  LINE: accept, %u station(s), %u stop record(s), %u vehicle(s): %s", (unsigned)s.stations.size(),
         (unsigned)(s.records.size() / RECORD_BYTES), vehicles, why ? why : "done");
    return why == nullptr;
}
