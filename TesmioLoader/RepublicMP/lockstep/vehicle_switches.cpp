// SPDX-License-Identifier: GPL-3.0-only
// See vehicle_switches.h. The comparison follows SoylentGreed's interface
// switches (RepublicLAN, read against the disassembly of 1.1.1.9).
#include "vehicle_switches.h"
#include "commands.h"
#include "switch_common.h"
#include "world_refs.h"
#include "../session/two_states.h"
#include "../ui/lang.h"
#include "../core/diag_log.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <math.h>
#include <string.h>
#include <vector>

#define VEHICLES_OFFSET 0x128C0
#define VEHICLE_BYTES 0x1C30
#define STOPS_LIST 0x638         // stop records, 0x198 each
#define STOP_BYTES 0x198
#define CARGO_LIST 0x3B8         // cargo records, 0x10 each
#define CARGO_BYTES 0x10
#define MAX_RECORDS 256
#define MAX_VEHICLES 16384
#define MAX_WORDS 60
#define NAME_FLAG 0x108          // a vehicle's custom-name flag and its name (0x40 characters = 0x80 bytes at +0x10a): text
#define NAME_END ((0x10A + 0x80 + 3) & ~3u)

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

// A vehicle's record list: begin and count (0 when unreadable or too long).
static U64 Records(U64 vehicle, int kind, unsigned* count)
{
    U64 v[2] = {};
    *count = 0;
    if (!SafeMemcpyPod(v, (const void*)(vehicle + kListAt[kind]), 16) || !v[0] || v[1] < v[0] || (v[1] - v[0]) % kRecordBytes[kind])
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
static std::vector<SwitchSpan> g_snapSpans;
static std::vector<U64> g_listScratch;
static std::vector<BYTE> g_bytes;
static std::vector<BYTE> g_cmpScratch;
static bool g_taken = false, g_click = false;
static unsigned long long g_epoch = 0;
static SwitchBackoffState g_backoff = { 0, SWITCH_SAMPLE_EVERY_BASE, 0, { 0, 0, 0, false } };
static unsigned long long g_snapshotCount = 0;
static unsigned long long g_mismatchCopyCount = 0;
// Words the interface changes without a click, per kind (word index within one record).
static std::vector<bool> g_noise[KIND_COUNT];
static int g_noiseLogged = 0, g_heapLogged = 0;

static void ScanVehicleSnapsBatchPod(
    const U64* list, size_t count, volatile size_t* ioIdx,
    Snap* outSnaps, SwitchSpan* outSpans, size_t maxSnaps,
    size_t* ioSnapCount, size_t* ioTotalBytes)
{
    DiagLog_BeginSafeSeh();
    size_t idx = *ioIdx;
    volatile size_t cur = idx;
    size_t sc = *ioSnapCount;
    size_t tb = *ioTotalBytes;
    __try {
        for (; idx < count && sc < maxSnaps; ++idx) {
            cur = idx;
            const U64 vehicle = list[idx];
            if (!vehicle) continue;
            Snap s = {};
            s.vehicle = vehicle;
            s.records[KIND_VEHICLE] = 1;
            U64 begin[KIND_COUNT] = { vehicle, 0, 0 };
            for (int k = KIND_STOP; k < KIND_COUNT; ++k) {
                const U64* rv = (const U64*)(vehicle + kListAt[k]);
                const U64 rb = rv[0];
                const U64 re = rv[1];
                if (!rb || re < rb || ((re - rb) % kRecordBytes[k]) != 0) continue;
                const U64 recCount = (re - rb) / kRecordBytes[k];
                if (recCount > MAX_RECORDS) continue;
                s.records[k] = (unsigned)recCount;
                begin[k] = rb;
            }
            for (int k = 0; k < KIND_COUNT; ++k) {
                s.at[k] = tb;
                const size_t bytes = (size_t)s.records[k] * kRecordBytes[k];
                outSpans[sc * KIND_COUNT + k] = { (const void*)begin[k], tb, bytes };
                tb += bytes;
            }
            outSnaps[sc] = s;
            ++sc;
        }
        *ioIdx = idx;
        *ioSnapCount = sc;
        *ioTotalBytes = tb;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ioIdx = cur + 1u;
        *ioSnapCount = sc;
        *ioTotalBytes = tb;
    }
    DiagLog_EndSafeSeh();
}

static bool TakeSnapshot()
{
    g_snaps.clear();
    g_snapSpans.clear();
    g_bytes.clear();
    bool wasNonEmpty = false;
    if (!SwitchCommon_ReadPtrVector(World() + VEHICLES_OFFSET, MAX_VEHICLES, g_listScratch, &wasNonEmpty)) {
        return false;
    }
    if (!wasNonEmpty) {
        ++g_snapshotCount;
        return true;
    }
    const size_t n = g_listScratch.size();
    g_snaps.resize(n);
    g_snapSpans.resize(n * KIND_COUNT);
    volatile size_t idx = 0;
    size_t snapCount = 0;
    size_t totalBytes = 0;
    while (idx < n && snapCount < n) {
        ScanVehicleSnapsBatchPod(
            g_listScratch.data(), n, &idx,
            g_snaps.data(), g_snapSpans.data(), n,
            &snapCount, &totalBytes);
    }
    g_snaps.resize(snapCount);
    g_snapSpans.resize(snapCount * KIND_COUNT);
    if (totalBytes > 0) {
        g_bytes.resize(totalBytes);
        SwitchCommon_CopySpanBatchPod(g_snapSpans.data(), snapCount * KIND_COUNT, g_bytes.data());
    }
    size_t writeIdx = 0;
    for (size_t i = 0; i < snapCount; ++i) {
        bool ok = true;
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (g_snaps[i].records[k] > 0 && g_snapSpans[i * KIND_COUNT + k].bytes == 0) {
                ok = false;
                break;
            }
        }
        if (ok) {
            if (writeIdx != i) g_snaps[writeIdx] = g_snaps[i];
            ++writeIdx;
        }
    }
    g_snaps.resize(writeIdx);
    ++g_snapshotCount;
    return true;
}

