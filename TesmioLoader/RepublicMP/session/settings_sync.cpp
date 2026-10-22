// SPDX-License-Identifier: GPL-3.0-only
// See settings_sync.h.
#include "settings_sync.h"
#include "../world_sync/citizens/citizen_destinations.h"
#include "../world_sync/buildings/build_sync.h"
#include "../ui/sync_block.h"
#include "../lockstep/commands.h"
#include <unordered_map>
#include <vector>
#include <math.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define VEC_BUILDINGS_BEGIN_RVA 0x9E6A18
#define VEC_BUILDINGS_END_RVA 0x9E6A20
#define VEC_CONSTRUCTIONS_BEGIN_RVA 0x9E6B38
#define VEC_CONSTRUCTIONS_END_RVA 0x9E6B40
#define BLD_TYPE_OFFSET 0x318
#define TYPE_CATEGORY_OFFSET 0x360
#define BLD_POS_OFFSET 0x350
#define BLD_STORAGE_OFFSET 0x970      // vector of 0xE0-byte storage slots
#define STORAGE_SLOT_SIZE 0xE0
#define SLOT_SETTINGS_OFFSET 0x18     // vector of {Resource*, float share, float enabled}
#define SLOT_ENTRY_SIZE 0x10
#define MAX_SLOTS 16

#define PIPELINE_TO_EXPORT_RVA 0x1B7AF0 // FUN_1401b7af0(world, bld): empties the export slots
#define PIPELINE_TO_IMPORT_RVA 0x1B7C00 // FUN_1401b7c00(world, bld): fills the chosen resource
#define AUTO_PURCHASE_RVA 0x1754D0      // FUN_1401754d0(world, bld, value)

enum FieldKind { FIELD_BYTES, FIELD_SLOT0 };   // SLOT0: offset is inside storage slot 0
enum FollowUp { FOLLOW_NONE, FOLLOW_PIPELINE, FOLLOW_AUTO_PURCHASE };

struct WatchedField {
    const char* name;
    int offset;
    int size;          // 1..8 bytes
    int kind;
    int followUp;
    int categories[6]; // -1 terminates; {ANY} = every building
};
#define ANY -2

// Building window settings (from the window functions dispatched by FUN_1406e9b80,
// cross-checked with the building save routine FUN_1401edc40).
static const WatchedField kFields[] = {
    // Foreign power connection / pipeline: +0x9E8 import side, +0x9EC export
    // side - exactly one is nonzero. Elsewhere these mean something else.
    { "import/export mode",      0x9E8,  8, FIELD_BYTES, FOLLOW_PIPELINE,      { 0x1F, 0x20, 0x6B, -1 } },
    { "max import/export share", 0x80,   4, FIELD_SLOT0, FOLLOW_NONE,          { 0x1F, 0x20, -1 } },
    { "pipeline resource",       0x133C, 1, FIELD_BYTES, FOLLOW_PIPELINE,      { 0x6B, -1 } },
    { "higher priority",         0x134C, 4, FIELD_BYTES, FOLLOW_NONE,          { 0x5B, 0x47, 0x12, 0x13, 0x23, -1 } },
    { "workers / visitors",      0xDEC,  8, FIELD_BYTES, FOLLOW_NONE,          { ANY, -1 } },
    { "construction workers",    0x630,  4, FIELD_BYTES, FOLLOW_NONE,          { ANY, -1 } },
    { "auto purchase",           0x11B8, 4, FIELD_BYTES, FOLLOW_AUTO_PURCHASE, { ANY, -1 } },
    { "collect from vehicles",   0x1272, 1, FIELD_BYTES, FOLLOW_NONE,          { ANY, -1 } },
    { "mechanisms only by truck", 0xE11, 1, FIELD_BYTES, FOLLOW_NONE,          { ANY, -1 } },
    { "prefer students",         0x11BE, 1, FIELD_BYTES, FOLLOW_NONE,          { ANY, -1 } },
    { "no waste disposal",       0x11BF, 1, FIELD_BYTES, FOLLOW_NONE,          { ANY, -1 } },
    // Who may move in: 8 flags read together by FUN_14083c590 (and saved /
    // copied as one block, FUN_1401edc40 / 0x1402D576E): +0x1414 nobody,
    // +0x1415..+0x1417 by the citizen's education (citizen+0xA8 = 0/1/2),
    // +0x1418..+0x141B by a second citizen trait in four ranges (citizen+0xE4).
    { "moving in restrictions",  0x1414, 8, FIELD_BYTES, FOLLOW_NONE,          { ANY, -1 } },
    { "allow repair",            0x11C0, 1, FIELD_BYTES, FOLLOW_NONE,          { 0x6C, -1 } },
    { "repair cancel when idle", 0x11C1, 1, FIELD_BYTES, FOLLOW_NONE,          { 0x6C, -1 } },
    { "repair threshold",        0xFC8,  4, FIELD_BYTES, FOLLOW_NONE,          { 0x6C, -1 } },
    { "closed above temperature", 0xE00, 1, FIELD_BYTES, FOLLOW_NONE,          { 0x46, -1 } },
    { "water quality",           0x1340, 4, FIELD_BYTES, FOLLOW_NONE,          { 0x61, 0x62, -1 } },
};
static const int kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

void SettingsSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    CitizenDestinations_Bind(host);
}

static bool FieldApplies(const WatchedField& f, int category)
{
    for (int i = 0; i < 6 && f.categories[i] != -1; i++)
        if (f.categories[i] == ANY || f.categories[i] == category) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Raw access (SEH-guarded, no C++ objects)
// ---------------------------------------------------------------------------

static int CategoryOf(BYTE* b)
{
    BYTE* type = *(BYTE**)(b + BLD_TYPE_OFFSET);
    return type ? *(int*)(type + TYPE_CATEGORY_OFFSET) : -1;
}

// Address of the field on this building, or null (no storage slot 0).
static BYTE* FieldAddress(BYTE* b, const WatchedField& f)
{
    if (f.kind == FIELD_SLOT0) {
        BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
        BYTE* slotsEnd = *(BYTE**)(b + BLD_STORAGE_OFFSET + 8);
        if (!slots || slotsEnd - slots < STORAGE_SLOT_SIZE) return nullptr;
        return slots + f.offset;
    }
    return b + f.offset;
}

static bool ReadField(BYTE* b, const WatchedField& f, unsigned long long* out)
{
    __try {
        BYTE* p = FieldAddress(b, f);
        if (!p) return false;
        *out = 0;
        memcpy(out, p, f.size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Storage slot settings of one slot: count and (share, enabled) pairs.
static int ReadSlot(BYTE* b, int slot, float* data)
{
    __try {
        BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
        BYTE* slotsEnd = *(BYTE**)(b + BLD_STORAGE_OFFSET + 8);
        if (!slots || (slotsEnd - slots) / STORAGE_SLOT_SIZE <= slot) return -1;
        BYTE* s = slots + slot * STORAGE_SLOT_SIZE;
        BYTE* e = *(BYTE**)(s + SLOT_SETTINGS_OFFSET);
        BYTE* ee = *(BYTE**)(s + SLOT_SETTINGS_OFFSET + 8);
        if (!e || ee < e) return 0;
        int n = (int)((ee - e) / SLOT_ENTRY_SIZE);
        if (n > SETTINGS_MAX_SLOT_ENTRIES) n = SETTINGS_MAX_SLOT_ENTRIES;
        for (int i = 0; i < n; i++) {
            data[i * 2] = *(float*)(e + i * SLOT_ENTRY_SIZE + 8);
            data[i * 2 + 1] = *(float*)(e + i * SLOT_ENTRY_SIZE + 0xC);
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static int SlotCount(BYTE* b)
{
    __try {
        BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
        BYTE* slotsEnd = *(BYTE**)(b + BLD_STORAGE_OFFSET + 8);
        if (!slots || slotsEnd < slots) return 0;
        int n = (int)((slotsEnd - slots) / STORAGE_SLOT_SIZE);
        return n > MAX_SLOTS ? MAX_SLOTS : n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static unsigned long long HashSlot(const float* data, int n)
{
    unsigned long long h = 0xcbf29ce484222325ull ^ (unsigned)n;
    const unsigned char* p = (const unsigned char*)data;
    for (int i = 0; i < n * 2 * 4; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

// Snapshot of the building list (pointers only valid for this scan).
static int CopyBuildings(BYTE** out, int max)
{
    int n = 0;
    __try {
        const unsigned int kBeginRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
        const unsigned int kEndRvas[2]   = { VEC_BUILDINGS_END_RVA,   VEC_CONSTRUCTIONS_END_RVA };
        for (int v = 0; v < 2; ++v) {
            BYTE** it = *(BYTE***)(g_exeBase + kBeginRvas[v]);
            BYTE** end = *(BYTE***)(g_exeBase + kEndRvas[v]);
            if (!it || end < it) continue;
            for (; it < end && n < max; ++it) if (*it) out[n++] = *it;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
}

static bool ReadIdentity(BYTE* b, int* category, float pos[3])
{
    __try {
        *category = CategoryOf(b);
        memcpy(pos, b + BLD_POS_OFFSET, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The local building at pos with this category (0.5 m in x/z, 1 m in y), nearest first.
static BYTE* FindBuilding(const float pos[3], int category)
{
    BYTE* found = nullptr;
    float best = 1e30f;
    __try {
        const unsigned int kBeginRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
        const unsigned int kEndRvas[2]   = { VEC_BUILDINGS_END_RVA,   VEC_CONSTRUCTIONS_END_RVA };
        for (int v = 0; v < 2; ++v) {
            BYTE** it = *(BYTE***)(g_exeBase + kBeginRvas[v]);
            BYTE** end = *(BYTE***)(g_exeBase + kEndRvas[v]);
            for (; it && it < end; ++it) {
                BYTE* b = *it;
                if (!b || CategoryOf(b) != category) continue;
                const float* p = (const float*)(b + BLD_POS_OFFSET);
                float dx = p[0] - pos[0], dz = p[2] - pos[2];
                if (fabsf(dx) >= 0.5f || fabsf(dz) >= 0.5f || fabsf(p[1] - pos[1]) >= 1.0f) continue;
                float d = dx * dx + dz * dz;
                if (d < best) { best = d; found = b; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return found;
}

typedef void (*BuildingFn)(__int64 world, __int64 bld);
typedef void (*AutoPurchaseFn)(__int64 world, __int64 bld, int value);

static bool WriteField(BYTE* b, const WatchedField& f, unsigned long long value)
{
    __int64 world = (__int64)(g_exeBase + WORLD_RVA);
    __try {
        BYTE* p = FieldAddress(b, f);
        if (!p) return false;
        memcpy(p, &value, f.size);
        if (f.followUp == FOLLOW_PIPELINE && CategoryOf(b) == 0x6B) {
            // As the window does after a mode or resource change.
            if (*(int*)(b + 0x9EC)) ((BuildingFn)(g_exeBase + PIPELINE_TO_EXPORT_RVA))(world, (__int64)b);
            else if (*(int*)(b + 0x9E8)) ((BuildingFn)(g_exeBase + PIPELINE_TO_IMPORT_RVA))(world, (__int64)b);
        } else if (f.followUp == FOLLOW_AUTO_PURCHASE) {
            ((AutoPurchaseFn)(g_exeBase + AUTO_PURCHASE_RVA))(world, (__int64)b, (int)value);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WriteSlot(BYTE* b, int slot, const float* data, int n)
{
    __try {
        BYTE* slots = *(BYTE**)(b + BLD_STORAGE_OFFSET);
        BYTE* slotsEnd = *(BYTE**)(b + BLD_STORAGE_OFFSET + 8);
        if (!slots || (slotsEnd - slots) / STORAGE_SLOT_SIZE <= slot) return false;
        BYTE* s = slots + slot * STORAGE_SLOT_SIZE;
        BYTE* e = *(BYTE**)(s + SLOT_SETTINGS_OFFSET);
        BYTE* ee = *(BYTE**)(s + SLOT_SETTINGS_OFFSET + 8);
        if (!e || (int)((ee - e) / SLOT_ENTRY_SIZE) != n) return false; // other resource list - skip
        for (int i = 0; i < n; i++) {
            *(float*)(e + i * SLOT_ENTRY_SIZE + 8) = data[i * 2];
            *(float*)(e + i * SLOT_ENTRY_SIZE + 0xC) = data[i * 2 + 1];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Scanning (game thread)
// ---------------------------------------------------------------------------

struct FieldState {
    unsigned long long value = 0;
    bool known = false;
    bool muted = false;          // flooding - no longer watched on this building
    int changes = 0;
    ULONGLONG windowStart = 0;
};
struct BuildingState {
    FieldState fields[sizeof(kFields) / sizeof(kFields[0])];
    unsigned long long slotHash[MAX_SLOTS] = {};
    bool slotKnown[MAX_SLOTS] = {};
    FieldState slotFlood[MAX_SLOTS];
    bool seen = false;           // in this scan's building list
    bool scanned = false;        // went through a scan before
};

static std::unordered_map<BYTE*, BuildingState> g_states;
static ULONGLONG g_lastScanMs = 0;
static std::vector<BYTE*> g_scanList;

struct PendingSetting {
    SettingWire w;
    ULONGLONG since;
};
static std::vector<PendingSetting> g_pending;

static void Send(BYTE* b, int field, unsigned long long value, int slot, const float* data, int count)
{
    SettingWire w = {};
    if (!ReadIdentity(b, &w.category, w.pos)) return;
    w.field = field;
    w.value = value;
    w.slot = slot;
    w.count = count;
    if (data && count > 0) memcpy(w.data, data, count * 2 * sizeof(float));
    BuildSync_SendSetting(w);
    if (field == SETTINGS_FIELD_SLOT)
        Logf("republicmp  SETTINGS: sending storage slot %d (%d entries) of category 0x%X at (%.1f,%.1f)", slot,
             count, w.category, w.pos[0], w.pos[2]);
    else
        Logf("republicmp  SETTINGS: sending [%s] = 0x%llx of category 0x%X at (%.1f,%.1f)", kFields[field].name,
             value, w.category, w.pos[0], w.pos[2]);
}

// Last time the local player had the left mouse button down (the game's own
// button bytes, see game/input_block). Every setting is changed with a click
// or a slider drag, so a change seen without a recent click was made by the
// simulation: it is recorded but not sent.
#define LMB_HELD_RVA 0xA54E90
#define LMB_JUST_PRESSED_RVA 0xA54E91
static ULONGLONG g_lastClickMs = 0;

static void TrackClick(ULONGLONG now)
{
    __try {
        if (*(g_exeBase + LMB_HELD_RVA) || *(g_exeBase + LMB_JUST_PRESSED_RVA)) g_lastClickMs = now;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool ClickedRecently(ULONGLONG now)
{
    return g_lastClickMs && now - g_lastClickMs <= SETTINGS_CLICK_WINDOW_MS;
}

// True if the change may be sent; mutes a field that changes too often.
static bool CountChange(FieldState& st, int field, ULONGLONG now)
{
    if (now - st.windowStart > SETTINGS_FLOOD_WINDOW_MS) {
        st.windowStart = now;
        st.changes = 0;
    }
    if (++st.changes > SETTINGS_FLOOD_LIMIT) {
        st.muted = true;
        Logf("republicmp  SETTINGS: [%s] changes too often on a building - no longer synced there",
             field == SETTINGS_FIELD_SLOT ? "storage slot" : kFields[field].name);
        return false;
    }
    return true;
}

static void Scan(bool sendChanges)
{
    g_scanList.resize(200000);
    int n = CopyBuildings(g_scanList.data(), (int)g_scanList.size());
    ULONGLONG now = GetTickCount64();
    bool byPlayer = ClickedRecently(now);
    for (auto& kv : g_states) kv.second.seen = false;

    for (int i = 0; i < n; i++) {
        BYTE* b = g_scanList[i];
        int category;
        float pos[3];
        if (!ReadIdentity(b, &category, pos)) continue;
        BuildingState& st = g_states[b];
        // Not "!seen": seen is cleared at the start of every scan, and
        // fields[0] / slot 0 only exist on some buildings - a house without
        // storage would then count as new on every scan and never send.
        bool fresh = !st.scanned;
        st.seen = true;
        st.scanned = true;

        for (int f = 0; f < kFieldCount; f++) {
            if (!FieldApplies(kFields[f], category)) continue;
            FieldState& fs = st.fields[f];
            if (fs.muted) continue;
            unsigned long long v;
            if (!ReadField(b, kFields[f], &v)) continue;
            if (fs.known && v != fs.value && sendChanges && !fresh && byPlayer && CountChange(fs, f, now))
                Send(b, f, v, 0, nullptr, 0);
            fs.value = v;
            fs.known = true;
        }

        int slots = SlotCount(b);
        for (int s = 0; s < slots; s++) {
            float data[SETTINGS_MAX_SLOT_ENTRIES * 2];
            int count = ReadSlot(b, s, data);
            if (count < 0 || st.slotFlood[s].muted) continue;
            unsigned long long h = HashSlot(data, count);
            if (st.slotKnown[s] && h != st.slotHash[s] && sendChanges && !fresh && byPlayer &&
                CountChange(st.slotFlood[s], SETTINGS_FIELD_SLOT, now))
                Send(b, SETTINGS_FIELD_SLOT, 0, s, data, count);
            st.slotHash[s] = h;
            st.slotKnown[s] = true;
        }
    }
    CitizenDestinations_Scan(g_scanList.data(), n, sendChanges, byPlayer);
    // Forget demolished buildings (their address may be reused).
    for (auto it = g_states.begin(); it != g_states.end();) {
        if (!it->second.seen) it = g_states.erase(it);
        else ++it;
    }
}

// ---------------------------------------------------------------------------
// Lockstep (see settings_sync.h)
// ---------------------------------------------------------------------------

#define SET_PAY_RVA 0x160550 // FUN_140160550(world, building, rubles, dollars)
#define STOP_RVA 0x160570    // FUN_140160570(world, building, 1)
#define RESUME_RVA 0x161440  // FUN_140161440(world, building)
#define BLD_PAY_OFFSET 0x608
#define BLD_STOPPED_OFFSET 0x879

struct LsBuilding {
    BYTE* b;
    int category;
    float pos[3];
    unsigned long long values[sizeof(kFields) / sizeof(kFields[0])];
    unsigned hasMask;           // bit per field that applies and was readable
    unsigned short pay;         // +608/+609
    unsigned char stopped;      // +879
    int slots;
    int slotCount[MAX_SLOTS];
    size_t slotAt;              // first float of slot 0 in g_lsSlotData
};
static std::vector<LsBuilding> g_lsBuildings;
static std::vector<float> g_lsSlotData;
static std::vector<BYTE*> g_lsList;

static bool ReadConstruction(BYTE* b, unsigned short* pay, unsigned char* stopped)
{
    __try {
        *pay = *(unsigned short*)(b + BLD_PAY_OFFSET);
        *stopped = *(b + BLD_STOPPED_OFFSET);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ReadLsBuilding(BYTE* b, LsBuilding& s)
{
    s.b = b;
    s.hasMask = 0;
    s.slots = 0;
    if (!ReadIdentity(b, &s.category, s.pos)) return;
    for (int f = 0; f < kFieldCount; f++)
        if (FieldApplies(kFields[f], s.category) && ReadField(b, kFields[f], &s.values[f])) s.hasMask |= 1u << f;
    if (!ReadConstruction(b, &s.pay, &s.stopped)) { s.pay = 0; s.stopped = 0; }
    s.slots = SlotCount(b);
    s.slotAt = g_lsSlotData.size();
    for (int i = 0; i < s.slots; i++) {
        float data[SETTINGS_MAX_SLOT_ENTRIES * 2];
        int n = ReadSlot(b, i, data);
        s.slotCount[i] = n < 0 ? -1 : n;
        if (n > 0) g_lsSlotData.insert(g_lsSlotData.end(), data, data + n * 2);
    }
}

static bool Overlaps(unsigned offset, int at, int size)
{
    return (int)offset + 4 > at && (int)offset < at + size;
}

static int SafeCategory(const BYTE* b)
{
    __try {
        return CategoryOf((BYTE*)b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool SettingsSync_OwnsBuildingWord(const BYTE* building, unsigned offset)
{
    int category = SafeCategory(building);
    for (int f = 0; f < kFieldCount; f++)
        if (kFields[f].kind == FIELD_BYTES && FieldApplies(kFields[f], category) && Overlaps(offset, kFields[f].offset, kFields[f].size))
            return true;
    return Overlaps(offset, BLD_PAY_OFFSET, 2) || Overlaps(offset, BLD_STOPPED_OFFSET, 1);
}

bool SettingsSync_OwnsSlotWord(const BYTE* building, unsigned slot, unsigned region, unsigned local)
{
    if (region == 2) return (local % SLOT_ENTRY_SIZE) >= 8; // share and enabled of each entry
    if (region != 0 || slot != 0) return false;
    int category = SafeCategory(building);
    for (int f = 0; f < kFieldCount; f++)
        if (kFields[f].kind == FIELD_SLOT0 && FieldApplies(kFields[f], category) && Overlaps(local, kFields[f].offset, kFields[f].size))
            return true;
    return false;
}

void SettingsSync_LockstepBaseline()
{
    g_lsList.resize(200000);
    int n = CopyBuildings(g_lsList.data(), (int)g_lsList.size());
    g_lsBuildings.resize(n);
    g_lsSlotData.clear();
    for (int i = 0; i < n; i++) ReadLsBuilding(g_lsList[i], g_lsBuildings[i]);
}

static void SubmitSetting(const LsBuilding& s, int field, unsigned long long value, int slot, const float* data, int count)
{
    SettingWire w = {};
    w.field = field;
    w.category = s.category;
    memcpy(w.pos, s.pos, sizeof(w.pos));
    w.value = value;
    w.slot = slot;
    w.count = count;
    if (data && count > 0) memcpy(w.data, data, count * 2 * sizeof(float));
    Commands_Submit(LS_CMD_SETTING, &w, sizeof(w));
}

static void RawWrite(BYTE* p, const void* v, int size)
{
    __try {
        memcpy(p, v, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SettingsSync_LockstepDetect()
{
    for (const LsBuilding& old : g_lsBuildings) {
        if (!old.b) continue;
        BYTE* b = old.b;
        int category;
        float pos[3];
        // Gone or replaced since the baseline (a command may have demolished it).
        if (!ReadIdentity(b, &category, pos) || category != old.category || memcmp(pos, old.pos, sizeof(pos)) != 0) continue;
        for (int f = 0; f < kFieldCount; f++) {
            if (!(old.hasMask & (1u << f))) continue;
            unsigned long long v;
            if (!ReadField(b, kFields[f], &v) || v == old.values[f]) continue;
            SubmitSetting(old, f, v, 0, nullptr, 0);
            BYTE* p = FieldAddress(b, kFields[f]);
            if (p) RawWrite(p, &old.values[f], kFields[f].size);
        }
        unsigned short pay;
        unsigned char stopped;
        if (ReadConstruction(b, &pay, &stopped)) {
            if (pay != old.pay) {
                SubmitSetting(old, SETTINGS_FIELD_PAY, pay, 0, nullptr, 0);
                RawWrite(b + BLD_PAY_OFFSET, &old.pay, 2);
            }
            if (stopped != old.stopped) {
                SubmitSetting(old, SETTINGS_FIELD_STOP, stopped, 0, nullptr, 0);
                RawWrite(b + BLD_STOPPED_OFFSET, &old.stopped, 1);
            }
        }
        size_t at = old.slotAt;
        for (int i = 0; i < old.slots; i++) {
            int n = old.slotCount[i];
            if (n < 0) continue;
            float data[SETTINGS_MAX_SLOT_ENTRIES * 2];
            int now = ReadSlot(b, i, data);
            const float* before = g_lsSlotData.data() + at;
            at += n > 0 ? n * 2 : 0;
            if (now != n || n == 0 || memcmp(data, before, n * 2 * sizeof(float)) == 0) {
                if (now != n && now >= 0)
                    Logf("republicmp  SETTINGS: storage slot %d changed its resource list locally - not carried", i);
                continue;
            }
            SubmitSetting(old, SETTINGS_FIELD_SLOT, 0, i, data, n);
            WriteSlot(b, i, before, n);
        }
    }
}

typedef void (*SetPayFn)(__int64 world, __int64 bld, int rub, int usd);
typedef void (*StopFn)(__int64 world, __int64 bld, int stop);

static bool ApplyConstruction(BYTE* b, int field, unsigned long long value)
{
    __int64 world = (__int64)(g_exeBase + WORLD_RVA);
    __try {
        if (field == SETTINGS_FIELD_PAY)
            ((SetPayFn)(g_exeBase + SET_PAY_RVA))(world, (__int64)b, (int)(value & 0xFF), (int)((value >> 8) & 0xFF));
        else if (value)
            ((StopFn)(g_exeBase + STOP_RVA))(world, (__int64)b, 1);
        else
            ((BuildingFn)(g_exeBase + RESUME_RVA))(world, (__int64)b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Applies one received setting; false if its building is not there (yet).
static bool Apply(const SettingWire& w);

bool SettingsSync_ApplyCommand(const SettingWire& w)
{
    if (w.field == SETTINGS_FIELD_PAY || w.field == SETTINGS_FIELD_STOP) {
        BYTE* b = FindBuilding(w.pos, w.category);
        bool ok = b && ApplyConstruction(b, w.field, w.value);
        Logf("republicmp  SETTINGS: %s = %llu at (%.1f,%.1f) %s", w.field == SETTINGS_FIELD_PAY ? "pay rubles/dollars" : "stopped",
             w.value, w.pos[0], w.pos[2], ok ? "applied" : "NOT applied");
        return ok;
    }
    if (w.field == CITIZEN_DESTINATIONS_FIELD) return false;
    return FindBuilding(w.pos, w.category) && Apply(w);
}

// Applies one received setting; false if its building is not there (yet).
static bool Apply(const SettingWire& w)
{
    if (w.field == CITIZEN_DESTINATIONS_FIELD) return CitizenDestinations_Apply(w);
    BYTE* b = FindBuilding(w.pos, w.category);
    if (!b) return false;
    BuildingState& st = g_states[b];
    if (w.field == SETTINGS_FIELD_SLOT) {
        if (w.slot < 0 || w.slot >= MAX_SLOTS || w.count < 0 || w.count > SETTINGS_MAX_SLOT_ENTRIES) return true;
        bool ok = WriteSlot(b, w.slot, w.data, w.count);
        st.slotHash[w.slot] = HashSlot(w.data, w.count); // not echoed back
        st.slotKnown[w.slot] = true;
        Logf("republicmp  SETTINGS: storage slot %d of category 0x%X at (%.1f,%.1f) %s", w.slot, w.category,
             w.pos[0], w.pos[2], ok ? "applied" : "NOT applied (different resource list)");
        return true;
    }
    if (w.field < 0 || w.field >= kFieldCount || !FieldApplies(kFields[w.field], w.category)) return true;
    bool ok = WriteField(b, kFields[w.field], w.value);
    unsigned long long now = 0;
    if (ReadField(b, kFields[w.field], &now)) {
        st.fields[w.field].value = now; // not echoed back
        st.fields[w.field].known = true;
    }
    Logf("republicmp  SETTINGS: [%s] = 0x%llx at (%.1f,%.1f) %s", kFields[w.field].name, w.value, w.pos[0],
         w.pos[2], ok ? "applied" : "FAILED");
    return true;
}

void SettingsSync_OnRemote(const SettingWire& w)
{
    // A newer full list supersedes an older list waiting for an endpoint.
    if (w.field == CITIZEN_DESTINATIONS_FIELD) {
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            const SettingWire& old = it->w;
            if (old.field == w.field && old.slot == w.slot && old.category == w.category &&
                old.value == w.value && !memcmp(old.pos, w.pos, sizeof(w.pos)))
                it = g_pending.erase(it);
            else ++it;
        }
    }
    if (!Apply(w)) {
        if (w.field == CITIZEN_DESTINATIONS_FIELD) CitizenDestinations_LogPending(w, "queued");
        g_pending.push_back({ w, GetTickCount64() });
    }
}

// ---------------------------------------------------------------------------
// Field diff (Debug window): which bytes of a building a click changes
// ---------------------------------------------------------------------------
//
// A research aid for finding window settings not synced yet. Every scan
// copies the first FIELD_DIFF_BYTES of every building; a byte that changes
// while the local player has NOT clicked is marked as simulation noise for
// that building and ignored from then on; a change of a non-noise byte right
// after a click is logged as "FIELD_DIFF". Leave the building alone for a few
// seconds first so its noise is learned, then click the button in question.

#define FIELD_DIFF_BYTES 0x1600
#define FIELD_DIFF_MAX_RANGES 12

struct FieldDiffState {
    std::vector<unsigned char> snap;
    std::vector<unsigned char> noisy;
    bool seen = false;
};
static std::unordered_map<BYTE*, FieldDiffState> g_diff;
static bool g_fieldDiffOn = false;
static ULONGLONG g_lastDiffMs = 0;
static std::vector<BYTE*> g_diffList;

static bool CopyObject(BYTE* b, unsigned char* out)
{
    __try {
        memcpy(out, b, FIELD_DIFF_BYTES);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LogDiffRange(char* line, size_t lineSize, const unsigned char* was, const unsigned char* now, int off,
                         int len)
{
    char part[96];
    int shown = len > 8 ? 8 : len;
    int n = _snprintf_s(part, sizeof(part), _TRUNCATE, " +0x%X", off);
    if (len > 1) n += _snprintf_s(part + n, sizeof(part) - n, _TRUNCATE, "[%d]", len);
    n += _snprintf_s(part + n, sizeof(part) - n, _TRUNCATE, " ");
    for (int i = 0; i < shown; i++) n += _snprintf_s(part + n, sizeof(part) - n, _TRUNCATE, "%02X", was[off + i]);
    n += _snprintf_s(part + n, sizeof(part) - n, _TRUNCATE, "->");
    for (int i = 0; i < shown; i++) n += _snprintf_s(part + n, sizeof(part) - n, _TRUNCATE, "%02X", now[off + i]);
    strncat_s(line, lineSize, part, _TRUNCATE);
}

static void FieldDiffScan(bool byPlayer)
{
    g_diffList.resize(200000);
    int n = CopyBuildings(g_diffList.data(), (int)g_diffList.size());
    for (auto& kv : g_diff) kv.second.seen = false;
    std::vector<unsigned char> cur(FIELD_DIFF_BYTES);
    for (int i = 0; i < n; i++) {
        BYTE* b = g_diffList[i];
        if (!CopyObject(b, cur.data())) continue;
        FieldDiffState& d = g_diff[b];
        d.seen = true;
        if (d.snap.empty()) {
            d.snap = cur;
            d.noisy.assign(FIELD_DIFF_BYTES, 0);
            continue;
        }
        char line[1024] = "";
        int ranges = 0;
        for (int off = 0; off < FIELD_DIFF_BYTES;) {
            if (cur[off] == d.snap[off] || d.noisy[off]) { ++off; continue; }
            int start = off;
            while (off < FIELD_DIFF_BYTES && cur[off] != d.snap[off] && !d.noisy[off]) ++off;
            if (!byPlayer) {
                for (int k = start; k < off; k++) d.noisy[k] = 1; // changed by itself
            } else if (ranges < FIELD_DIFF_MAX_RANGES) {
                LogDiffRange(line, sizeof(line), d.snap.data(), cur.data(), start, off - start);
                ranges++;
            }
        }
        if (ranges) {
            int category;
            float pos[3];
            if (!ReadIdentity(b, &category, pos)) category = -1, pos[0] = pos[2] = 0;
            Logf("republicmp  FIELD_DIFF: category 0x%X at (%.1f,%.1f):%s", category, pos[0], pos[2], line);
        }
        d.snap.swap(cur);
        cur.resize(FIELD_DIFF_BYTES);
    }
    for (auto it = g_diff.begin(); it != g_diff.end();) {
        if (!it->second.seen) it = g_diff.erase(it);
        else ++it;
    }
}

void SettingsSync_SetFieldDiff(bool on)
{
    g_fieldDiffOn = on;
    g_diff.clear();
    Logf("republicmp  FIELD_DIFF: %s", on ? "on - leave the building alone a few seconds, then click" : "off");
}

bool SettingsSync_GetFieldDiff() { return g_fieldDiffOn; }

bool SettingsSync_ClickedRecently() { return ClickedRecently(GetTickCount64()); }

void SettingsSync_Tick()
{
    ULONGLONG now = GetTickCount64();
    TrackClick(now); // every frame - a click can be shorter than the scan interval
    if (g_fieldDiffOn && now - g_lastDiffMs >= SETTINGS_SCAN_INTERVAL_MS) {
        g_lastDiffMs = now;
        FieldDiffScan(ClickedRecently(now));
    }

    bool syncing = BuildSync_IsConnected() && !SyncBlock_IsActive();
    if (!syncing) {
        CitizenDestinations_Reset();
        g_states.clear();
        g_pending.clear();
        return;
    }
    // A setting for a building whose placement is still on its way waits for it.
    for (size_t i = 0; i < g_pending.size();) {
        if (Apply(g_pending[i].w)) {
            g_pending.erase(g_pending.begin() + i);
        } else if (now - g_pending[i].since > SETTINGS_RETRY_MS) {
            if (g_pending[i].w.field == CITIZEN_DESTINATIONS_FIELD)
                CitizenDestinations_LogPending(g_pending[i].w, "timeout - dropped");
            else
                Logf("republicmp  SETTINGS: building for a setting at (%.1f,%.1f) never appeared - dropped",
                     g_pending[i].w.pos[0], g_pending[i].w.pos[2]);
            g_pending.erase(g_pending.begin() + i);
        } else {
            ++i;
        }
    }
    if (now - g_lastScanMs < SETTINGS_SCAN_INTERVAL_MS) return;
    bool first = g_lastScanMs == 0 || g_states.empty();
    g_lastScanMs = now;
    Scan(!first);
}
