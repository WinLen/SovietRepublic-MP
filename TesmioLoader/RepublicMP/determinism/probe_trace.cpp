// SPDX-License-Identifier: GPL-3.0-only
// See probe_trace.h.
#include "probe_trace.h"
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WORLD_RVA 0x9D4F10

#define BLD_STORAGE_OFFSET 0x970
#define BLD_TYPE_OFFSET 0x318
#define BLD_POS_OFFSET 0x3A0
#define TYPE_CATEGORY_OFFSET 0x360
#define CIT_AGE_OFFSET 0xD4
#define CIT_POS_OFFSET 0x620
#define VEH_ID_OFFSET 0x4
#define VEH_POS_OFFSET 0xDE0
#define SLOT_SIZE 0xE0
#define ENTRY_SIZE 0x10

#define MAX_OBJECTS 100000
#define MAX_SLOTS 32
#define MAX_ENTRIES 64

#define TRACE_BUDGET (1024ull << 20)
#define REPORT_OBJECTS 6
#define REPORT_WORDS 16
#define NOISE_OFFSETS 12
#define WATCH_LINES 60

// A spot where citizens keep splitting first (one city, several runs): with
// HOT_SPOT on, the load leftovers of the buildings and citizens around it are
// logged with their values, in case the simulation reads one of them after
// all. Off by default; set the spot to the place to examine.
#define HOT_SPOT 0
#define HOT_X 0.0f
#define HOT_Z 0.0f
#define HOT_BUILDING_RADIUS 250.0f
#define HOT_CITIZEN_RADIUS 40.0f
#define HOT_WORDS 48
#define HOT_LINES 900
#define CIT_HOME_OFFSET 0x20
#define CIT_ASSIGNMENT_OFFSET 0xC8

// The agents' split (832ae0): the every-frame list and the three buckets
// (world vectors of citizen pointers), the bucket counter, then the places in
// the citizen list of the every-frame list's members, then per citizen (by its
// place in the citizen list, word 4 + 1 + 512 + place) which list it is on and
// where: tag | list << 24 | position, 0x8FFFFFFF on none.
#define AGENT_LISTS 4
#define AGENT_LIST_OFFSET 0x126C0  // then +0x126D8, +0x126F0, +0x12708 (stride 0x18)
#define AGENT_BUCKET_COUNTER_RVA 0x9963F0
#define AGENT_MEMBERS 512
#define AGENT_CITIZENS 8192
#define AGENTS_SIZE ((AGENT_LISTS + 1 + AGENT_MEMBERS + AGENT_CITIZENS) * 4)
#define AGENT_TAG 0x80000000u // every word gets the top bit, so no pair of them looks like a heap pointer

void ProbeTrace_Bind(const TsmHost* host)
{
    TsmBind(host);
}

// Bytes left out: written by the renderer, the camera or a window on its own
// schedule, never read by the simulation (from SoylentGreed's vehicle and
// building notes).
struct Mask {
    unsigned offset, size;
};
static const Mask kBuildingMasks[] = {
    { 0x121C, 0x14 }, // recomputed by the building window every frame
    { 0x1158, 0x8 },  // camera visibility +115a and distance +115c (this PC's view, sim_patches)
    { 0x1344, 4 },    // animation phase of a building near the camera (16c900)
    { 0x137C, 4 },    // ambient sound timer of a site near the camera (139a70: amb_construction)
};
static const Mask kVehicleMasks[] = {
    { 0x8, 0x20 },    // model name buffer
    { 0xED8, 0xBC },  // render transform
    { 0x115C, 4 },    // interpolant of the render pass
    { 0x13D4, 4 },    // animation start
    { 0x13D8, 2 },    // visible now / last frame
    { 0x13DC, 0xC },  // camera distances: +13dc (GetZDistance at 66e2cb for every vehicle; its readers
                      // store only sound, effects and the model LOD - SoylentGreed's camera-dependence.md),
                      // +13e0, +13e4 (moved with the camera when the player built during a probe)
    { 0x142C, 4 },  { 0x1468, 4 }, { 0x153C, 4 }, { 0x16FC, 4 }, { 0x1768, 4 }, { 0x1A18, 4 }, // render words
    { 0x1878, 8 },    // sound object
    { 0x18A4, 0x1C }, // sound levels the sound code (6a36f0, 6a480c, 6a7520) derives from the camera distance
    { 0x1958, 4 },    // sound volume
};

// C3DDLL64 Start (fd3e0) writes the QPC start at +10; Stop (fd440)
// writes QPC end/frequency at +18/+20 and decrements a warm-up count at +28.
// H_timerStop replaces its derived FPS, so these wall/frame counters are
// not the controlled timer. Keep all four simulation timer words in the trace.
static const Mask kTimerMasks[] = { { 0x10, 0x18 }, { 0x28, 4 } };

static const Mask kWorldMasks[] = {
    { 0xF60, 0x18 }, // mouse ray and cursor point
    // Clocks the game advances on every rendered frame, paused or not, so
    // their value follows the frame count, not the ticks: the ticker text
    // scroll +117b8 (Late 30d344, PowerTime ignoring the pause; read only by
    // the string copy at 332eb8), the shader time +13010 (30d372; read by
    // the effect constants at 588991), the interface statistics refresh
    // +c6c8..+c6dc (counted down by the interface at 31ad2c every 3 s of
    // real time) and an interface window clock +d224 (4c4c62).
    { 0x117B8, 4 },
    { 0x13010, 4 },
    { 0xC6C8, 0x18 },
    { 0xD224, 4 },
};