static bool PointerLike(U64 v)
{
    return IsPointerLikeX64(v);
}

static bool TextWord(int kind, unsigned offset)
{
    if (kind == KIND_CARGO) return (offset % CARGO_BYTES) == 0x0Cu;
    return kind == KIND_VEHICLE && offset >= NAME_FLAG && offset < NAME_END;
}

// A stop record's lists (each half's +8 pairs and +38..+c8 lists): heap links,
// their contents are cargo_settings.cpp's. The vehicle's internal vectors,
// object pointers (+4A0 link), dispatch/work state (+4c8..+574, +5d8..+5f4 incl. fuel),
// route/rail path, 25-entry trail buffer, 6th node, wheels vector, and dynamic
// simulation/render matrices (+680..+1700, +1708..+1768, +17e8..+17f0,
// +1950..+1a10, +1a54..+1a5c, +1a7c..+1a84, +1ad0..+1ad8)
// must never be overwritten or reverted as UI switches.
#define HALF_BYTES 0xC8
static bool ListWord(int kind, unsigned offset)
{
    if (kind == KIND_VEHICLE) {
        return (offset >= 0x3A0 && offset < 0x3D0) ||
               (offset >= 0x458 && offset < 0x4A8) ||
               (offset >= 0x4C8 && offset < 0x574) ||
               (offset >= 0x5D8 && offset < 0x5F4) ||
               (offset >= 0x600 && offset < 0x608) ||
               (offset >= 0x638 && offset < 0x658) ||
               (offset >= 0x680 && offset < 0x1740) ||
               (offset >= 0x1748 && offset < 0x1750) ||
               (offset >= 0x17B8 && offset < 0x1800) ||
               (offset >= 0x1880 && offset < 0x1888) ||
               (offset >= 0x19E8 && offset < 0x1AA8) ||
               (offset >= 0x1B18 && offset < 0x1B50) ||
               (offset >= 0x1BAC && offset < 0x1BB4) ||
               (offset >= 0x1BD4 && offset < 0x1BE0) ||
               (offset >= 0x1C28 && offset < 0x1C30);
    }
    if (kind == KIND_CARGO) return (offset % CARGO_BYTES) < 0x0Cu; // Resource* (+0x00) and dynamic amountTons (+0x08); only filterFlag (+0x0C) is a UI switch
    if (kind != KIND_STOP || offset >= 2 * HALF_BYTES) return false;
    unsigned l = offset % HALF_BYTES;
    return (l >= 0x8 && l < 0x20) || l >= 0x38;
}

bool VehicleSwitches_IsListWordForTest(int kind, unsigned offset)
{
    return ListWord(kind, offset);
}

static void Submit(const SwitchMsg& m)
{
    Commands_Submit(LS_CMD_VEHICLE_SWITCH, &m, sizeof(m));
}

