// SPDX-License-Identifier: GPL-3.0-only
// See building_switches.h. The comparison follows SoylentGreed's interface
// switches (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "building_switches.h"
#include "commands.h"
#include "world_refs.h"
#include "../session/settings_sync.h"
#include "../session/two_states.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <string.h>
#include <vector>

#define BUILDINGS_OFFSET WORLD_BUILDINGS_OFFSET
#define CONSTRUCTIONS_OFFSET WORLD_CONSTRUCTIONS_OFFSET
#define DOWN_RVA 0xA54E90
#define PRESS_RVA 0xA54E91
#define ENTER_RVA 0xA54CAC
#define KEYPAD_ENTER_RVA 0xA54D2C
#define BUILDING_BYTES BUILDING_STRUCT_BYTES
#define SLOTS_OFFSET BLD_STORAGE_OFFSET
#define SLOT_BYTES BLD_STORAGE_SLOT_SIZE
#define SLOT_STRIDE 0x4000   // a slot word's offset: slot * 0x4000 + region * 0x1000 + offset in the region
#define REGION_STRIDE 0x1000
#define MAX_SLOTS 64
#define MAX_ENTRIES 0xFF     // per slot vector (0x10 bytes each, or 4 for the +30 words)
#define MAX_BUILDINGS 16384
#define MAX_WORDS 60
#define NAME_AT 0x114        // a building's name (0x40 characters): text, never heap links
#define NAME_END (0x114 + 0x80)
#define SAMPLE_EVERY 16

enum { KIND_BUILDING = 0, KIND_SLOT = 1, KIND_WORLD = 2 };
static const char* const kKindText[3] = { "building", "storage slot", "world setting" };

// A slot's regions: 0 its own bytes, 1 the +00 entries, 2 the +18 entries, 3 the +30 words.
static const unsigned kRegionAt[4] = { 0, 0x00, 0x18, 0x30 };
static const unsigned kRegionEach[4] = { SLOT_BYTES, 0x10, 0x10, 4 };

// The bottom menu's world settings block (the load stores it at 430a7b..430ab0).
struct WorldRange {
    unsigned at, size;
};
static const WorldRange kWorldRanges[] = { { 0xAF8, 0x14 }, { 0xC798, 4 } };

// Building words that are this PC's view (camera visibility and distance,
// animation phase, ambience timer, piece index, the window's cost display)
// or std::vector headers / heap pointers inside BUILDING (from ~BUILDING at 164bb0):
// never a setting.
static const WorldRange kViewWords[] = {
    { 0x5B8, 0x18 },  { 0x600, 8 },     { 0x610, 0x150 }, { 0x848, 0x50 },
    { 0x970, 0xE8 },  { 0xB78, 0x18 },  { 0xBC0, 0x78 },  { 0xC70, 0x68 },
    { 0xD38, 0xA0 },  { 0xE60, 0x48 },  { 0xEE0, 0x30 },  { 0xF18, 0x108 },
    { 0x1030, 0x18 }, { 0x1090, 0x18 }, { 0x10C8, 0x18 }, { 0x1140, 0x18 },
    { 0x1158, 8 },    { 0x1168, 0x30 }, { 0x11D8, 0x18 }, { 0x1238, 0x30 },
    { 0x1320, 0x18 }, { 0x1344, 4 },    { 0x137C, 4 },    { 0x1388, 0x18 },
};

#pragma pack(push, 1)
struct SwitchWord {
    unsigned offset, old, now;
};
struct SwitchMsg {
    RouteRef building; // none for the world
    unsigned kind;
    unsigned shape;    // slot words: the slot count * 0x10000 + the region's element count
    unsigned count;
    SwitchWord words[MAX_WORDS];
};
#pragma pack(pop)

void BuildingSwitches_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }

static bool Copy(void* to, const void* from, size_t n)
{
    return SafeMemcpyPod(to, from, n);
}

static unsigned SlotCount(U64 building, U64* begin)
{
    U64 v[2] = {};
    *begin = 0;
    if (!Copy(v, (const void*)(building + SLOTS_OFFSET), 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) % SLOT_BYTES) return 0;
    U64 n = (v[1] - v[0]) / SLOT_BYTES;
    if (n > MAX_SLOTS) return 0;
    *begin = v[0];
    return (unsigned)n;
}

