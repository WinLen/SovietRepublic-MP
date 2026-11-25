// SPDX-License-Identifier: GPL-3.0-only
// See building_switches.h. The comparison follows SoylentGreed's interface
// switches (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "building_switches.h"
#include "commands.h"
#include "switch_common.h"
#include "world_refs.h"
#include "../session/settings_sync.h"
#include "../session/save_sync.h"
#include "../session/two_states.h"
#include "../ui/lang.h"
#include "../ui/native_menu.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <algorithm>
#include <math.h>
#include <string.h>
#include <vector>

#define BUILDINGS_OFFSET WORLD_BUILDINGS_OFFSET
#define CONSTRUCTIONS_OFFSET WORLD_CONSTRUCTIONS_OFFSET
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

enum { KIND_BUILDING = 0, KIND_SLOT = 1, KIND_WORLD = 2, KIND_MANUAL_BUY = 3, KIND_UNIV_RESEARCH = 4, KIND_AUTO_BUY = 5 };
static const char* const kKindText[6] = {
    "building", "storage slot", "world setting", "manual resource purchase", "university research", "auto resource purchase"
};
#define AUTO_BUY_DISABLE_FLAG 0x100u

// A slot's regions: 0 its own bytes, 1 the +00 entries, 2 the +18 entries, 3 the +30 words.
static const unsigned kRegionAt[4] = { 0, 0x00, 0x18, 0x30 };
static const unsigned kRegionEach[4] = { SLOT_BYTES, 0x10, 0x10, 4 };

// The bottom menu's world settings block (the load stores it at 430a7b..430ab0).
struct WorldRange {
    unsigned at, size;
};
static const WorldRange kWorldRanges[] = { { 0xAF8, 0x10 }, { 0xB08, 4 }, { 0xC798, 4 } };