// A list kind is a world vector of object pointers; a single kind is one
// object in the exe's data (the world, the game timer). A watched kind never
// stops the trace: each offset that starts to differ is logged once.
struct Kind {
    const char* name;
    unsigned listOffset; // 0: the single object at `rva`
    unsigned size;
    const Mask* masks;
    int maskCount;
    bool storage;
    bool watch;
    unsigned rva;
    bool lists; // the citizen-pointer vectors inside each object of the list (see CopyLists)
};
static const Kind kKinds[] = {
    { "vehicle", 0x12810, 0x1AD8, kVehicleMasks, (int)(sizeof(kVehicleMasks) / sizeof(Mask)), false, false, 0 },
    { "building", 0x11B08, 0x1420, kBuildingMasks, (int)(sizeof(kBuildingMasks) / sizeof(Mask)), true, false, 0 },
    { "citizen", 0x126A8, 0x750, nullptr, 0, false, false, 0 },
    { "world", 0, 0x14800, kWorldMasks, (int)(sizeof(kWorldMasks) / sizeof(Mask)), false, true, WORLD_RVA },
    { "timer", 0, 0x30, kTimerMasks, (int)(sizeof(kTimerMasks) / sizeof(Mask)), false, true, 0x9D4EE0 }, // C3D_TIMER the frame loop scales (PowerTime)
    // Synthetic (no rva): how 832ae0 split the citizens this step - see CopyAgents.
    { "agents", 0, AGENTS_SIZE, nullptr, 0, false, true, 0 },
    // The order of the citizens in every vector of citizen pointers inside a
    // building or a vehicle: a container kept in pointer order would order
    // them differently in each run (each load lays the heap out anew).
    { "building lists", 0x11B08, 0x1420, nullptr, 0, false, false, 0, true },
    { "vehicle lists", 0x12810, 0x1AD8, nullptr, 0, false, false, 0, true },
};
#define KIND_CITIZEN 2
#define KIND_WORLD 3
#define KIND_TIMER 4
#define KINDS ((int)(sizeof(kKinds) / sizeof(kKinds[0])))

// One object after a step, as one blob: the object's bytes (masks zeroed),
// then for a building its storage: per slot the entry count (u32), the slot's
// 0xE0 bytes and its entries' 16 bytes each.
typedef std::vector<BYTE> Blob;

// ---------------------------------------------------------------------------
// Reading the world
// ---------------------------------------------------------------------------

static long long Count(const BYTE* vec, long long elemSize)
{
    BYTE* b = *(BYTE* const*)vec;
    BYTE* e = *(BYTE* const*)(vec + 8);
    if (!b || e < b) return 0;
    return (e - b) / elemSize;
}

// [p, p + n) is committed readable memory. A read of freed heap would be
// caught anyway, but every fault costs one of the loader's eight crash
// reports, and a real crash later would go unreported.
static bool Readable(const void* p, size_t n)
{
    static const BYTE* lo = nullptr;
    static const BYTE* hi = nullptr;
    const BYTE* a = (const BYTE*)p;
    if (a >= lo && a + n <= hi) return true;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return false;
    lo = (const BYTE*)mbi.BaseAddress;
    hi = lo + mbi.RegionSize;
    return a + n <= hi || Readable(hi, (size_t)(a + n - hi));
}