// A slot region: its address and element count (region 0: the slot itself, 1 element).
static bool Region(U64 slot, int region, U64* begin, unsigned* count)
{
    *begin = 0;
    *count = 0;
    if (region == 0) {
        *begin = slot;
        *count = 1;
        return true;
    }
    U64 v[2] = {};
    if (!Copy(v, (const void*)(slot + kRegionAt[region]), 16)) return false;
    if (!v[0] && !v[1]) return true;
    if (!v[0] || v[1] < v[0] || (v[1] - v[0]) % kRegionEach[region]) return false;
    U64 n = (v[1] - v[0]) / kRegionEach[region];
    if (n > MAX_ENTRIES) return false;
    *begin = v[0];
    *count = (unsigned)n;
    return true;
}

// One compared range: where it is now, what it was, how to name its words.
struct Range {
    U64 building;  // 0: the world
    int kind;
    unsigned base; // the message offset of its first byte
    U64 at;
    unsigned bytes;
    unsigned shape;
    size_t copy;   // its bytes in g_bytes
};

static std::vector<Range> g_ranges;
static std::vector<BYTE> g_bytes;
static bool g_taken = false, g_click = false;
static unsigned g_frame = 0;
static unsigned long long g_epoch = 0;
static std::vector<bool> g_noiseBuilding, g_noiseSlot[4], g_noiseWorld;
static int g_noiseLogged = 0, g_heapLogged = 0, g_leftLogged = 0;

// A building's category (type +0x360), for the log.
static int Category(U64 building)
{
    U64 type = 0;
    int category = -1;
    if (building && Copy(&type, (const void*)(building + 0x318), 8) && type) Copy(&category, (const void*)(type + 0x360), 4);
    return category;
}

static bool Clicked()
{
    return g_exeBase[DOWN_RVA] || g_exeBase[PRESS_RVA] || g_exeBase[ENTER_RVA] || g_exeBase[KEYPAD_ENTER_RVA];
}

static void Add(U64 building, int kind, unsigned base, U64 at, unsigned bytes, unsigned shape)
{
    if (!at || !bytes) return;
    Range r = { building, kind, base, at, bytes, shape, g_bytes.size() };
    g_bytes.resize(r.copy + bytes);
    if (Copy(g_bytes.data() + r.copy, (const void*)at, bytes)) g_ranges.push_back(r);
    else g_bytes.resize(r.copy);
}

static bool TakeSnapshot()
{
    g_ranges.clear();
    g_bytes.clear();
    const unsigned kOffsets[2] = { BUILDINGS_OFFSET, CONSTRUCTIONS_OFFSET };
    for (unsigned vecOff : kOffsets) {
        U64 v[2] = {};
        if (!Copy(v, World() + vecOff, 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) / 8 > MAX_BUILDINGS) continue;
        size_t n = (size_t)((v[1] - v[0]) / 8);
        std::vector<U64> list(n);
        if (n && !Copy(list.data(), (const void*)v[0], n * 8)) continue;
        for (U64 b : list) {
            if (!b) continue;
            Add(b, KIND_BUILDING, 0, b, BUILDING_BYTES, 0);
            U64 slots = 0;
            unsigned count = SlotCount(b, &slots);
            for (unsigned s = 0; s < count; s++) {
                U64 slot = slots + (U64)s * SLOT_BYTES;
                for (int r = 0; r < 4; r++) {
                    U64 at = 0;
                    unsigned elems = 0;
                    if (!Region(slot, r, &at, &elems)) continue;
                    Add(b, KIND_SLOT, s * SLOT_STRIDE + r * REGION_STRIDE, at, elems * kRegionEach[r], count * 0x10000 + elems);
                }
            }
        }
    }
    return true;
}

static bool PointerLike(U64 v)
{
    return IsPointerLikeX64(v);
}

static bool InRanges(const WorldRange* list, size_t n, unsigned offset)
{
    for (size_t i = 0; i < n; i++)
        if (offset + 4 > list[i].at && offset < list[i].at + list[i].size) return true;
    return false;
}

bool BuildingSwitches_IsExcludedBuildingOffset(unsigned offset)
{
    return InRanges(kViewWords, sizeof(kViewWords) / sizeof(kViewWords[0]), offset);
}

// Words this module leaves alone: the settings sync's own, and the view's.
static bool LeftOut(const Range& r, unsigned offset)
{
    if (r.kind == KIND_BUILDING)
        return SettingsSync_OwnsBuildingWord((const BYTE*)r.building, offset) ||
               InRanges(kViewWords, sizeof(kViewWords) / sizeof(kViewWords[0]), offset);
    if (r.kind == KIND_SLOT)
        return (((offset % SLOT_STRIDE) / REGION_STRIDE == 0) && (offset % REGION_STRIDE) < 0x48) ||
               SettingsSync_OwnsSlotWord((const BYTE*)r.building, offset / SLOT_STRIDE, (offset % SLOT_STRIDE) / REGION_STRIDE,
                                         offset % REGION_STRIDE);
    return false;
}