// Building words that are this PC's view (camera visibility and distance,
// animation phase, ambience timer, piece index, the window's cost display)
// or std::vector headers / heap pointers inside BUILDING (from ~BUILDING at 164bb0):
// never a setting.
static const WorldRange kViewWords[] = {
    { 0x5B8, 0x20 },  { 0x5E0, 0x18 },  { 0x600, 8 },     { 0x610, 0x150 }, { 0x848, 0x68 },
    { 0x970, 0x78 },  { 0x9F8, 0x240 },
    { 0xC70, 0x68 },  { 0xD38, 0xC4 },  { 0xE60, 0x48 },  { 0xEE0, 0x30 },
    { 0xF18, 0xB0 },  { 0xFCC, 0x54 },  { 0x1030, 0x18 }, { 0x1090, 0x18 }, { 0x10C8, 0x18 },
    { 0x1140, 0x18 }, { 0x1158, 8 },    { 0x1168, 0x30 }, { 0x11D8, 0x18 },
    { 0x1238, 0x30 }, { 0x1308, 8 },    { 0x1320, 0x18 }, { 0x1344, 4 },    { 0x137C, 4 },
    { 0x1388, 0x18 },
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
static std::vector<SwitchSpan> g_snapSpans;
static std::vector<U64> g_listScratch;
static std::vector<BYTE> g_bytes;
static std::vector<BYTE> g_cmpScratch;
static bool g_taken = false, g_click = false;
static SwitchBackoffState g_backoff = { 0, SWITCH_SAMPLE_EVERY_BASE, 0, { 0, 0, 0, false } };
static unsigned long long g_epoch = 0;
static unsigned long long g_snapshotCount = 0;
static unsigned long long g_mismatchCopyCount = 0;
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

static void ScanBuildingRangesBatchPod(
    const U64* list, size_t count, volatile size_t* ioIdx,
    Range* outRanges, SwitchSpan* outSpans, size_t maxRanges,
    size_t* ioRangeCount, size_t* ioTotalBytes)
{
    const size_t maxPerBld = 1u + (size_t)MAX_SLOTS * 4u;
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    volatile size_t cur = idx;
    size_t rc = *ioRangeCount;
    size_t tb = *ioTotalBytes;
    __try {
        for (; idx < count; ++idx) {
            cur = idx;
            if (rc + maxPerBld > maxRanges) break;
            const U64 b = list[idx];
            if (!b) continue;
            outRanges[rc] = { b, KIND_BUILDING, 0, b, BUILDING_BYTES, 0, tb };
            outSpans[rc] = { (const void*)b, tb, BUILDING_BYTES };
            ++rc;
            tb += BUILDING_BYTES;
            const U64* sv = (const U64*)(b + SLOTS_OFFSET);
            const U64 sb = sv[0];
            const U64 se = sv[1];
            if (!sb || se < sb || ((se - sb) % SLOT_BYTES) != 0) continue;
            const unsigned slotCount = (unsigned)((se - sb) / SLOT_BYTES);
            if (slotCount > MAX_SLOTS) continue;
            for (unsigned s = 0; s < slotCount; ++s) {
                const U64 slot = sb + (U64)s * SLOT_BYTES;
                for (int r = 0; r < 4; ++r) {
                    U64 at = 0;
                    unsigned elems = 0;
                    if (r == 0) {
                        at = slot;
                        elems = 1;
                    } else {
                        const U64* rv = (const U64*)(slot + kRegionAt[r]);
                        const U64 rb = rv[0];
                        const U64 re = rv[1];
                        if (!rb || re < rb || ((re - rb) % kRegionEach[r]) != 0) continue;
                        const U64 n = (re - rb) / kRegionEach[r];
                        if (n == 0 || n > MAX_ENTRIES) continue;
                        at = rb;
                        elems = (unsigned)n;
                    }
                    const unsigned bytes = elems * kRegionEach[r];
                    if (!at || !bytes) continue;
                    outRanges[rc] = { b, KIND_SLOT, s * SLOT_STRIDE + (unsigned)r * REGION_STRIDE,
                                      at, bytes, slotCount * 0x10000u + elems, tb };
                    outSpans[rc] = { (const void*)at, tb, bytes };
                    ++rc;
                    tb += bytes;
                }
            }
        }
        *ioIdx = idx;
        *ioRangeCount = rc;
        *ioTotalBytes = tb;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioRangeCount = rc;
        *ioTotalBytes = tb;
    }
    DiagLog_EndSafeSeh();
}

static bool TakeSnapshot()
{
    g_ranges.clear();
    g_snapSpans.clear();
    g_bytes.clear();
    size_t rangeCount = 0;
    size_t totalBytes = 0;
    const size_t minHeadroom = 1u + (size_t)MAX_SLOTS * 4u;
    const unsigned kOffsets[2] = { BUILDINGS_OFFSET, CONSTRUCTIONS_OFFSET };
    for (unsigned vecOff : kOffsets) {
        bool wasNonEmpty = false;
        if (!SwitchCommon_ReadPtrVector(World() + vecOff, MAX_BUILDINGS, g_listScratch, &wasNonEmpty) ||
            !wasNonEmpty) {
            continue;
        }
        const size_t n = g_listScratch.size();
        volatile size_t idx = 0;
        while (idx < n) {
            if (g_ranges.size() < rangeCount + minHeadroom) {
                const size_t rem = n - idx;
                const size_t want = rangeCount + (std::max)(minHeadroom, rem * 4u);
                g_ranges.resize(want);
                g_snapSpans.resize(want);
            }
            ScanBuildingRangesBatchPod(
                g_listScratch.data(), n, &idx,
                g_ranges.data(), g_snapSpans.data(), g_ranges.size(),
                &rangeCount, &totalBytes);
        }
    }
    g_ranges.resize(rangeCount);
    g_snapSpans.resize(rangeCount);
    if (totalBytes > 0) {
        g_bytes.resize(totalBytes);
        SwitchCommon_CopySpanBatchPod(g_snapSpans.data(), rangeCount, g_bytes.data());
    }
    size_t writeIdx = 0;
    for (size_t i = 0; i < rangeCount; ++i) {
        if (g_snapSpans[i].bytes != 0) {
            if (writeIdx != i) g_ranges[writeIdx] = g_ranges[i];
            ++writeIdx;
        }
    }
    g_ranges.resize(writeIdx);
    ++g_snapshotCount;
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

static bool IsLeftOutSlotDynamicOffset(unsigned offset, bool click = false)
{
    unsigned region = (offset % SLOT_STRIDE) / REGION_STRIDE;
    unsigned local = offset % REGION_STRIDE;
    if (region == 0 && local < 0x48) return true;
    if (region == 1 && (local % 16) == 8) return !click;
    return false;
}

bool BuildingSwitches_IsLeftOutSlotOffsetForTest(unsigned offset, bool click)
{
    return IsLeftOutSlotDynamicOffset(offset, click);
}

void BuildingSwitches_RevertBuildingName(unsigned long long building)
{
    if (!g_taken || !building) return;
    for (const Range& r : g_ranges) {
        if (r.kind == KIND_BUILDING && r.building == building && r.bytes >= NAME_END &&
            r.copy + NAME_END <= g_bytes.size()) {
            Copy((void*)(building + NAME_AT), g_bytes.data() + r.copy + NAME_AT, NAME_END - NAME_AT);
            break;
        }
    }
}

static bool IsNonPointerBuildingWord(int kind, unsigned offset)
{
    if (kind != KIND_BUILDING) return false;
    return offset == BLD_SEARCH_RANGE_OFFSET ||
           (offset >= NAME_AT && offset < NAME_END) ||
           (offset >= BLD_CITIZEN_FILTER_AT && offset < BLD_CITIZEN_FILTER_END) ||
           (offset >= BLD_COPY_WEIGHTS_BEGIN_OFFSET && offset < BLD_COPY_WEIGHTS_END_OFFSET);
}

static bool IsNeverNoiseWord(const Range& r, unsigned offset)
{
    if (r.kind == KIND_BUILDING)
        return offset == BLD_AUTO_BUY_MODE_OFFSET || offset == BLD_EXPORT_MODE_OFFSET ||
               offset == BLD_UNIV_STAFF_RATIO_OFFSET || offset == BLD_SEARCH_RANGE_OFFSET ||
               IsNonPointerBuildingWord(r.kind, offset);
    if (r.kind == KIND_SLOT)
        return ((offset % SLOT_STRIDE) / REGION_STRIDE == 1) &&
               (((offset % 16) == 0x0C) || ((offset % 16) == 8));
    return false;
}

// Words this module leaves alone: the settings sync's own, and the view's.
static bool LeftOut(const Range& r, unsigned offset, bool click = false)
{
    if (r.kind == KIND_BUILDING)
        return SettingsSync_OwnsBuildingWord((const BYTE*)r.building, offset) ||
               InRanges(kViewWords, sizeof(kViewWords) / sizeof(kViewWords[0]), offset);
    if (r.kind == KIND_SLOT)
        return IsLeftOutSlotDynamicOffset(offset, click) ||
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

static bool CheckBuildingControlAllowed(U64 building, const char* actionDesc)
{
    int bldId = 0;
    float bx = 0.0f, bz = 0.0f;
    Copy(&bldId, (const void*)building, 4);
    Copy(&bx, (const void*)(building + OBJ_POS_OFFSET), 4);
    Copy(&bz, (const void*)(building + OBJ_POS_OFFSET + 8), 4);
    return TwoStates_CheckBuildingControlAllowed(bldId, bx, bz, actionDesc);
}

static void Submit(const SwitchMsg& m)
{
    Commands_Submit(LS_CMD_BUILDING_SWITCH, &m, sizeof(m));
}

enum { kRangeScanDone = 0, kRangeScanChanged = 1, kRangeScanFaulted = 2 };

static bool VerifySlotRangeLivePod(const Range& r)
{
    const U64* sv = (const U64*)(r.building + SLOTS_OFFSET);
    const U64 sb = sv[0];
    const U64 se = sv[1];
    if (!sb || se < sb || ((se - sb) % SLOT_BYTES) != 0) return false;
    const U64 count = (se - sb) / SLOT_BYTES;
    const unsigned s = r.base / SLOT_STRIDE;
    const unsigned region = (r.base % SLOT_STRIDE) / REGION_STRIDE;
    if (count > MAX_SLOTS || count != r.shape / 0x10000u || s >= count || region > 3u) return false;
    if (region == 0) {
        return (sb + (U64)s * SLOT_BYTES) == r.at && (r.shape & 0xFFFFu) == 1u;
    }
    const U64 slot = sb + (U64)s * SLOT_BYTES;
    const U64* rv = (const U64*)(slot + kRegionAt[region]);
    const U64 rb = rv[0];
    const U64 re = rv[1];
    if (!rb && !re) return r.at == 0 && (r.shape & 0xFFFFu) == 0;
    if (!rb || re < rb || ((re - rb) % kRegionEach[region]) != 0) return false;
    const U64 elems = (re - rb) / kRegionEach[region];
    return elems <= MAX_ENTRIES && rb == r.at && elems == (U64)(r.shape & 0xFFFFu);
}

static int FindNextChangedRangePod(const Range* ranges, size_t n, size_t* ioIdx, const BYTE* bytesBase)
{
    if (!ranges || !ioIdx || !bytesBase) return kRangeScanDone;
    size_t idx = *ioIdx;
    if (idx >= n) return kRangeScanDone;
    int result = kRangeScanDone;
    volatile size_t cur = idx;
    DiagLog_BeginSafeSeh();
    __try {
        for (; idx < n; ++idx) {
            cur = idx;
            const Range& r = ranges[idx];
            if (r.kind == KIND_SLOT && !VerifySlotRangeLivePod(r)) continue;
            if (memcmp((const void*)r.at, bytesBase + r.copy, r.bytes) != 0) {
                *ioIdx = idx;
                result = kRangeScanChanged;
                break;
            }
        }
        if (result == kRangeScanDone) {
            *ioIdx = n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        result = kRangeScanFaulted;
    }
    DiagLog_EndSafeSeh();
    return result;
}

static bool CompareChangedRange(const Range& r, bool click)
{
    if (!SwitchCommon_CopyMismatchToScratch((const void*)r.at, r.bytes, g_cmpScratch)) return false;
    ++g_mismatchCopyCount;
    const BYTE* cur = g_cmpScratch.data();
    const BYTE* old = g_bytes.data() + r.copy;
    std::vector<bool>& noise = NoiseOf(r);

    SwitchMsg m;
    memset(&m, 0, sizeof(m));
    m.building = kNoRef;
    m.kind = r.kind;
    m.shape = r.shape;
    bool haveRef = r.kind == KIND_WORLD;
    bool anyEmittedOrLearned = false;
    for (unsigned q = 0; q + 4 <= r.bytes; q += 4) {
        unsigned wa = *(const unsigned*)(old + q), wb = *(const unsigned*)(cur + q);
        if (wa == wb) continue;
        unsigned offset = r.base + q;
        unsigned word = NoiseIndex(r, q);
        if (LeftOut(r, offset, click)) {
            if (click && g_leftLogged < 40) {
                ++g_leftLogged;
                Logf("republicmp  SWITCH: a click changed %s +0x%X (category 0x%X) - left to the building settings sync",
                     kKindText[r.kind], offset, Category(r.building));
            }
            continue;
        }
        bool neverNoise = IsNeverNoiseWord(r, offset);
        if (!click) {
            if (!neverNoise && word < noise.size() && !noise[word]) {
                noise[word] = true;
                anyEmittedOrLearned = true;
                if (g_noiseLogged < 40) {
                    ++g_noiseLogged;
                    Logf("republicmp  SWITCH: the interface changes %s +0x%X without a click - left out", kKindText[r.kind], offset);
                }
            }
            continue;
        }
        if (!neverNoise && word < noise.size() && noise[word]) continue;
        // A heap address in the aligned pair (names and citizen/car filters are non-pointer fields).
        unsigned pair = q & ~7u;
        bool nonPtrPair = IsNonPointerBuildingWord(r.kind, offset);
        if (!nonPtrPair && pair + 8 <= r.bytes &&
            (PointerLike(*(const U64*)(old + pair)) || PointerLike(*(const U64*)(cur + pair)))) {
            if (g_heapLogged < 20) {
                ++g_heapLogged;
                Logf("republicmp  SWITCH: a click changed %s +0x%X to a heap address - not carried", kKindText[r.kind], offset);
            }
            SafeWritePod((void*)(r.at + q), wa);
            anyEmittedOrLearned = true;
            continue;
        }
        if (!haveRef) {
            if (!CheckBuildingControlAllowed(r.building, Tr("изменение настроек чужого здания", "changing foreign building settings"))) {
                Copy((void*)r.at, old, r.bytes);
                return true;
            }
            haveRef = SwitchCommon_GuardedRef(false, r.building, m.building);
            if (!haveRef) {
                Logf("republicmp  SWITCH: a click changed a building that is not in the world's list - not carried");
                return anyEmittedOrLearned;
            }
        }
        // Put back at once: the clicking PC's simulation never sees it before the command.
        SafeWritePod((void*)(r.at + q), wa);
        m.words[m.count++] = { offset, wa, wb };
        anyEmittedOrLearned = true;
        if (m.count == MAX_WORDS) {
            Submit(m);
            m.count = 0;
        }
    }
    if (m.count) Submit(m);
    return anyEmittedOrLearned;
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
#define WORLD_WORDS 12
static unsigned g_worldAt[WORLD_WORDS];
static int g_worldCount = 0;
static unsigned g_auth[WORLD_WORDS], g_stale[WORLD_WORDS], g_sent[WORLD_WORDS];
static bool g_sentValid[WORLD_WORDS], g_authReady = false, g_worldFrame = false;

typedef unsigned long long (*CopyWeightsToolFn)(unsigned long long world, unsigned long long a2);
static CopyWeightsToolFn g_testCopyWeightsOverride = nullptr;
static int g_copyWeightsReady = -1;

void BuildingSwitches_Reset()
{
    g_taken = false;
    g_worldFrame = false;
    g_authReady = false;
    g_copyWeightsReady = -1;
    g_ranges.clear();
    g_snapSpans.clear();
    g_listScratch.clear();
    g_bytes.clear();
    g_cmpScratch.clear();
    memset(g_sentValid, 0, sizeof(g_sentValid));
    g_noiseBuilding.clear();
    for (int i = 0; i < 4; ++i) g_noiseSlot[i].clear();
    g_noiseWorld.clear();
    g_noiseLogged = g_heapLogged = g_leftLogged = 0;
    SwitchBackoff_Reset(g_backoff);
    g_epoch = 0;
    g_snapshotCount = 0;
    g_mismatchCopyCount = 0;
}

static bool IsOneByteWorldSetting(unsigned at)
{
    return (at >= 0xB08 && at <= 0xB0B) || (at >= 0xC798 && at <= 0xC79B);
}

static void WorldWords()
{
    if (g_worldCount) return;
    for (const WorldRange& w : kWorldRanges) {
        unsigned at = w.at;
        while (at < w.at + w.size && g_worldCount < WORLD_WORDS) {
            if (IsOneByteWorldSetting(at)) {
                g_worldAt[g_worldCount++] = at;
                at += 1;
            } else {
                if (at + 4 <= w.at + w.size) {
                    g_worldAt[g_worldCount++] = at;
                }
                at += 4;
            }
        }
    }
}

static bool IsLocalPerStateWorldSetting(unsigned at)
{
    return TwoStates_IsActive() &&
           ((at >= 0xAF8 && at <= 0xB0B) ||
            (at >= 0xC798 && at <= 0xC79B));
}

static bool ReadWorldSetting(BYTE* world, unsigned at, unsigned& out)
{
    out = 0;
    if (IsOneByteWorldSetting(at)) {
        BYTE b = 0;
        if (!Copy(&b, world + at, 1)) return false;
        out = (unsigned)b;
        return true;
    }
    return Copy(&out, world + at, 4);
}

static void WriteWorldSetting(BYTE* world, unsigned at, unsigned val)
{
    if (IsOneByteWorldSetting(at)) {
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
        if (!ReadWorldSetting(world, g_worldAt[i], cur)) continue;
        if (IsLocalPerStateWorldSetting(g_worldAt[i])) {
            g_auth[i] = g_stale[i] = cur;
            g_sentValid[i] = false;
            continue;
        }
        if (cur == g_auth[i]) {
            g_stale[i] = cur;
            continue;
        }
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
    g_click = SwitchCommon_Clicked(g_exeBase);
    g_epoch = Commands_ApplyEpoch();
    g_worldFrame = true;
    if (!SwitchBackoff_ShouldSampleBefore(g_backoff, g_exeBase, g_click)) return;
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
    bool anyChange = false;
    const size_t total = g_ranges.size();
    size_t idx = 0;
    while (idx < total) {
        const int status = FindNextChangedRangePod(g_ranges.data(), total, &idx, g_bytes.data());
        if (status == kRangeScanDone) break;
        if (status == kRangeScanFaulted) continue;
        const size_t changedIdx = idx++;
        if (CompareChangedRange(g_ranges[changedIdx], g_click)) {
            anyChange = true;
        }
    }
    SwitchBackoff_RecordAfter(g_backoff, g_exeBase, g_click, anyChange);
}

// The address of a carried word on this PC (0: not there, or another shape).
static U64 WordAddress(U64 building, const SwitchMsg& m, unsigned offset)
{
    if (m.kind == KIND_WORLD) {
        for (const WorldRange& w : kWorldRanges) {
            unsigned need = IsOneByteWorldSetting(offset) ? 1 : 4;
            if (offset >= w.at && offset + need <= w.at + w.size) return (U64)World() + offset;
        }
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

static bool CaptureSliderAction(U64 world, U64 building, int currencyMode, unsigned kind, unsigned shape,
                                unsigned sliderOff, bool skipSliders, bool allowEmpty)
{
    if (!world || !building || (currencyMode != 3 && currencyMode != 4)) return false;
    RouteRef ref = kNoRef;
    if (!SwitchCommon_GuardedRef(false, building, ref)) return false;

    SwitchMsg m = {};
    m.building = ref;
    m.kind = kind;
    m.shape = shape;

    if (!skipSliders) {
        U64 v[2] = {};
        if (!Copy(v, (const void*)(world + WORLD_UI_RES_SLIDERS_OFFSET), 16)) return false;
        if (!v[0] && !v[1]) {
            if (!allowEmpty) return false;
        } else {
            if (!v[0] || v[1] < v[0] || (v[1] - v[0]) % SLOT_BYTES != 0) return false;
            U64 slotCount = (v[1] - v[0]) / SLOT_BYTES;
            if (slotCount > MAX_SLOTS) return false;
            for (unsigned s = 0; s < (unsigned)slotCount && m.count < MAX_WORDS; s++) {
                U64 uiSlot = v[0] + (U64)s * SLOT_BYTES;
                U64 e[2] = {};
                if (!Copy(e, (const void*)uiSlot, 16) || !e[0] || e[1] < e[0] || (e[1] - e[0]) % 0x10 != 0) continue;
                U64 entryCount = (e[1] - e[0]) / 0x10;
                if (entryCount > MAX_ENTRIES) continue;
                for (unsigned k = 0; k < (unsigned)entryCount && m.count < MAX_WORDS; k++) {
                    float tons = 0.0f;
                    if (!Copy(&tons, (const void*)(e[0] + (U64)k * 0x10 + sliderOff), 4)) continue;
                    if (!isfinite(tons) || tons <= 0.0f) continue;
                    unsigned floatBits = 0;
                    memcpy(&floatBits, &tons, 4);
                    unsigned off = s * SLOT_STRIDE + REGION_STRIDE + k * 0x10 + sliderOff;
                    m.words[m.count++] = { off, 0u, floatBits };
                }
            }
        }
    }
    if (!allowEmpty && m.count == 0) return false;
    Submit(m);
    return true;
}

bool BuildingSwitches_CaptureManualBuy(unsigned long long world, unsigned long long building, int currencyMode)
{
    return CaptureSliderAction(world, building, currencyMode, KIND_MANUAL_BUY, (unsigned)currencyMode, 0x08u, false, false);
}

bool BuildingSwitches_CaptureAutoBuy(unsigned long long world, unsigned long long building, int currencyMode, bool isDisable)
{
    unsigned shape = (unsigned)currencyMode | (isDisable ? AUTO_BUY_DISABLE_FLAG : 0u);
    return CaptureSliderAction(world, building, currencyMode, KIND_AUTO_BUY, shape, 0x0Cu, isDisable, true);
}

static void WriteSlidersFromMsg(BYTE* uiVecAddr, const SwitchMsg& m, unsigned sliderOff)
{
    U64 v[2] = {};
    if (!Copy(v, uiVecAddr, 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) % SLOT_BYTES != 0) return;
    U64 slotCount = (v[1] - v[0]) / SLOT_BYTES;
    for (unsigned i = 0; i < m.count && i < MAX_WORDS; i++) {
        unsigned off = m.words[i].offset;
        unsigned s = off / SLOT_STRIDE;
        unsigned region = (off % SLOT_STRIDE) / REGION_STRIDE;
        unsigned local = off % REGION_STRIDE;
        if (region != 1 || s >= slotCount || (local % 0x10) != sliderOff) continue;
        unsigned k = local / 0x10;
        U64 e[2] = {};
        U64 uiSlot = v[0] + (U64)s * SLOT_BYTES;
        if (!Copy(e, (const void*)uiSlot, 16) || !e[0] || e[1] < e[0] || (e[1] - e[0]) % 0x10 != 0) continue;
        if (k >= (e[1] - e[0]) / 0x10) continue;
        *(unsigned*)(e[0] + (U64)k * 0x10 + sliderOff) = m.words[i].now;
    }
}

static bool ApplySliderActionViaEngine(U64 building, const SwitchMsg& m, int currencyMode, unsigned actionRva,
                                       unsigned char initZeroFlag, unsigned sliderOff, bool writeSliders)
{
    if (g_exeBase[actionRva] == 0 || g_exeBase[BLD_INIT_RES_SLIDERS_RVA] == 0) return false;
    typedef void (*InitSlidersFn)(void* world, void* bld, unsigned char mode);
    typedef void (*ActionFn)(void* world, void* bld, int currencyMode);
    typedef void (*ClearVecFn)(void* vec);
    BYTE* uiVecAddr = World() + WORLD_UI_RES_SLIDERS_OFFSET;
    BYTE savedUiVec[24] = {};
    memcpy(savedUiVec, uiVecAddr, 24);
    memset(uiVecAddr, 0, 24);

    ((InitSlidersFn)(g_exeBase + BLD_INIT_RES_SLIDERS_RVA))(World(), (void*)building, initZeroFlag);
    if (writeSliders) WriteSlidersFromMsg(uiVecAddr, m, sliderOff);
    ((ActionFn)(g_exeBase + actionRva))(World(), (void*)building, currencyMode);
    if (g_exeBase[BLD_STORAGE_VEC_CLEAR_RVA] != 0) {
        ((ClearVecFn)(g_exeBase + BLD_STORAGE_VEC_CLEAR_RVA))(uiVecAddr);
    }
    memcpy(uiVecAddr, savedUiVec, 24);
    return true;
}

static const char* ApplyManualBuyFallback(U64 building, const SwitchMsg& m)
{
    U64 slots = 0;
    unsigned slotCount = SlotCount(building, &slots);
    if (!slots || !slotCount) return "building has no storage slots";
    const bool isUsd = (m.shape == 3);
    double* treasury = (double*)(World() + (isUsd ? WORLD_USD_OFFSET : WORLD_RUB_OFFSET));
    const unsigned priceOff = isUsd ? RES_PRICE_USD_OFFSET : RES_PRICE_RUB_OFFSET;

    for (unsigned i = 0; i < m.count && i < MAX_WORDS; i++) {
        unsigned off = m.words[i].offset;
        unsigned s = off / SLOT_STRIDE;
        unsigned region = (off % SLOT_STRIDE) / REGION_STRIDE;
        unsigned local = off % REGION_STRIDE;
        if (region != 1 || s >= slotCount || (local % 0x10) != 8) continue;
        unsigned k = local / 0x10;
        U64 at = 0;
        unsigned elems = 0;
        if (!Region(slots + (U64)s * SLOT_BYTES, 1, &at, &elems) || k >= elems) continue;
        float tons = 0.0f;
        memcpy(&tons, &m.words[i].now, 4);
        if (!isfinite(tons) || tons <= 0.0f) continue;

        U64 entryAddr = at + (U64)k * 0x10;
        const BYTE* resPtr = *(const BYTE* const*)entryAddr;
        float unitPrice = resPtr ? *(const float*)(resPtr + priceOff) : 0.0f;
        if (isfinite(unitPrice) && unitPrice > 0.0f) {
            *treasury -= (double)tons * (double)unitPrice;
        }
        *(float*)(entryAddr + 0x08) += tons;
    }
    *(int*)(building + BLD_EXPORT_MODE_OFFSET) = 0;
    return nullptr;
}

static const char* ApplyManualBuy(U64 building, const SwitchMsg& m)
{
    if (!building) return "the building is gone";
    if (m.shape != 3 && m.shape != 4) return "invalid currency mode";
    if (ApplySliderActionViaEngine(building, m, (int)m.shape, BLD_MANUAL_BUY_RES_RVA, 0, 0x08u, true)) {
        *(int*)(building + BLD_EXPORT_MODE_OFFSET) = 0;
        return nullptr;
    }
    return ApplyManualBuyFallback(building, m);
}

static const char* ApplyAutoBuyFallback(U64 building, const SwitchMsg& m, int currencyMode, bool isDisable)
{
    U64 slots = 0;
    unsigned slotCount = SlotCount(building, &slots);
    if (slots && slotCount > 0) {
        for (unsigned s = 0; s < slotCount; s++) {
            U64 at = 0;
            unsigned elems = 0;
            if (!Region(slots + (U64)s * SLOT_BYTES, 1, &at, &elems) || !at) continue;
            for (unsigned k = 0; k < elems; k++) {
                *(float*)(at + (U64)k * 0x10 + 0x0C) = 0.0f;
            }
        }
        if (!isDisable) {
            for (unsigned i = 0; i < m.count && i < MAX_WORDS; i++) {
                unsigned off = m.words[i].offset;
                unsigned s = off / SLOT_STRIDE;
                unsigned region = (off % SLOT_STRIDE) / REGION_STRIDE;
                unsigned local = off % REGION_STRIDE;
                if (region != 1 || s >= slotCount || (local % 0x10) != 0x0C) continue;
                unsigned k = local / 0x10;
                U64 at = 0;
                unsigned elems = 0;
                if (!Region(slots + (U64)s * SLOT_BYTES, 1, &at, &elems) || !at || k >= elems) continue;
                float tons = 0.0f;
                memcpy(&tons, &m.words[i].now, 4);
                if (!isfinite(tons) || tons <= 0.0f) continue;
                *(unsigned*)(at + (U64)k * 0x10 + 0x0C) = m.words[i].now;
            }
        }
    }
    if (isDisable) {
        *(int*)(building + BLD_AUTO_BUY_MODE_OFFSET) = 0;
    } else {
        *(int*)(building + BLD_AUTO_BUY_MODE_OFFSET) = currencyMode;
        *(int*)(building + BLD_EXPORT_MODE_OFFSET) = 0;
    }
    return nullptr;
}

static const char* ApplyAutoBuy(U64 building, const SwitchMsg& m)
{
    if (!building) return "the building is gone";
    if ((m.shape & ~(0xFFu | AUTO_BUY_DISABLE_FLAG)) != 0) return "invalid auto-buy flags";
    const int currencyMode = (int)(m.shape & 0xFFu);
    const bool isDisable = (m.shape & AUTO_BUY_DISABLE_FLAG) != 0;
    if (currencyMode != 3 && currencyMode != 4) return "invalid currency mode";
    if (ApplySliderActionViaEngine(building, m, currencyMode, BLD_APPLY_AUTO_BUY_RVA, 1, 0x0Cu, !isDisable)) {
        if (isDisable) {
            *(int*)(building + BLD_AUTO_BUY_MODE_OFFSET) = 0;
        } else {
            *(int*)(building + BLD_EXPORT_MODE_OFFSET) = 0;
        }
        return nullptr;
    }
    return ApplyAutoBuyFallback(building, m, currencyMode, isDisable);
}

bool BuildingSwitches_CaptureUnivResearch(unsigned long long building, int ownerState,
                                          int oldTechIdx, int newTechIdx,
                                          unsigned oldRatioBits, unsigned newRatioBits)
{
    if (!building || (ownerState != 1 && ownerState != 2)) return false;
    RouteRef ref = kNoRef;
    if (!SwitchCommon_GuardedRef(false, building, ref)) return false;

    SwitchMsg m = {};
    m.building = ref;
    m.kind = KIND_UNIV_RESEARCH;
    m.shape = (unsigned)ownerState;
    m.count = 2;
    m.words[0] = { 0u, (unsigned)(oldTechIdx + 1), (unsigned)(newTechIdx + 1) };
    m.words[1] = { BLD_UNIV_STAFF_RATIO_OFFSET, oldRatioBits, newRatioBits };
    if (Commands_Capturing()) {
        Submit(m);
        return true;
    }
    return BuildingSwitches_ApplyCommand(&m, sizeof(m));
}

static const char* ApplyUnivResearch(U64 building, const SwitchMsg& m)
{
    if (!building) return "the building is gone";
    if (m.count < 2 || (m.shape != 1 && m.shape != 2)) return "invalid university research payload";
    int oldTechIdx = (int)m.words[0].old - 1;
    int newTechIdx = (int)m.words[0].now - 1;
    *(unsigned*)(building + BLD_UNIV_STAFF_RATIO_OFFSET) = m.words[1].now;
    if (newTechIdx == oldTechIdx && m.words[1].now != m.words[1].old) {
        return nullptr;
    }
    if (!TwoStates_ApplyUnivResearchSelection(World(), (BYTE*)building, (int)m.shape, newTechIdx)) {
        return "university research target invalid";
    }
    return nullptr;
}

static const TsmRoutineSig kRebuildStopLineNamesSig[] = {
    { REBUILD_STOP_LINE_NAMES_RVA, { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x9F, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48 }, 16 },
};
static int g_rebuildStopLineNamesReady = -1;

static void RefreshStopLineNamesForBuilding(U64 building)
{
    if (!building || !g_exeBase) return;
    BYTE* world = World();
    U64 refreshSrc = 0;
    if (Copy(&refreshSrc, world + WORLD_RENAME_REFRESH_SRC_OFF, 8)) {
        Copy(world + WORLD_RENAME_REFRESH_DST_OFF, &refreshSrc, 8);
    }
    if (!TsmCheckRoutineTable(g_exeBase, &g_rebuildStopLineNamesReady, kRebuildStopLineNamesSig, 1)) return;
    U64 linesMgr = 0;
    if (Copy(&linesMgr, world + WORLD_LINES_OFFSET, 8) && linesMgr) {
        typedef void (__fastcall* RebuildStopLineNamesFn)(void* linesMgr, void* bld);
        ((RebuildStopLineNamesFn)(g_exeBase + REBUILD_STOP_LINE_NAMES_RVA))((void*)linesMgr, (void*)building);
    }
}

static const char* ValidateSwitchWords(U64 building, const SwitchMsg& m)
{
    if (m.kind != KIND_WORLD && !building) return "the building is gone";
    for (unsigned i = 0; i < m.count; i++) {
        unsigned off = m.words[i].offset;
        bool isOneByteWorld = (m.kind == KIND_WORLD && IsOneByteWorldSetting(off));
        if ((!isOneByteWorld && (off % 4)) || !WordAddress(building, m, off))
            return "a word is not there (another shape here)";
        if (m.kind != KIND_WORLD) {
            Range checkR = { building, (int)m.kind, 0, 0, 0, m.shape, 0 };
            if (LeftOut(checkR, off, true))
                return "a word is in a view/vector range";
            bool nonPtrPair = IsNonPointerBuildingWord((int)m.kind, off);
            U64 pairAddr = WordAddress(building, m, off & ~7u);
            if (!nonPtrPair && pairAddr && WordAddress(building, m, (off & ~7u) + 4) == pairAddr + 4 &&
                PointerLike(*(const U64*)pairAddr))
                return "a word overlaps a heap pointer here";
        }
    }
    return nullptr;
}

static void WriteSwitchWords(U64 building, const SwitchMsg& m, unsigned& differing)
{
    bool renamedBuilding = false;
    for (unsigned i = 0; i < m.count; i++) {
        unsigned off = m.words[i].offset;
        if (m.kind == KIND_WORLD && IsLocalPerStateWorldSetting(off)) continue;
        if (m.kind == KIND_WORLD && IsOneByteWorldSetting(off)) {
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
            if (m.kind == KIND_BUILDING && off >= NAME_AT && off < NAME_END) {
                renamedBuilding = true;
            }
        }
        if (m.kind == KIND_WORLD) // the session's value from now on
            for (int k = 0; k < g_worldCount; k++)
                if (g_worldAt[k] == off) {
                    g_auth[k] = IsOneByteWorldSetting(off) ? (m.words[i].now & 0xFF) : m.words[i].now;
                    g_stale[k] = g_auth[k];
                    g_sentValid[k] = false;
                }
    }
    if (renamedBuilding) {
        RefreshStopLineNamesForBuilding(building);
    }
}

bool BuildingSwitches_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(SwitchMsg)) return false;
    SwitchMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.kind > KIND_AUTO_BUY || (!m.count && m.kind != KIND_AUTO_BUY) || m.count > MAX_WORDS) return false;
    const char* why = nullptr;
    unsigned differing = 0;
    DiagLog_BeginSafeSeh();
    __try {
        U64 building = m.kind == KIND_WORLD ? 0 : Ref_BuildingAt(m.building);
        if (m.kind == KIND_MANUAL_BUY) {
            why = ApplyManualBuy(building, m);
        } else if (m.kind == KIND_AUTO_BUY) {
            why = ApplyAutoBuy(building, m);
        } else if (m.kind == KIND_UNIV_RESEARCH) {
            why = ApplyUnivResearch(building, m);
        } else {
            why = ValidateSwitchWords(building, m);
            if (!why) WriteSwitchWords(building, m, differing);
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

static const TsmRoutineSig kCopyWeightsSig[] = {
    { FN_TOOL_COPY_WEIGHTS_RVA,
      { 0x4C, 0x8B, 0xDC, 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x80, 0xB9, 0xBC, 0x0F, 0x00 },
      16 },
};

void BuildingSwitches_SetCopyWeightsOverrideForTest(unsigned long long (*fn)(unsigned long long world, unsigned long long a2))
{
    g_testCopyWeightsOverride = fn;
    g_copyWeightsReady = -1;
}

static bool CopyWeightsReady()
{
    if (g_testCopyWeightsOverride) return true;
    return Ref_CheckRoutineTable(&g_copyWeightsReady, kCopyWeightsSig,
                                 sizeof(kCopyWeightsSig) / sizeof(kCopyWeightsSig[0]),
                                 "BUILDING_SWITCHES");
}

static bool IsWaitingLobbyBlockedForTools()
{
    return SaveSync_IsWaitingLobbyActive() ||
           (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded());
}

static bool IsValidCopyWeightsTarget(U64 srcBld, U64 dstBld)
{
    if (!IsPointerLikeX64(srcBld) || !IsPointerLikeX64(dstBld) || srcBld == dstBld) return false;
    BYTE isPlanned = 0;
    float progress = 0.0f;
    if (!Copy(&isPlanned, (const void*)(dstBld + BLD_IS_PLANNED_OFF), sizeof(isPlanned)) || isPlanned != 0)
        return false;
    if (!Copy(&progress, (const void*)(dstBld + BLD_BUILT_PROGRESS_OFF), sizeof(progress)) ||
        !isfinite(progress) || progress < 1.0f)
        return false;
    int srcCat = Category(srcBld);
    return srcCat >= 0 && srcCat == Category(dstBld);
}

static void SubmitCopyWeightsMsg(U64 srcBld, U64 dstBld)
{
    if (!CheckBuildingControlAllowed(dstBld, Tr("копирование весов в чужое здание", "copying weights to a foreign building")))
        return;
    SwitchMsg m = {};
    m.building = kNoRef;
    m.kind = KIND_BUILDING;
    if (!SwitchCommon_GuardedRef(false, dstBld, m.building)) return;
    for (unsigned off = BLD_COPY_WEIGHTS_BEGIN_OFFSET;
         off + 4 <= BLD_COPY_WEIGHTS_END_OFFSET && m.count < MAX_WORDS;
         off += 4) {
        unsigned srcWord = 0, dstWord = 0;
        if (!Copy(&srcWord, (const void*)(srcBld + off), sizeof(srcWord)) ||
            !Copy(&dstWord, (const void*)(dstBld + off), sizeof(dstWord))) {
            return;
        }
        if (srcWord != dstWord) {
            m.words[m.count++] = { off, dstWord, srcWord };
        }
    }
    if (m.count > 0) Submit(m);
}

static unsigned long long CallToolWithClearedPress(CopyWeightsToolFn tool, unsigned long long world,
                                                   unsigned long long a2, BYTE origPress)
{
    g_exeBase[SWITCH_PRESS_RVA] = 0;
    unsigned long long r = tool(world, a2);
    g_exeBase[SWITCH_PRESS_RVA] = origPress;
    return r;
}

unsigned long long BuildingSwitches_CopyWeightsTool(unsigned long long world, unsigned long long a2)
{
    if (!world || !g_exeBase) return 0;
    CopyWeightsToolFn tool = g_testCopyWeightsOverride
                                 ? g_testCopyWeightsOverride
                                 : (CopyWeightsToolFn)(g_exeBase + FN_TOOL_COPY_WEIGHTS_RVA);
    if (IsWaitingLobbyBlockedForTools()) {
        BYTE origPress = g_exeBase[SWITCH_PRESS_RVA];
        unsigned long long r = CallToolWithClearedPress(tool, world, a2, origPress);
        if (origPress != 0) SaveSync_NotifyWaitingLobbyActionBlocked();
        return r;
    }
    if (!Commands_TakeLocal() || !CopyWeightsReady()) return tool(world, a2);
    BYTE origPress = g_exeBase[SWITCH_PRESS_RVA];
    if (origPress == 0) return tool(world, a2);

    U64 prevHover = 0;
    const bool haveHover = Copy(&prevHover, (const void*)(world + WORLD_HOVERED_BLD_OFF), sizeof(prevHover));
    if (haveHover) {
        const U64 zero = 0;
        Copy((void*)(world + WORLD_HOVERED_BLD_OFF), &zero, sizeof(zero));
    }
    unsigned long long r = CallToolWithClearedPress(tool, world, a2, origPress);

    U64 dstBld = 0;
    if (!Copy(&dstBld, (const void*)(world + WORLD_HOVERED_BLD_OFF), sizeof(dstBld)) || !dstBld) {
        if (haveHover) Copy((void*)(world + WORLD_HOVERED_BLD_OFF), &prevHover, sizeof(prevHover));
        return r;
    }
    U64 srcBld = 0;
    if (Copy(&srcBld, (const void*)(world + WORLD_COPY_TOOL_SRC_BLD_OFF), sizeof(srcBld)) &&
        IsValidCopyWeightsTarget(srcBld, dstBld)) {
        SubmitCopyWeightsMsg(srcBld, dstBld);
    }
    return r;
}

bool BuildingSwitches_IsOneByteWorldSettingForTest(unsigned at)
{
    return IsOneByteWorldSetting(at);
}

bool BuildingSwitches_IsLocalPerStateWorldSettingForTest(unsigned at)
{
    return IsLocalPerStateWorldSetting(at);
}

bool BuildingSwitches_IsNoiseEmptyForTest()
{
    if (!g_noiseBuilding.empty() || !g_noiseWorld.empty()) return false;
    for (int i = 0; i < 4; ++i) {
        if (!g_noiseSlot[i].empty()) return false;
    }
    return g_noiseLogged == 0 && g_heapLogged == 0 && g_leftLogged == 0 &&
           g_backoff.framesSinceSample == 0 && g_backoff.sampleEvery == SWITCH_SAMPLE_EVERY_BASE &&
           g_backoff.cleanSamples == 0 && g_epoch == 0;
}

bool BuildingSwitches_ClickedForTest()
{
    return SwitchCommon_Clicked(g_exeBase);
}

void BuildingSwitches_GetStatsForTest(SwitchPerfStatsForTest* out)
{
    SwitchCommon_FillStats(out, g_backoff, g_snapshotCount, g_mismatchCopyCount,
                           g_listScratch.capacity(), g_cmpScratch.capacity());
}