static bool CopyRaw(const BYTE* from, BYTE* out, size_t n)
{
    if (!Readable(from, n)) return false;
    __try {
        memcpy(out, from, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Appends a building's storage to `blob` (`cap` bytes free); bytes used, -1 if
// a read faulted.
static long long CopyStorage(const BYTE* raw, BYTE* blob, size_t cap)
{
    size_t used = 0;
    __try {
        BYTE* slots = *(BYTE* const*)(raw + BLD_STORAGE_OFFSET);
        long long ns = Count(raw + BLD_STORAGE_OFFSET, SLOT_SIZE);
        if (ns > MAX_SLOTS) ns = MAX_SLOTS;
        for (long long s = 0; s < ns; s++) {
            BYTE* slot = slots + s * SLOT_SIZE;
            long long ne = Count(slot, ENTRY_SIZE);
            if (ne > MAX_ENTRIES) ne = MAX_ENTRIES;
            size_t need = 4 + SLOT_SIZE + (size_t)ne * ENTRY_SIZE;
            if (used + need > cap) break;
            *(unsigned*)(blob + used) = (unsigned)ne;
            memcpy(blob + used + 4, slot, SLOT_SIZE);
            if (ne) memcpy(blob + used + 4 + SLOT_SIZE, *(BYTE**)slot, (size_t)ne * ENTRY_SIZE);
            used += need;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return (long long)used;
}

static bool ReadList(unsigned listOffset, BYTE*** list, long long* n)
{
    __try {
        BYTE* vec = g_exeBase + WORLD_RVA + listOffset;
        *list = *(BYTE***)vec;
        *n = Count(vec, 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *n >= 0 && *n <= MAX_OBJECTS;
}

// The objects' addresses of the last copy, per kind (the same object keeps
// its address for as long as it lives).
static std::vector<BYTE*> g_addresses[KINDS];

static void CopyAgents(Blob& b)
{
    b.assign(AGENTS_SIZE, 0xFF);
    unsigned* w = (unsigned*)b.data();
    BYTE** citizens = nullptr;
    long long nc = 0;
    if (!ReadList(0x126A8, &citizens, &nc)) return;
    std::unordered_map<BYTE*, unsigned> place;
    place.reserve((size_t)nc * 2);
    for (long long i = 0; i < nc; i++) place[citizens[i]] = (unsigned)i;
    for (int l = 0; l < AGENT_LISTS; l++) {
        BYTE** list = nullptr;
        long long n = 0;
        if (!ReadList(AGENT_LIST_OFFSET + l * 0x18, &list, &n)) n = 0;
        w[l] = (unsigned)n | AGENT_TAG;
        unsigned* where = w + AGENT_LISTS + 1 + AGENT_MEMBERS;
        if (l == 0)
            for (int c = 0; c < AGENT_CITIZENS; c++) where[c] = 0x8FFFFFFFu;
        for (long long i = 0; i < n && i < 0xFFFFFF; i++) {
            auto it = place.find(list[i]);
            if (it != place.end() && it->second < AGENT_CITIZENS)
                where[it->second] = AGENT_TAG | ((unsigned)l << 24) | (unsigned)i;
        }
        if (l == 0)
            for (long long i = 0; i < n && i < AGENT_MEMBERS; i++) {
                auto it = place.find(list[i]);
                w[AGENT_LISTS + 1 + i] = it == place.end() ? 0xFFFFFFFE : (it->second | AGENT_TAG);
            }
    }
    w[AGENT_LISTS] = *(unsigned*)(g_exeBase + AGENT_BUCKET_COUNTER_RVA) | AGENT_TAG;
}

// This step's citizens: address -> place in the citizen list.
static std::unordered_map<BYTE*, unsigned> g_citizenPlace;

static void MapCitizens()
{
    g_citizenPlace.clear();
    BYTE** list = nullptr;
    long long n = 0;
    if (!ReadList(0x126A8, &list, &n)) return;
    g_citizenPlace.reserve((size_t)n * 2);
    for (long long i = 0; i < n; i++) g_citizenPlace[list[i]] = (unsigned)i;
}

#define LIST_MAX 4096
static bool ReadPointers(BYTE* const* from, size_t n, BYTE** out)
{
    if (!Readable(from, n * sizeof(BYTE*))) return false;
    __try {
        memcpy(out, from, n * sizeof(BYTE*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Per object: for each vector header {begin, end, capacity} in it whose
// first element is a citizen, the header's offset, the count and the
// citizens' places, in the vector's order (tagged so no pair looks like a
// pointer).
static bool PointerLike(unsigned long long q);

static void CopyLists(const Kind& k, BYTE* object, Blob& b)
{
    b.clear();
    static BYTE raw[0x2000];
    static BYTE* elements[LIST_MAX];
    if (k.size > sizeof(raw) || !CopyRaw(object, raw, k.size)) return;
    for (unsigned o = 0; o + 24 <= k.size; o += 8) {
        BYTE* begin = *(BYTE**)(raw + o);
        BYTE* end = *(BYTE**)(raw + o + 8);
        BYTE* cap = *(BYTE**)(raw + o + 16);
        // only a header that can be one: aligned heap pointers, a sane capacity
        // (anything else would fault in ReadPointers, and every fault costs one
        // of the loader's crash reports)
        if (((ULONG_PTR)begin | (ULONG_PTR)end | (ULONG_PTR)cap) & 7) continue;
        if (!PointerLike((unsigned long long)begin) || end <= begin || cap < end) continue;
        size_t n = (size_t)(end - begin) / 8;
        if ((size_t)(cap - begin) / 8 > LIST_MAX || !ReadPointers((BYTE* const*)begin, n, elements)) continue;
        if (!g_citizenPlace.count(elements[0])) continue;
        unsigned head[2] = { o | 0xC0000000u, (unsigned)n | 0xA0000000u };
        b.insert(b.end(), (BYTE*)head, (BYTE*)head + 8);
        for (size_t i = 0; i < n; i++) {
            auto it = g_citizenPlace.find(elements[i]);
            unsigned w = it == g_citizenPlace.end() ? 0xFFFFFFFEu : (it->second | 0x80000000u);
            b.insert(b.end(), (BYTE*)&w, (BYTE*)&w + 4);
        }
        o += 16; // past this header
    }
}

static void CopyAll(int ki, std::vector<Blob>& out)
{
    const Kind& k = kKinds[ki];
    std::vector<BYTE*>& addresses = g_addresses[ki];
    if (k.lists) {
        BYTE** list = nullptr;
        long long n = 0;
        if (!ReadList(k.listOffset, &list, &n)) n = 0;
        out.resize((size_t)n);
        addresses.assign(list, list + n);
        for (long long i = 0; i < n; i++) {
            if (list[i]) CopyLists(k, list[i], out[(size_t)i]);
            else out[(size_t)i].clear();
        }
        return;
    }
    if (!k.listOffset && !k.rva) {
        addresses.assign(1, nullptr);
        out.resize(1);
        CopyAgents(out[0]);
        return;
    }
    if (!k.listOffset) {
        addresses.assign(1, g_exeBase + k.rva);
        out.resize(1);
        out[0].resize(k.size);
        if (!CopyRaw(g_exeBase + k.rva, out[0].data(), k.size)) memset(out[0].data(), 0, k.size);
        for (int m = 0; m < k.maskCount; m++) memset(out[0].data() + k.masks[m].offset, 0, k.masks[m].size);
        return;
    }
    BYTE** list = nullptr;
    long long n = 0;
    if (!ReadList(k.listOffset, &list, &n)) n = 0;
    out.resize((size_t)n);
    addresses.assign(list, list + n);
    static BYTE storage[MAX_SLOTS * (4 + SLOT_SIZE + MAX_ENTRIES * ENTRY_SIZE)];
    for (long long i = 0; i < n; i++) {
        Blob& b = out[(size_t)i];
        b.resize(k.size);
        BYTE* p = list[i];
        if (!p || !CopyRaw(p, b.data(), k.size)) {
            memset(b.data(), 0, k.size);
            continue;
        }
        for (int m = 0; m < k.maskCount; m++) memset(b.data() + k.masks[m].offset, 0, k.masks[m].size);
        if (k.storage) {
            long long used = CopyStorage(b.data(), storage, sizeof(storage));
            if (used > 0) b.insert(b.end(), storage, storage + used);
        }
    }
}

// Identity of an object in run 2, for its load leftovers: a vehicle's id, a
// building's position bits; a citizen's address (they are born and die in the
// middle of the list, so places shift).
static unsigned long long Key(int ki, const Blob& b, size_t i)
{
    const Kind& k = kKinds[ki];
    if (!k.listOffset) return 0;
    if (k.lists) return i;
    if (ki == KIND_CITIZEN) return i < g_addresses[ki].size() ? (unsigned long long)g_addresses[ki][i] : i;
    if (!k.storage) return *(const unsigned*)(b.data() + VEH_ID_OFFSET);
    unsigned long long h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 12; i++) h = (h ^ b[BLD_POS_OFFSET + i]) * 0x100000001b3ull;
    return h;
}

// A qword that looks like a user-mode heap pointer: different on every load.
static bool PointerLike(unsigned long long q)
{
    unsigned hi = (unsigned)(q >> 32);
    return hi >= 0x1 && hi <= 0x7fff && (unsigned)q != 0;
}

// The dword at `o` is a half of a pointer in either blob.
static bool InPointer(const Blob& a, const Blob& b, size_t o)
{
    size_t q = o & ~(size_t)7;
    if (q + 8 > a.size() || q + 8 > b.size()) return false;
    return PointerLike(*(const unsigned long long*)(a.data() + q)) ||
           PointerLike(*(const unsigned long long*)(b.data() + q));
}

// ---------------------------------------------------------------------------
// Run 1: the stream
// ---------------------------------------------------------------------------
//
// Per step and kind: u32 object count, then per object either
//   u32 index, u32 dword changes n, n x {u32 offset, u32 value}   (same size)
//   u32 index, 0xFFFFFFFF, u32 bytes, the bytes                    (size changed)
// ending with index 0xFFFFFFFF. A count change writes every object in full.

static std::vector<BYTE> g_stream;
static std::vector<size_t> g_stepAt; // step (1-based) - 1 -> offset in g_stream
static std::vector<Blob> g_last[KINDS];
static bool g_full = false;
static unsigned long long g_firstDifference = 0;
static bool g_disk = false;
static HANDLE g_traceFile = INVALID_HANDLE_VALUE;
static unsigned long long g_fileEnd = 0;
static std::vector<unsigned long long> g_fileAt;
static std::vector<DWORD> g_fileSizes;
void ProbeTrace_SetDisk(bool enabled) { g_disk = enabled; }

static bool OpenTraceFile()
{
    wchar_t dir[MAX_PATH], name[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(dir, L"rmp", 0, name)) return false;
    g_traceFile = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (g_traceFile == INVALID_HANDLE_VALUE) DeleteFileW(name);
    return g_traceFile != INVALID_HANDLE_VALUE;
}
static bool TraceFileChunk(bool write, unsigned long long offset, DWORD bytes)
{
    LARGE_INTEGER pos; pos.QuadPart = offset;
    if (!SetFilePointerEx(g_traceFile, pos, nullptr, FILE_BEGIN)) return false;
    DWORD done = 0;
    BOOL ok = write ? WriteFile(g_traceFile, g_stream.data(), bytes, &done, nullptr)
                    : ReadFile(g_traceFile, g_stream.data(), bytes, &done, nullptr);
    return ok && done == bytes;
}

static void Put(unsigned v)
{
    const BYTE* p = (const BYTE*)&v;
    g_stream.insert(g_stream.end(), p, p + 4);
}

static void PutFull(unsigned index, const Blob& b)
{
    Put(index);
    Put(0xFFFFFFFFu);
    Put((unsigned)b.size());
    g_stream.insert(g_stream.end(), b.begin(), b.end());
}

static void Record(int ki, std::vector<Blob>& cur)
{
    std::vector<Blob>& last = g_last[ki];
    Put((unsigned)cur.size());
    bool all = cur.size() != last.size();
    for (size_t i = 0; i < cur.size(); i++) {
        const Blob& c = cur[i];
        if (all || c.size() != last[i].size()) {
            PutFull((unsigned)i, c);
            continue;
        }
        const Blob& l = last[i];
        unsigned changes = 0;
        size_t mark = 0;
        for (size_t o = 0; o + 4 <= c.size(); o += 4) {
            unsigned v = *(const unsigned*)(c.data() + o);
            if (v == *(const unsigned*)(l.data() + o)) continue;
            if (!changes) {
                Put((unsigned)i);
                mark = g_stream.size();
                Put(0);
            }
            Put((unsigned)o);
            Put(v);
            changes++;
        }
        if (changes) memcpy(g_stream.data() + mark, &changes, 4);
    }
    Put(0xFFFFFFFFu);
    last.swap(cur);
}

static unsigned Get(size_t& at)
{
    unsigned v;
    memcpy(&v, g_stream.data() + at, 4);
    at += 4;
    return v;
}

// Applies step `step`'s records to `mirror` (run 1 as it was after that step).
static void Replay(unsigned long long step, std::vector<Blob>* mirror)
{
    size_t at = g_disk ? 0 : g_stepAt[(size_t)(step - 1)];
    for (int ki = 0; ki < KINDS; ki++) {
        std::vector<Blob>& m = mirror[ki];
        unsigned n = Get(at);
        m.resize(n);
        for (;;) {
            unsigned index = Get(at);
            if (index == 0xFFFFFFFFu) break;
            unsigned changes = Get(at);
            Blob& b = m[index];
            if (changes == 0xFFFFFFFFu) {
                unsigned bytes = Get(at);
                b.assign(g_stream.data() + at, g_stream.data() + at + bytes);
                at += bytes;
                continue;
            }
            for (unsigned c = 0; c < changes; c++) {
                unsigned o = Get(at), v = Get(at);
                memcpy(b.data() + o, &v, 4);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Run 2: comparing
// ---------------------------------------------------------------------------

static std::vector<Blob> g_mirror[KINDS], g_cur[KINDS], g_prev[KINDS];
// Per object (by identity), the dwords that already differed the first time
// it was compared: left over from the load (heap contents the game never
// initialised), not simulation.
static std::unordered_map<unsigned long long, std::vector<bool>> g_noise[KINDS];
static bool g_reported = false, g_stopped = false;
static unsigned long long g_steps2 = 0;

static void LogNoise(int ki)
{
    const Kind& k = kKinds[ki];
    std::unordered_map<unsigned, int> perOffset;
    int objects = 0;
    for (auto& e : g_noise[ki]) {
        bool any = false;
        for (size_t d = 0; d < e.second.size(); d++)
            if (e.second[d]) {
                perOffset[(unsigned)(d * 4)]++;
                any = true;
            }
        objects += any;
    }
    Logf("republicmp  PROBE TRACE: %ss: %d of %zu differ already at their first comparison (load leftovers, left out), "
         "%zu offset(s)", k.name, objects, g_noise[ki].size(), perOffset.size());
    for (int line = 0; line < NOISE_OFFSETS && !perOffset.empty(); line++) {
        auto best = perOffset.begin();
        for (auto it = perOffset.begin(); it != perOffset.end(); ++it)
            if (it->second > best->second || (it->second == best->second && it->first < best->first)) best = it;
        Logf("republicmp  PROBE TRACE:   +0x%X in %d %s(s)", best->first, best->second, k.name);
        perOffset.erase(best);
    }
}

static void Where(const Kind& k, const Blob& b, size_t o, char* out, size_t outSize)
{
    if (o < k.size) {
        _snprintf_s(out, outSize, _TRUNCATE, "+0x%X", (unsigned)o);
        return;
    }
    size_t at = k.size;
    for (int s = 0; at + 4 + SLOT_SIZE <= b.size(); s++) {
        unsigned ne = *(const unsigned*)(b.data() + at);
        size_t end = at + 4 + SLOT_SIZE + (size_t)ne * ENTRY_SIZE;
        if (o < end) {
            if (o < at + 4) _snprintf_s(out, outSize, _TRUNCATE, "slot %d count", s);
            else if (o < at + 4 + SLOT_SIZE) _snprintf_s(out, outSize, _TRUNCATE, "slot %d +0x%X", s, (unsigned)(o - at - 4));
            else {
                size_t eo = o - at - 4 - SLOT_SIZE;
                char name[32] = "?";
                const char* res = *(const char* const*)(b.data() + at + 4 + SLOT_SIZE + (eo & ~(size_t)15));
                __try {
                    if (res) strncpy_s(name, res, _TRUNCATE);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                _snprintf_s(out, outSize, _TRUNCATE, "slot %d entry %u (%s) +0x%X", s, (unsigned)(eo / ENTRY_SIZE), name,
                            (unsigned)(eo % ENTRY_SIZE));
            }
            return;
        }
        at = end;
    }
    _snprintf_s(out, outSize, _TRUNCATE, "storage +0x%X", (unsigned)(o - k.size));
}

// A building's place in this step's building list, -1 if none.
static int BuildingPlace(BYTE* p)
{
    if (!p) return -1;
    const std::vector<BYTE*>& a = g_addresses[1];
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] == p) return (int)i;
    return -1;
}

static void Describe(int ki, size_t i, const Blob& b)
{
    const Kind& k = kKinds[ki];
    if (k.lists) {
        Logf("republicmp  PROBE TRACE:   %s of #%zu (words: c0000000|offset in the object, a0000000|count, "
             "80000000|citizen's place):", k.name, i);
    } else if (ki == KIND_CITIZEN) {
        const float* pos = (const float*)(b.data() + CIT_POS_OFFSET);
        Logf("republicmp  PROBE TRACE:   citizen #%zu age %.6f at (%.2f,%.2f,%.2f), home building #%d, assigned to #%d:", i,
             *(const float*)(b.data() + CIT_AGE_OFFSET), pos[0], pos[1], pos[2],
             BuildingPlace(*(BYTE* const*)(b.data() + CIT_HOME_OFFSET)),
             BuildingPlace(*(BYTE* const*)(b.data() + CIT_ASSIGNMENT_OFFSET)));
    } else if (k.storage) {
        int category = -1;
        __try {
            BYTE* type = *(BYTE* const*)(b.data() + BLD_TYPE_OFFSET);
            if (type) category = *(int*)(type + TYPE_CATEGORY_OFFSET);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        const float* pos = (const float*)(b.data() + BLD_POS_OFFSET);
        Logf("republicmp  PROBE TRACE:   building #%zu index %d category %d at (%.2f,%.2f,%.2f):", i, *(const int*)b.data(),
             category, pos[0], pos[1], pos[2]);
    } else {
        const float* pos = (const float*)(b.data() + VEH_POS_OFFSET);
        Logf("republicmp  PROBE TRACE:   vehicle #%zu id %d at (%.3f,%.3f,%.3f):", i, *(const int*)(b.data() + VEH_ID_OFFSET),
             pos[0], pos[1], pos[2]);
    }
}

static void LogDword(const char* where, unsigned r1, unsigned r2, const unsigned* before)
{
    float f1, f2, fb = 0;
    memcpy(&f1, &r1, 4);
    memcpy(&f2, &r2, 4);
    if (before) memcpy(&fb, before, 4);
    if (before)
        Logf("republicmp  PROBE TRACE:     %s: run 1 %08x run 2 %08x, step before %08x (float %.9g / %.9g, before %.9g; "
             "int %d / %d)", where, r1, r2, *before, f1, f2, fb, (int)r1, (int)r2);
    else
        Logf("republicmp  PROBE TRACE:     %s: run 1 %08x run 2 %08x (float %.9g / %.9g; int %d / %d)", where, r1, r2, f1, f2,
             (int)r1, (int)r2);
}

// The load leftovers of an object near the hot spot, with their values.
static int g_hotLines = 0;
static void LogHotNoise(int ki, size_t i, const Blob& a, const Blob& b, const std::vector<bool>& noise)
{
    const Kind& k = kKinds[ki];
    if (!HOT_SPOT || k.lists || g_hotLines >= HOT_LINES) return;
    float radius;
    const float* pos;
    if (ki == KIND_CITIZEN) {
        radius = HOT_CITIZEN_RADIUS;
        pos = (const float*)(b.data() + CIT_POS_OFFSET);
    } else if (k.storage) {
        radius = HOT_BUILDING_RADIUS;
        pos = (const float*)(b.data() + BLD_POS_OFFSET);
    } else {
        return;
    }
    float dx = pos[0] - HOT_X, dz = pos[2] - HOT_Z;
    if (dx * dx + dz * dz > radius * radius) return;
    int words = 0;
    size_t len = a.size() < b.size() ? a.size() : b.size();
    for (size_t o = 0; o + 4 <= len; o += 4)
        if (o / 4 < noise.size() && noise[o / 4] && !InPointer(a, b, o)) words++;
    Logf("republicmp  PROBE TRACE: hot spot load leftovers, %d non-pointer word(s) of", words);
    Describe(ki, i, b);
    g_hotLines += 2;
    int shown = 0;
    for (size_t o = 0; o + 4 <= len && shown < HOT_WORDS && g_hotLines < HOT_LINES; o += 4) {
        if (o / 4 >= noise.size() || !noise[o / 4] || InPointer(a, b, o)) continue;
        char where[96];
        Where(k, b, o, where, sizeof(where));
        LogDword(where, *(const unsigned*)(a.data() + o), *(const unsigned*)(b.data() + o), nullptr);
        shown++;
        g_hotLines++;
    }
}

// A citizen's activity entries (count +0x110, 0x80 each from +0x118; on
// completion 83a6d0 adds the done fraction times the entry's effects to the
// needs +0xd8..): every word that differs between the runs, load leftovers
// included - a field the game never sets on a new entry would show here.
#define CIT_ACTIVITY_COUNT 0x110
#define CIT_ACTIVITIES 0x118
#define CIT_ACTIVITY_SIZE 0x80
#define CIT_ACTIVITY_END 0x498
static void LogActivities(const Blob& a, const Blob& b, const std::vector<bool>& noise)
{
    if (a.size() < CIT_ACTIVITY_END || b.size() < CIT_ACTIVITY_END) return;
    Logf("republicmp  PROBE TRACE:     activities: %d / %d (run 1 / run 2)", *(const int*)(a.data() + CIT_ACTIVITY_COUNT),
         *(const int*)(b.data() + CIT_ACTIVITY_COUNT));
    int shown = 0;
    for (size_t o = CIT_ACTIVITIES; o + 4 <= CIT_ACTIVITY_END && shown < 40; o += 4) {
        unsigned r1 = *(const unsigned*)(a.data() + o), r2 = *(const unsigned*)(b.data() + o);
        if (r1 == r2) continue;
        char where[64];
        _snprintf_s(where, sizeof(where), _TRUNCATE, "activity %u +0x%X%s%s", (unsigned)((o - CIT_ACTIVITIES) / CIT_ACTIVITY_SIZE),
                    (unsigned)((o - CIT_ACTIVITIES) % CIT_ACTIVITY_SIZE), o / 4 < noise.size() && noise[o / 4] ? " (load leftover)" : "",
                    InPointer(a, b, o) ? " (pointer)" : "");
        LogDword(where, r1, r2, nullptr);
        shown++;
    }
}

// Compares kind `ki` (mirror = run 1, cur = run 2); with `report`, logs it.
// Returns the number of differing objects.
static int Compare(int ki, bool report, unsigned long long step)
{
    const Kind& k = kKinds[ki];
    std::vector<Blob>& m = g_mirror[ki];
    std::vector<Blob>& c = g_cur[ki];
    std::vector<Blob>& p = g_prev[ki];
    size_t n = m.size() < c.size() ? m.size() : c.size();
    int differing = 0;
    for (size_t i = 0; i < n; i++) {
        const Blob& a = m[i];
        const Blob& b = c[i];
        unsigned long long key = Key(ki, b, i);
        auto it = g_noise[ki].find(key);
        if (it == g_noise[ki].end() && k.lists) // an order is never a load leftover
            it = g_noise[ki].emplace(key, std::vector<bool>()).first;
        if (it == g_noise[ki].end()) { // first comparison: whatever differs now is a load leftover
            std::vector<bool> noise((b.size() > a.size() ? b.size() : a.size()) / 4, false);
            size_t len = (a.size() < b.size() ? a.size() : b.size());
            for (size_t o = 0; o + 4 <= len; o += 4)
                if (*(const unsigned*)(a.data() + o) != *(const unsigned*)(b.data() + o)) noise[o / 4] = true;
            LogHotNoise(ki, i, a, b, noise);
            g_noise[ki].emplace(key, std::move(noise));
            continue;
        }
        const std::vector<bool>& noise = it->second;
        bool sizeDiffers = a.size() != b.size();
        size_t len = (a.size() < b.size() ? a.size() : b.size());
        int words = 0;
        for (size_t o = 0; o + 4 <= len; o += 4) {
            unsigned r1 = *(const unsigned*)(a.data() + o), r2 = *(const unsigned*)(b.data() + o);
            if (r1 == r2) continue;
            if (o / 4 < noise.size() && noise[o / 4]) continue;
            if (InPointer(a, b, o)) continue;
            if (!words && !sizeDiffers) {
                differing++;
                if (report && differing <= REPORT_OBJECTS) Describe(ki, i, b);
            }
            words++;
            if (report && differing <= REPORT_OBJECTS && words <= REPORT_WORDS) {
                char where[96];
                Where(k, b, o, where, sizeof(where));
                const unsigned* before = i < p.size() && o + 4 <= p[i].size() ? (const unsigned*)(p[i].data() + o) : nullptr;
                LogDword(where, r1, r2, before);
            }
        }
        if (sizeDiffers) {
            differing++;
            if (report && differing <= REPORT_OBJECTS) {
                Describe(ki, i, b);
                Logf("republicmp  PROBE TRACE:     storage size differs: run 1 %zu bytes, run 2 %zu", a.size(), b.size());
            }
        }
        if (report && differing <= REPORT_OBJECTS && words > REPORT_WORDS)
            Logf("republicmp  PROBE TRACE:     ... %d more word(s)", words - REPORT_WORDS);
        if (report && words && ki == KIND_CITIZEN && differing <= REPORT_OBJECTS) LogActivities(a, b, noise);
    }
    if (m.size() != c.size()) differing++;
    if (report && differing)
        Logf("republicmp  PROBE TRACE: %ss first differ after step %llu: %d of them, %zu/%zu listed", k.name, step,
             differing, m.size(), c.size());
    return differing;
}

// A watched kind: logs each offset the first time it differs (after the
// object's load leftovers), and goes on.
static std::unordered_map<unsigned, bool> g_watched[KINDS];
static int g_watchLines[KINDS];

static void Watch(int ki, unsigned long long step)
{
    const Kind& k = kKinds[ki];
    std::vector<Blob>& m = g_mirror[ki];
    std::vector<Blob>& c = g_cur[ki];
    std::vector<Blob>& p = g_prev[ki];
    size_t n = m.size() < c.size() ? m.size() : c.size();
    for (size_t i = 0; i < n; i++) {
        const Blob& a = m[i];
        const Blob& b = c[i];
        unsigned long long key = Key(ki, b, i);
        size_t len = (a.size() < b.size() ? a.size() : b.size());
        auto it = g_noise[ki].find(key);
        if (it == g_noise[ki].end()) {
            std::vector<bool> noise(len / 4, false);
            for (size_t o = 0; o + 4 <= len; o += 4)
                if (*(const unsigned*)(a.data() + o) != *(const unsigned*)(b.data() + o)) noise[o / 4] = true;
            g_noise[ki].emplace(key, std::move(noise));
            continue;
        }
        const std::vector<bool>& noise = it->second;
        for (size_t o = 0; o + 4 <= len; o += 4) {
            unsigned r1 = *(const unsigned*)(a.data() + o), r2 = *(const unsigned*)(b.data() + o);
            if (r1 == r2 || (o / 4 < noise.size() && noise[o / 4]) || InPointer(a, b, o)) continue;
            // The world block and the timer also hold this PC's view, input
            // and interface state: logged, but only the simulated kinds and
            // the citizen split decide the result.
            if (!g_firstDifference && ki != KIND_WORLD && ki != KIND_TIMER) g_firstDifference = step;
            if (g_watched[ki].count((unsigned)o)) continue;
            g_watched[ki][(unsigned)o] = true;
            if (g_watchLines[ki] >= WATCH_LINES) continue;
            g_watchLines[ki]++;
            char where[64];
            if (!k.listOffset && !k.rva && o / 4 >= AGENT_LISTS + 1 + AGENT_MEMBERS) // agents: a citizen's list and position
                _snprintf_s(where, sizeof(where), _TRUNCATE, "agents: list of citizen #%u after step %llu",
                            (unsigned)(o / 4 - (AGENT_LISTS + 1 + AGENT_MEMBERS)), step);
            else
                _snprintf_s(where, sizeof(where), _TRUNCATE, "%s #%zu +0x%X after step %llu", k.name, i, (unsigned)o, step);
            const unsigned* before = i < p.size() && o + 4 <= p[i].size() ? (const unsigned*)(p[i].data() + o) : nullptr;
            LogDword(where, r1, r2, before);
        }
    }
}

// A citizen born at the address of one that died is a new object: its load
// leftovers (heap contents) are taken afresh at its first comparison.
static std::unordered_set<BYTE*> g_previousAddresses[KINDS];

static void ForgetReusedAddresses(int ki)
{
    std::unordered_set<BYTE*> now(g_addresses[ki].begin(), g_addresses[ki].end());
    if (!g_previousAddresses[ki].empty())
        for (BYTE* a : now)
            if (!g_previousAddresses[ki].count(a)) g_noise[ki].erase((unsigned long long)a);
    g_previousAddresses[ki].swap(now);
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void ProbeTrace_Reset()
{
    if (g_traceFile != INVALID_HANDLE_VALUE) CloseHandle(g_traceFile);
    g_traceFile = INVALID_HANDLE_VALUE;
    g_fileAt.clear(); g_fileSizes.clear(); g_fileEnd = 0;
    std::vector<BYTE>().swap(g_stream);
    std::vector<size_t>().swap(g_stepAt);
    for (int ki = 0; ki < KINDS; ki++) {
        std::vector<Blob>().swap(g_last[ki]);
        std::vector<Blob>().swap(g_mirror[ki]);
        std::vector<Blob>().swap(g_cur[ki]);
        std::vector<Blob>().swap(g_prev[ki]);
        g_noise[ki].clear();
        g_watched[ki].clear();
        g_previousAddresses[ki].clear();
        g_watchLines[ki] = 0;
    }
    g_full = g_reported = g_stopped = false;
    g_firstDifference = 0;
    g_hotLines = 0;
    g_steps2 = 0;
}

void ProbeTrace_Step(int run, unsigned long long step)
{
    if (run == 0) {
        if (g_full) return;
        if (g_disk) {
            if (g_traceFile == INVALID_HANDLE_VALUE && !OpenTraceFile()) {
                g_full = true;
                Logf("republicmp  PROBE TRACE: temporary file could not be opened - trace incomplete");
                return;
            }
            g_stream.clear();
        }
        if (g_stream.size() > TRACE_BUDGET) {
            g_full = true;
            Logf("republicmp  PROBE TRACE: kept for the first %llu steps only (memory budget)", step - 1);
            return;
        }
        g_stepAt.push_back(g_stream.size());
        MapCitizens();
        for (int ki = 0; ki < KINDS; ki++) {
            std::vector<Blob> cur;
            CopyAll(ki, cur);
            Record(ki, cur);
        }
        if (g_disk) {
            if (g_stream.size() > MAXDWORD || !TraceFileChunk(true, g_fileEnd, (DWORD)g_stream.size())) {
                g_full = true;
                Logf("republicmp  PROBE TRACE: temporary write failed - trace incomplete");
                return;
            }
            g_fileAt.push_back(g_fileEnd);
            g_fileSizes.push_back((DWORD)g_stream.size());
            g_fileEnd += g_stream.size();
        }
        return;
    }
    if (g_stopped) return;
    if (step == 1)
        for (int ki = 0; ki < KINDS; ki++) {
            std::vector<Blob>().swap(g_last[ki]); // run 1's working copies are not needed any more
            g_mirror[ki].clear();
            g_prev[ki].clear();
        }
    if (step - 1 >= g_stepAt.size()) {
        g_stopped = true;
        return;
    }
    if (g_disk) {
        size_t i = (size_t)(step - 1);
        if (i >= g_fileAt.size()) { g_stopped = true; return; }
        g_stream.resize(g_fileSizes[i]);
        if (!TraceFileChunk(false, g_fileAt[i], g_fileSizes[i])) {
            g_stopped = true;
            Logf("republicmp  PROBE TRACE: temporary read failed - trace incomplete");
            return;
        }
    }
    g_steps2 = step;
    Replay(step, g_mirror);
    MapCitizens();
    int differing = 0;
    for (int ki = 0; ki < KINDS; ki++) {
        CopyAll(ki, g_cur[ki]);
        if (ki == KIND_CITIZEN) ForgetReusedAddresses(ki);
        if (kKinds[ki].watch || g_reported) Watch(ki, step); // after the first report, a timeline
        else differing += Compare(ki, false, step);
    }
    if (step == 1)
        for (int ki = 0; ki < KINDS; ki++) LogNoise(ki);
    if (differing) {
        if (!g_firstDifference) g_firstDifference = step;
        for (int ki = 0; ki < KINDS; ki++)
            if (!kKinds[ki].watch) Compare(ki, true, step);
        g_reported = true; // from here on every kind is watched to the end
    }
    for (int ki = 0; ki < KINDS; ki++) g_prev[ki].swap(g_cur[ki]);
}

bool ProbeTrace_Complete(unsigned long long steps) { return !g_full && !g_stopped && g_steps2 == steps; }
bool ProbeTrace_Different() { return g_firstDifference != 0; }
unsigned long long ProbeTrace_FirstDifference() { return g_firstDifference; }

void ProbeTrace_Finish()
{
    if (g_full || g_stopped)
        Logf("republicmp  PROBE TRACE: incomplete, compared %llu steps", g_steps2);
    else if (!g_firstDifference)
        Logf("republicmp  PROBE TRACE: vehicles, buildings, citizens, lists and agents identical after every one of %llu traced step(s)",
             g_steps2);
    ProbeTrace_Reset();
}