enum { kVehScanDone = 0, kVehScanChanged = 1, kVehScanFaulted = 2 };

static int FindNextChangedVehicleSnapPod(
    const Snap* snaps, size_t n, size_t* ioIdx, unsigned* ioKind,
    const BYTE* bytesBase, U64* outBegin, size_t* outBytes)
{
    if (!snaps || !ioIdx || !ioKind || !bytesBase || !outBegin || !outBytes) return kVehScanDone;
    size_t idx = *ioIdx;
    unsigned k = *ioKind;
    if (idx >= n) return kVehScanDone;
    int result = kVehScanDone;
    volatile size_t curIdx = idx;
    volatile unsigned curKind = k;
    DiagLog_BeginSafeSeh();
    __try {
        for (; idx < n; ++idx, k = 0) {
            for (; k < KIND_COUNT; ++k) {
                curIdx = idx;
                curKind = k;
                const Snap& s = snaps[idx];
                if (s.records[k] == 0) continue;
                U64 begin = s.vehicle;
                if (k != KIND_VEHICLE) {
                    const U64* rv = (const U64*)(s.vehicle + kListAt[k]);
                    const U64 rb = rv[0];
                    const U64 re = rv[1];
                    if (!rb || re < rb || ((re - rb) % kRecordBytes[k]) != 0) continue;
                    const U64 now = (re - rb) / kRecordBytes[k];
                    if (now > MAX_RECORDS || now != s.records[k]) continue;
                    begin = rb;
                }
                const size_t bytes = (size_t)s.records[k] * kRecordBytes[k];
                if (memcmp((const void*)begin, bytesBase + s.at[k], bytes) != 0) {
                    *ioIdx = idx;
                    *ioKind = k;
                    *outBegin = begin;
                    *outBytes = bytes;
                    result = kVehScanChanged;
                    break;
                }
            }
            if (result == kVehScanChanged) break;
        }
        if (result == kVehScanDone) {
            *ioIdx = n;
            *ioKind = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const size_t fi = curIdx;
        const unsigned fk = curKind + 1u;
        if (fk < KIND_COUNT) {
            *ioIdx = fi;
            *ioKind = fk;
        } else {
            *ioIdx = fi + 1u;
            *ioKind = 0;
        }
        result = kVehScanFaulted;
    }
    DiagLog_EndSafeSeh();
    return result;
}

// Compares one record kind of one vehicle; on a click the changes are put back and carried.
static bool CompareChangedVehicleSnap(const Snap& s, int kind, U64 begin, size_t bytes, bool click)
{
    if (!SwitchCommon_CopyMismatchToScratch((const void*)begin, bytes, g_cmpScratch)) return false;
    ++g_mismatchCopyCount;
    const BYTE* cur = g_cmpScratch.data();
    const BYTE* old = g_bytes.data() + s.at[kind];
    std::vector<bool>& noise = g_noise[kind];
    if (noise.empty()) noise.assign(kRecordBytes[kind] / 4, false);

    SwitchMsg m;
    memset(&m, 0, sizeof(m));
    m.kind = kind;
    m.records = kind == KIND_VEHICLE ? 0 : s.records[kind];
    bool haveRef = false;
    bool anyEmittedOrLearned = false;
    for (size_t q = 0; q + 8 <= bytes; q += 8) {
        U64 a = *(const U64*)(old + q), b = *(const U64*)(cur + q);
        if (a == b) continue;
        unsigned inRecord = (unsigned)(q % kRecordBytes[kind]);
        bool heap = (PointerLike(a) || PointerLike(b)) && !TextWord(kind, inRecord) && !(kind == KIND_CARGO && inRecord == 8);
        for (unsigned h = 0; h < 8; h += 4) {
            unsigned wa = *(const unsigned*)(old + q + h), wb = *(const unsigned*)(cur + q + h);
            if (wa == wb) continue;
            unsigned offset = (unsigned)(q + h);
            unsigned word = (offset % kRecordBytes[kind]) / 4;
            if (ListWord(kind, offset % kRecordBytes[kind])) continue;
            bool neverNoise = TextWord(kind, offset % kRecordBytes[kind]);
            if (!click) {
                if (!neverNoise && !noise[word]) {
                    noise[word] = true;
                    anyEmittedOrLearned = true;
                    if (g_noiseLogged < 40) {
                        ++g_noiseLogged;
                        Logf("republicmp  SWITCH: the interface changes %s +0x%X without a click - left out", kKindText[kind],
                             offset % kRecordBytes[kind]);
                    }
                }
                continue;
            }
            if (!neverNoise && noise[word]) continue;
            if (heap) {
                if (g_heapLogged < 20) {
                    ++g_heapLogged;
                    Logf("republicmp  SWITCH: a click changed %s +0x%X to a heap address - not carried", kKindText[kind],
                         offset % kRecordBytes[kind]);
                }
                SafeWritePod((void*)(begin + offset), wa);
                anyEmittedOrLearned = true;
                continue;
            }
            if (!haveRef) {
                if (!TwoStates_CheckVehicleControlAllowed((const BYTE*)s.vehicle, Tr("изменение настроек чужого транспорта", "changing foreign vehicle settings"))) {
                    SafeMemcpyPod((void*)begin, old, bytes);
                    return true;
                }
                haveRef = SwitchCommon_GuardedRef(true, s.vehicle, m.vehicle);
                if (!haveRef) {
                    Logf("republicmp  SWITCH: a click changed a vehicle that is not in the world's list - not carried");
                    return anyEmittedOrLearned;
                }
            }
            // Put back at once: the clicking PC's simulation never sees it before the command.
            SafeWritePod((void*)(begin + offset), wa);
            m.words[m.count++] = { offset, wa, wb };
            anyEmittedOrLearned = true;
            if (m.count == MAX_WORDS) {
                Submit(m);
                m.count = 0;
            }
        }
    }
    if (m.count) Submit(m);
    return anyEmittedOrLearned;
}

void VehicleSwitches_Reset()
{
    g_taken = false;
    g_snaps.clear();
    g_snapSpans.clear();
    g_listScratch.clear();
    g_bytes.clear();
    g_cmpScratch.clear();
    for (int i = 0; i < KIND_COUNT; ++i) g_noise[i].clear();
    g_noiseLogged = g_heapLogged = 0;
    SwitchBackoff_Reset(g_backoff);
    g_epoch = 0;
    g_snapshotCount = 0;
    g_mismatchCopyCount = 0;
}

void VehicleSwitches_Before(bool simBusy)
{
    g_taken = false;
    if (!Commands_TakeUi() || simBusy) return;
    g_click = SwitchCommon_Clicked(g_exeBase);
    // Frames without a click are compared now and then, to learn the interface's own changes.
    if (!SwitchBackoff_ShouldSampleBefore(g_backoff, g_exeBase, g_click)) return;
    g_epoch = Commands_ApplyEpoch();
    g_taken = TakeSnapshot();
}

void VehicleSwitches_After(bool simBusy)
{
    if (!g_taken) return;
    g_taken = false;
    if (simBusy || !Commands_TakeUi() || Commands_ApplyEpoch() != g_epoch) return;
    bool anyChange = false;
    const size_t total = g_snaps.size();
    size_t idx = 0;
    unsigned kind = 0;
    while (idx < total) {
        U64 begin = 0;
        size_t bytes = 0;
        const int status = FindNextChangedVehicleSnapPod(
            g_snaps.data(), total, &idx, &kind, g_bytes.data(), &begin, &bytes);
        if (status == kVehScanDone) break;
        if (status == kVehScanFaulted) continue;
        const size_t changedIdx = idx;
        const unsigned changedKind = kind;
        if (++kind >= KIND_COUNT) {
            ++idx;
            kind = 0;
        }
        if (CompareChangedVehicleSnap(g_snaps[changedIdx], (int)changedKind, begin, bytes, g_click)) {
            anyChange = true;
        }
    }
    SwitchBackoff_RecordAfter(g_backoff, g_exeBase, g_click, anyChange);
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

bool VehicleSwitches_IsNoiseEmptyForTest()
{
    for (int i = 0; i < KIND_COUNT; ++i) {
        if (!g_noise[i].empty()) return false;
    }
    return g_noiseLogged == 0 && g_heapLogged == 0 &&
           g_backoff.framesSinceSample == 0 && g_backoff.sampleEvery == SWITCH_SAMPLE_EVERY_BASE &&
           g_backoff.cleanSamples == 0 && g_epoch == 0;
}

void VehicleSwitches_GetStatsForTest(SwitchPerfStatsForTest* out)
{
    SwitchCommon_FillStats(out, g_backoff, g_snapshotCount, g_mismatchCopyCount,
                           g_listScratch.capacity(), g_cmpScratch.capacity());
}