static std::vector<bool>& NoiseOf(const Range& r)
{
    if (r.kind == KIND_BUILDING) {
        if (g_noiseBuilding.empty()) g_noiseBuilding.assign(BUILDING_BYTES / 4, false);
        return g_noiseBuilding;
    }
    if (r.kind == KIND_WORLD) {
        if (g_noiseWorld.empty()) g_noiseWorld.assign(0x10000 / 4, false);
        return g_noiseWorld;
    }
    int region = (r.base % SLOT_STRIDE) / REGION_STRIDE;
    if (g_noiseSlot[region].empty()) g_noiseSlot[region].assign(REGION_STRIDE / 4, false);
    return g_noiseSlot[region];
}

// The word index noise is kept by: within one element for slot entries, within the object otherwise.
static unsigned NoiseIndex(const Range& r, unsigned inRange)
{
    if (r.kind == KIND_WORLD) return ((r.base + inRange) & 0xFFFF) / 4;
    if (r.kind == KIND_SLOT) return (inRange % kRegionEach[(r.base % SLOT_STRIDE) / REGION_STRIDE]) / 4;
    return inRange / 4;
}

static bool GuardedRef(U64 building, RouteRef& ref)
{
    bool ok = false;
    DiagLog_BeginSafeSeh();
    __try {
        ok = Ref_Building(building, ref);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DiagLog_EndSafeSeh();
    return ok;
}

static void Submit(const SwitchMsg& m)
{
    Commands_Submit(LS_CMD_BUILDING_SWITCH, &m, sizeof(m));
}

static void Compare(const Range& r, bool click)
{
    // Still the same place and shape (a slot vector may have been reallocated or resized).
    if (r.kind == KIND_SLOT) {
        U64 slots = 0;
        unsigned count = SlotCount(r.building, &slots);
        unsigned s = r.base / SLOT_STRIDE, region = (r.base % SLOT_STRIDE) / REGION_STRIDE;
        if (count != r.shape / 0x10000 || s >= count) return;
        U64 at = 0;
        unsigned elems = 0;
        if (!Region(slots + (U64)s * SLOT_BYTES, region, &at, &elems) || at != r.at || elems != (r.shape & 0xFFFF)) return;
    }
    std::vector<BYTE> cur(r.bytes);
    if (!Copy(cur.data(), (const void*)r.at, r.bytes)) return;
    const BYTE* old = g_bytes.data() + r.copy;
    if (memcmp(cur.data(), old, r.bytes) == 0) return;
    std::vector<bool>& noise = NoiseOf(r);

    SwitchMsg m;
    memset(&m, 0, sizeof(m));
    m.building = kNoRef;
    m.kind = r.kind;
    m.shape = r.shape;
    bool haveRef = r.kind == KIND_WORLD;
    for (unsigned q = 0; q + 4 <= r.bytes; q += 4) {
        unsigned wa = *(const unsigned*)(old + q), wb = *(const unsigned*)(cur.data() + q);
        if (wa == wb) continue;
        unsigned offset = r.base + q;
        unsigned word = NoiseIndex(r, q);
        if (LeftOut(r, offset)) {
            if (click && g_leftLogged < 40) {
                ++g_leftLogged;
                Logf("republicmp  SWITCH: a click changed %s +0x%X (category 0x%X) - left to the building settings sync",
                     kKindText[r.kind], offset, Category(r.building));
            }
            continue;
        }
        if (!click) {
            if (word < noise.size() && !noise[word]) {
                noise[word] = true;
                if (g_noiseLogged < 40) {
                    ++g_noiseLogged;
                    Logf("republicmp  SWITCH: the interface changes %s +0x%X without a click - left out", kKindText[r.kind], offset);
                }
            }
            continue;
        }
        if (word < noise.size() && noise[word]) continue;
        // A heap address in the aligned pair (names are text).
        unsigned pair = q & ~7u;
        bool text = r.kind == KIND_BUILDING && offset >= NAME_AT && offset < NAME_END;
        if (!text && pair + 8 <= r.bytes &&
            (PointerLike(*(const U64*)(old + pair)) || PointerLike(*(const U64*)(cur.data() + pair)))) {
            if (g_heapLogged < 20) {
                ++g_heapLogged;
                Logf("republicmp  SWITCH: a click changed %s +0x%X to a heap address - not carried", kKindText[r.kind], offset);
            }
            continue;
        }
        if (!haveRef) {
            int bldId = 0;
            float bx = 0.0f, bz = 0.0f;
            Copy(&bldId, (const void*)r.building, 4);
            Copy(&bx, (const void*)(r.building + 0x350), 4);
            Copy(&bz, (const void*)(r.building + 0x358), 4);
            if (!TwoStates_CheckBuildingControlAllowed(bldId, bx, bz, "изменение настроек чужого здания")) {
                Copy((void*)r.at, old, r.bytes);
                return;
            }
            haveRef = GuardedRef(r.building, m.building);
            if (!haveRef) {
                Logf("republicmp  SWITCH: a click changed a building that is not in the world's list - not carried");
                return;
            }
        }
        // Put back at once: the clicking PC's simulation never sees it before the command.
        *(unsigned*)(r.at + q) = wa;
        m.words[m.count++] = { offset, wa, wb };
        if (m.count == MAX_WORDS) {
            Submit(m);
            m.count = 0;
        }
    }
    if (m.count) Submit(m);
}

// ---------------------------------------------------------------------------
// The world settings block
// ---------------------------------------------------------------------------
//
// The bottom menu keeps its own copy of these settings and writes it into the
// world every frame its page shows, so a PC whose menu still held an older
// value wrote that back after the other PC's command (run: +b08 read neither
// value on the player, and the host's menu rewrote it until it was taken for
// the interface's noise). The session's value - the last one ordered - is the
// authority: every interface frame a different value is put back, and only a
// click that writes a new value (not the one the menu kept writing before the
// click) becomes a command.
#define WORLD_WORDS 6
static unsigned g_worldAt[WORLD_WORDS];
static int g_worldCount = 0;
static unsigned g_auth[WORLD_WORDS], g_stale[WORLD_WORDS], g_sent[WORLD_WORDS];
static bool g_sentValid[WORLD_WORDS], g_authReady = false, g_worldFrame = false;

void BuildingSwitches_Reset()
{
    g_taken = false;
    g_worldFrame = false;
    g_authReady = false;
    g_ranges.clear();
    g_bytes.clear();
    memset(g_sentValid, 0, sizeof(g_sentValid));
}

static void WorldWords()
{
    if (g_worldCount) return;
    for (const WorldRange& w : kWorldRanges)
        for (unsigned at = w.at; at + 4 <= w.at + w.size && g_worldCount < WORLD_WORDS; at += 4) g_worldAt[g_worldCount++] = at;
}

static bool ReadWorldSetting(BYTE* world, unsigned at, unsigned& out)
{
    out = 0;
    if (at == 0xB08) {
        BYTE b = 0;
        if (!Copy(&b, world + at, 1)) return false;
        out = (unsigned)b;
        return true;
    }
    return Copy(&out, world + at, 4);
}

static void WriteWorldSetting(BYTE* world, unsigned at, unsigned val)
{
    if (at == 0xB08) {
        BYTE b = (BYTE)(val & 0xFF);
        Copy(world + at, &b, 1);
        return;
    }
    Copy(world + at, &val, 4);
}

static void WorldFrame(bool click)
{
    WorldWords();
    BYTE* world = World();
    if (!g_authReady) {
        // The session's start: both PCs loaded the same save.
        for (int i = 0; i < g_worldCount; i++) {
            ReadWorldSetting(world, g_worldAt[i], g_auth[i]);
            g_stale[i] = g_auth[i];
            g_sentValid[i] = false;
        }
        g_authReady = true;
        return;
    }
    SwitchMsg m;
    memset(&m, 0, sizeof(m));
    m.building = kNoRef;
    m.kind = KIND_WORLD;
    for (int i = 0; i < g_worldCount; i++) {
        unsigned cur = 0;
        if (!ReadWorldSetting(world, g_worldAt[i], cur) || cur == g_auth[i]) continue;
        if (click && cur != g_stale[i] && !(g_sentValid[i] && g_sent[i] == cur)) {
            m.words[m.count++] = { g_worldAt[i], g_auth[i], cur };
            g_sent[i] = cur;
            g_sentValid[i] = true;
        } else if (!click) {
            g_stale[i] = cur;
        }
        unsigned auth = g_auth[i];
        WriteWorldSetting(world, g_worldAt[i], auth);
    }
    if (m.count) Submit(m);
}

void BuildingSwitches_Before(bool simBusy)
{
    g_taken = false;
    g_worldFrame = false;
    if (!Commands_Capturing()) g_authReady = false; // the next session takes its own start
    if (!Commands_TakeUi() || simBusy) return;
    g_click = Clicked();
    g_epoch = Commands_ApplyEpoch();
    g_worldFrame = true;
    if (!g_click && (++g_frame % SAMPLE_EVERY) != 0) return;
    g_taken = TakeSnapshot();
}

void BuildingSwitches_After(bool simBusy)
{
    // A command made meanwhile (another thread) is not the interface's change.
    bool same = !simBusy && Commands_TakeUi() && Commands_ApplyEpoch() == g_epoch;
    if (g_worldFrame && same) WorldFrame(g_click);
    g_worldFrame = false;
    if (!g_taken) return;
    g_taken = false;
    if (!same) return;
    for (const Range& r : g_ranges) Compare(r, g_click);
}

// The address of a carried word on this PC (0: not there, or another shape).
static U64 WordAddress(U64 building, const SwitchMsg& m, unsigned offset)
{
    if (m.kind == KIND_WORLD) {
        for (const WorldRange& w : kWorldRanges)
            if (offset >= w.at && offset + 4 <= w.at + w.size) return (U64)World() + offset;
        return 0;
    }
    if (m.kind == KIND_BUILDING) return offset + 4 <= BUILDING_BYTES ? building + offset : 0;
    U64 slots = 0;
    unsigned count = SlotCount(building, &slots);
    unsigned s = offset / SLOT_STRIDE, region = (offset % SLOT_STRIDE) / REGION_STRIDE, local = offset % REGION_STRIDE;
    if (region > 3 || count != m.shape / 0x10000 || s >= count) return 0;
    U64 at = 0;
    unsigned elems = 0;
    if (!Region(slots + (U64)s * SLOT_BYTES, region, &at, &elems) || elems != (m.shape & 0xFFFF)) return 0;
    return local + 4 <= elems * kRegionEach[region] ? at + local : 0;
}

bool BuildingSwitches_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SwitchMsg)) return false;
    SwitchMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.kind > KIND_WORLD || !m.count || m.count > MAX_WORDS) return false;
    const char* why = nullptr;
    unsigned differing = 0;
    DiagLog_BeginSafeSeh();
    __try {
        U64 building = m.kind == KIND_WORLD ? 0 : Ref_BuildingAt(m.building);
        if (m.kind != KIND_WORLD && !building) why = "the building is gone";
        for (unsigned i = 0; i < m.count && !why; i++) {
            unsigned off = m.words[i].offset;
            if (off % 4 || !WordAddress(building, m, off)) {
                why = "a word is not there (another shape here)";
                break;
            }
            if (m.kind != KIND_WORLD) {
                Range checkR = { building, (int)m.kind, 0, 0, 0, m.shape, 0 };
                if (LeftOut(checkR, off)) {
                    why = "a word is in a view/vector range";
                    break;
                }
                bool text = m.kind == KIND_BUILDING && off >= NAME_AT && off < NAME_END;
                U64 pairAddr = WordAddress(building, m, off & ~7u);
                if (!text && pairAddr && WordAddress(building, m, (off & ~7u) + 4) == pairAddr + 4 &&
                    PointerLike(*(const U64*)pairAddr)) {
                    why = "a word overlaps a heap pointer here";
                    break;
                }
            }
        }
        for (unsigned i = 0; i < m.count && !why; i++) {
            unsigned off = m.words[i].offset;
            if (m.kind == KIND_WORLD && off == 0xB08) {
                BYTE* pb = (BYTE*)WordAddress(building, m, off);
                if (!pb) continue;
                unsigned curVal = *pb;
                if (curVal != (m.words[i].old & 0xFF) && curVal != (m.words[i].now & 0xFF)) differing++;
                *pb = (BYTE)(m.words[i].now & 0xFF);
            } else {
                unsigned* p = (unsigned*)WordAddress(building, m, off);
                if (!p) continue;
                if (*p != m.words[i].old && *p != m.words[i].now) differing++;
                *p = m.words[i].now;
            }
            if (m.kind == KIND_WORLD) // the session's value from now on
                for (int k = 0; k < g_worldCount; k++)
                    if (g_worldAt[k] == off) {
                        g_auth[k] = off == 0xB08 ? (m.words[i].now & 0xFF) : m.words[i].now;
                        g_sentValid[k] = false;
                    }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while writing";
    }
    DiagLog_EndSafeSeh();
    Logf("republicmp  SWITCH: %s%s (category 0x%X), %u word(s) from +0x%X%s: %s", kKindText[m.kind],
         m.kind == KIND_WORLD ? "" : " of a building", m.kind == KIND_WORLD ? 0 : Category(Ref_BuildingAt(m.building)), m.count,
         m.words[0].offset, differing ? " (some read neither value)" : "", why ? why : "made");
    return why == nullptr;
}
