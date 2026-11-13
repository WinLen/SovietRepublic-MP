// SPDX-License-Identifier: GPL-3.0-only
// See rename.h. Follows SoylentGreed's rename route (RepublicLAN, read against
// the disassembly of 1.1.1.9).
#include "rename.h"
#include "commands.h"
#include "world_refs.h"
#include "building_switches.h"
#include "../session/two_states.h"
#include "../ui/lang.h"
#include "../core/engine_rva.h"
#include "../core/safe_mem.h"
#include <intrin.h>
#include <math.h>
#include <string.h>

typedef unsigned __int64 U64;

#define CITY_LIST 0x10348
#define CITY_NAME 0x10
#define CITY_POSITION 4
#define CITY_CHARS 0x20
#define MAX_CITIES 4096
#define ROAD_NETWORK 0xF7A0
#define NETWORK_NODES 0x298
#define JUNCTION_OF_NODE 0xC0
#define JUNCTION_NODES 0x88
#define JUNCTION_NAME 4
#define JUNCTION_NAMED 0xD0
#define JUNCTION_CHARS 0x40
#define MAX_NODES 16
#define MAX_CHARS 0x40
#define MAGIC 0x314D414E
#define CITY_EXT_MAGIC 0x43504F53u

static const BYTE kCityCreateExpect[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,
    0x48, 0x89, 0x6C, 0x24, 0x18,
    0x48, 0x89, 0x74, 0x24, 0x20, 0x57
};

static const BYTE kCityDeleteExpect[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,
    0x48, 0x89, 0x6C, 0x24, 0x10,
    0x48, 0x89, 0x74, 0x24, 0x18
};

static const BYTE kCityReassignExpect[15] = {
    0x48, 0x8B, 0xC4,
    0x48, 0x89, 0x50, 0x10,
    0x48, 0x89, 0x48, 0x08,
    0x55, 0x53, 0x56, 0x57
};

static const TsmRoutineSig kRebuildLineNamesSig[] = {
    { REBUILD_STOP_LINE_NAMES_RVA, { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0x9F, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x48 }, 16 },
};
static int g_rebuildLineNamesReady = -1;

#pragma pack(push, 1)
struct RenameMsg {
    unsigned magic, kind, index, check, chars;
    unsigned short text[MAX_CHARS];
};

struct CityPosExt {
    unsigned extMagic;
    float pos[3];
};
#pragma pack(pop)

typedef __int64 (__fastcall *FnCityCreate)(void* world, float* pos, const unsigned short* name);
typedef __int64 (__fastcall *FnCityDelete)(void* rcx, void* rdx, void* r8, void* r9);
typedef void (__fastcall *FnCityReassignBlds)(void* world, float* pos);
static FnCityCreate o_cityCreate = nullptr;
static FnCityDelete o_cityDelete = nullptr;
static FnCityReassignBlds o_cityReassignBlds = nullptr;
static bool g_inReplayCreate = false;
static bool g_inReplayDelete = false;
static bool g_suppressNextCityReassign = false;

static bool ApplyCityCreate(const RenameMsg& m);
static bool ApplyCityDelete(const RenameMsg& m);
static bool ApplyCityReassign(const RenameMsg& m);
static int FindCityIndex(void* p);

static __int64 __fastcall H_CityCreate(void* world, float* pos, const unsigned short* name);
static __int64 __fastcall H_CityDelete(void* rcx, void* rdx, void* r8, void* r9);
static void __fastcall H_CityReassignBlds(void* world, float* pos);

bool Rename_InstallHooks()
{
    if (!g_exeBase) return false;
    int okCreate = InstallInlineHook(g_exeBase + CITY_CREATE_RVA, (void*)&H_CityCreate,
                                     (void**)&o_cityCreate, kCityCreateExpect, sizeof(kCityCreateExpect), "republicmp city_create");
    int okDelete = InstallInlineHook(g_exeBase + CITY_DELETE_RVA, (void*)&H_CityDelete,
                                     (void**)&o_cityDelete, kCityDeleteExpect, sizeof(kCityDeleteExpect), "republicmp city_delete");
    int okReassign = InstallInlineHook(g_exeBase + CITY_REASSIGN_BLDS_RVA, (void*)&H_CityReassignBlds,
                                       (void**)&o_cityReassignBlds, kCityReassignExpect, sizeof(kCityReassignExpect), "republicmp city_reassign");
    Logf("republicmp  CITY: hooks installed (create=%d, delete=%d, reassign=%d)", okCreate, okDelete, okReassign);
    return okDelete != 0;
}

void Rename_Bind(const TsmHost* host)
{
    TsmBind(host);
    Rename_InstallHooks();
}

static BYTE* World() { return g_exeBase + WORLD_RVA; }
static U64 Q(U64 at) { return *(U64*)at; }

static unsigned CityCheck(U64 city) { return Ref_FnvWords((const unsigned*)(city + CITY_POSITION), 3); }

static unsigned NodeCheck(U64 node) { return Ref_NodeCheck(node); }

// A vector of pointers: begin and count (false: not one, or longer than max).
static bool List(U64 at, unsigned max, U64& begin, unsigned& count)
{
    U64 b = Q(at), e = Q(at + 8);
    begin = b;
    count = 0;
    if (!b && !e) return true;
    if (!b || e < b || (e - b) % 8 || (e - b) / 8 > max) return false;
    count = (unsigned)((e - b) / 8);
    return true;
}

static void EmbedCityPosExt(RenameMsg& m, float x, float y, float z)
{
    CityPosExt ext{};
    ext.extMagic = CITY_EXT_MAGIC;
    ext.pos[0] = x;
    ext.pos[1] = y;
    ext.pos[2] = z;
    memcpy(&m.text[CITY_CHARS], &ext, sizeof(ext));
}

static bool ReadCityPosExt(const RenameMsg& m, CityPosExt& out)
{
    memcpy(&out, &m.text[CITY_CHARS], sizeof(out));
    return out.extMagic == CITY_EXT_MAGIC;
}

static const char* KindLabel(unsigned kind)
{
    static const struct {
        unsigned kind;
        const char* label;
    } kLabels[] = {
        { KIND_BUILDING_RENAME, "building"  },
        { KIND_VEHICLE_RENAME,  "vehicle"   },
        { KIND_CITY,            "city"      },
        { KIND_LINE,            "line"      },
        { KIND_CROSSROAD,       "crossroad" },
        { KIND_CITY_REASSIGN,   "city reassign" },
    };
    for (const auto& entry : kLabels) {
        if (entry.kind == kind) return entry.label;
    }
    return "object";
}

static unsigned ExpectedRenameLength(unsigned kind)
{
    if (kind == KIND_CITY) return CITY_CHARS;
    if (kind == KIND_BUILDING_RENAME || kind == KIND_VEHICLE_RENAME ||
        kind == KIND_LINE || kind == KIND_CROSSROAD) {
        return JUNCTION_CHARS;
    }
    return 0;
}

static U64 FindMatchingCity(U64 world, const RenameMsg& m, unsigned* outIndex, bool allowAutoCreate)
{
    U64 begin = 0;
    unsigned n = 0;
    if (!List(world + CITY_LIST, MAX_CITIES, begin, n)) return 0;
    if (m.index < n) {
        U64 city = Q(begin + (U64)m.index * 8);
        if (city && CityCheck(city) == m.check) {
            if (outIndex) *outIndex = m.index;
            return city;
        }
    }
    CityPosExt ext{};
    if (!ReadCityPosExt(m, ext)) return 0;

    for (unsigned i = 0; i < n; i++) {
        U64 city = Q(begin + (U64)i * 8);
        if (city && CityCheck(city) == m.check) {
            if (outIndex) *outIndex = i;
            return city;
        }
    }
    float bestDistSq = 250.0f * 250.0f;
    U64 bestCity = 0;
    unsigned bestIdx = 0;
    for (unsigned i = 0; i < n; i++) {
        U64 city = Q(begin + (U64)i * 8);
        if (!city) continue;
        float dx = *(const float*)(city + CITY_POSITION) - ext.pos[0];
        float dz = *(const float*)(city + CITY_POSITION + 8) - ext.pos[2];
        float d2 = dx * dx + dz * dz;
        if (d2 <= bestDistSq) {
            bestDistSq = d2;
            bestCity = city;
            bestIdx = i;
        }
    }
    if (bestCity) {
        if (outIndex) *outIndex = bestIdx;
        return bestCity;
    }
    if (allowAutoCreate && o_cityCreate && ApplyCityCreate(m)) {
        if (List(world + CITY_LIST, MAX_CITIES, begin, n) && n > 0) {
            U64 created = Q(begin + (U64)(n - 1) * 8);
            if (created) {
                if (outIndex) *outIndex = n - 1;
                return created;
            }
        }
    }
    return 0;
}

// The field's owner: a listed city, building, vehicle, line, or junction.
static bool Identify(U64 field, RenameMsg& m, bool* allowed, U64* outBuildingToRevert)
{
    *allowed = true;
    if (outBuildingToRevert) *outBuildingToRevert = 0;
    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (List(world + CITY_LIST, MAX_CITIES, begin, n)) {
        for (unsigned i = 0; i < n; i++) {
            U64 city = Q(begin + (U64)i * 8);
            if (city && city + CITY_NAME == field) {
                m.kind = KIND_CITY;
                m.index = i;
                m.check = CityCheck(city);
                float cx = *(float*)(city + CITY_POSITION);
                float cy = *(float*)(city + CITY_POSITION + 4);
                float cz = *(float*)(city + CITY_POSITION + 8);
                EmbedCityPosExt(m, cx, cy, cz);
                *allowed = TwoStates_CheckBuildAllowed(cx, cz, Tr("переименование города", "renaming city"));
                return true;
            }
        }
    }
    if (field > 0x114) {
        U64 bld = field - 0x114;
        RouteRef bldRef = kNoRef;
        if (Ref_Building(bld, bldRef)) {
            m.kind = KIND_BUILDING_RENAME;
            m.index = ((bldRef.list & 1u) << 31) | ((unsigned)bldRef.place & 0x7FFFFFFFu);
            m.check = bldRef.check;
            if (outBuildingToRevert) *outBuildingToRevert = bld;
            *allowed = TwoStates_CheckBuildingControlAllowed(
                *(const int*)bld, *(const float*)(bld + 0x350), *(const float*)(bld + 0x358),
                Tr("переименование чужого здания", "renaming foreign building"));
            return true;
        }
    }
    if (field > VEH_NAME_OFFSET) {
        U64 veh = field - VEH_NAME_OFFSET;
        RouteRef vehRef = kNoRef;
        if (Ref_Vehicle(veh, vehRef)) {
            m.kind = KIND_VEHICLE_RENAME;
            m.index = (unsigned)vehRef.place & 0x7FFFFFFFu;
            m.check = vehRef.check;
            *allowed = TwoStates_CheckVehicleControlAllowed(
                (const BYTE*)veh, Tr("переименование чужого транспорта", "renaming foreign vehicle"));
            return true;
        }
    }
    if (field > LINE_NAME_OFFSET) {
        U64 line = field - LINE_NAME_OFFSET;
        RouteRef lineRef = kNoRef;
        if (Ref_Line(line, lineRef)) {
            m.kind = KIND_LINE;
            m.index = ((lineRef.list & 1u) << 31) | ((unsigned)lineRef.place & 0x7FFFFFFFu);
            m.check = lineRef.check;
            *allowed = true;
            return true;
        }
    }
    // A junction: the field is +4 of an object whose first node's +c0 is that object.
    U64 junction = field - JUNCTION_NAME, nodes = 0, roads = Q(world + ROAD_NETWORK);
    unsigned count = 0;
    if (!roads || !List(junction + JUNCTION_NODES, MAX_NODES, nodes, count) || !count) return false;
    U64 first = Q(nodes);
    if (!first || Q(first + JUNCTION_OF_NODE) != junction) return false;
    U64 all = 0;
    unsigned total = 0;
    if (!List(roads + NETWORK_NODES, 10000000, all, total)) return false;
    for (unsigned i = 0; i < total; i++)
        if (Q(all + (U64)i * 8) == first) {
            m.kind = KIND_CROSSROAD;
            m.index = i;
            m.check = NodeCheck(first);
            *allowed = TwoStates_CheckBuildAllowed(*(float*)(first + 4), *(float*)(first + 0xC), Tr("переименование перекрёстка", "renaming junction"));
            return true;
        }
    return false;
}

static bool GuardedIdentify(U64 field, RenameMsg& m, bool* allowed, U64* outBuildingToRevert)
{
    __try {
        return Identify(field, m, allowed, outBuildingToRevert);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CopyText(U64 text, unsigned length, RenameMsg& m)
{
    __try {
        const unsigned short* t = (const unsigned short*)text;
        unsigned n = 0;
        while (n < length && n < MAX_CHARS && t[n]) n++;
        if (n >= length || n >= MAX_CHARS) n = (length < MAX_CHARS ? length : MAX_CHARS) - 1; // as swprintf truncates
        memcpy(m.text, t, n * 2);
        m.chars = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Rename_Capture(U64 field, U64 length, U64 text)
{
    RenameMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    bool allowed = true;
    U64 bldToRevert = 0;
    if (!field || !text || !GuardedIdentify(field, m, &allowed, &bldToRevert)) return false;
    if (!allowed) {
        if (bldToRevert) BuildingSwitches_RevertBuildingName(bldToRevert);
        return true;
    }
    unsigned want = ExpectedRenameLength(m.kind);
    if ((unsigned)length != want || !CopyText(text, want, m)) {
        Logf("republicmp  RENAME: a %s name not shared - unexpected length %u; written on this PC only",
             KindLabel(m.kind), (unsigned)length);
        return false;
    }
    if (bldToRevert) BuildingSwitches_RevertBuildingName(bldToRevert);
    Commands_Submit(LS_CMD_RENAME, &m, sizeof(m));
    return true;
}

static void RefreshOpenWindowsAfterRename(U64 world, U64 target)
{
    U64 begin = 0;
    unsigned count = 0;
    if (!List(world + 0xCAC0, 256, begin, count)) return;
    for (unsigned i = 0; i < count; i++) {
        U64 win = Q(begin + (U64)i * 8);
        if (!win) continue;
        if (target && Q(win + 0x240) == target) {
            *(unsigned*)(win + 0x258) = 0;
        }
        if (*(unsigned*)(win + 0x248) == 8) {
            *(unsigned*)(win + 0x258) = 0;
            *(unsigned*)(win + 0x278) = 0;
        }
    }
}

static void RefreshStopLinesAfterBuildingRename(U64 world, U64 bld)
{
    if (!world || !bld || !g_exeBase) return;
    *(U64*)(world + WORLD_RENAME_REFRESH_DST_OFF) = *(U64*)(world + WORLD_RENAME_REFRESH_SRC_OFF);
    if (!TsmCheckRoutineTable(g_exeBase, &g_rebuildLineNamesReady, kRebuildLineNamesSig, 1)) return;
    U64 linesMgr = Q(world + WORLD_LINES_OFFSET);
    if (linesMgr) {
        typedef void (__fastcall* RebuildStopLineNamesFn)(void* linesMgr, void* bld);
        ((RebuildStopLineNamesFn)(g_exeBase + REBUILD_STOP_LINE_NAMES_RVA))((void*)linesMgr, (void*)bld);
    }
}

static const char* Apply(const RenameMsg& m)
{
    U64 world = (U64)World(), begin = 0, field = 0, flag = 0, target = 0;
    unsigned n = 0;
    BYTE flagValue = 1;
    if (m.kind == KIND_CITY) {
        U64 city = FindMatchingCity(world, m, nullptr, true);
        if (!city) return "the city is not here";
        field = city + CITY_NAME;
        target = city;
    } else if (m.kind == KIND_BUILDING_RENAME) {
        RouteRef ref = { (m.index >> 31) & 1u, (int)(m.index & 0x7FFFFFFFu), m.check };
        U64 bld = Ref_BuildingAt(ref);
        if (!bld) return "the building is not here";
        field = bld + 0x114;
        target = bld;
    } else if (m.kind == KIND_VEHICLE_RENAME) {
        RouteRef ref = { 0, (int)(m.index & 0x7FFFFFFFu), m.check };
        U64 veh = Ref_VehicleAt(ref);
        if (!veh) return "the vehicle is not here";
        field = veh + VEH_NAME_OFFSET;
        flag = veh + VEH_CUSTOM_NAME_FLAG_OFF;
        flagValue = (m.chars > 0) ? 1 : 0;
        target = veh;
    } else if (m.kind == KIND_LINE) {
        RouteRef ref = { (m.index >> 31) & 1u, (int)(m.index & 0x7FFFFFFFu), m.check };
        U64 line = Ref_LineAt(ref);
        if (!line) return "the line is not here";
        field = line + LINE_NAME_OFFSET;
        flag = line + LINE_CUSTOM_NAME_FLAG_OFF;
        target = line;
    } else if (m.kind == KIND_CROSSROAD) {
        U64 roads = Q(world + ROAD_NETWORK);
        if (!roads || !List(roads + NETWORK_NODES, 10000000, begin, n) || m.index >= n) return "the crossroad is not here";
        U64 node = Q(begin + (U64)m.index * 8);
        if (!node || NodeCheck(node) != m.check) return "the crossroad is not here";
        U64 junction = Q(node + JUNCTION_OF_NODE);
        if (!junction) return "the node has no crossroad here";
        field = junction + JUNCTION_NAME;
        flag = junction + JUNCTION_NAMED;
        target = junction;
    } else {
        return "unknown rename target";
    }
    const size_t charCount = (m.chars <= MAX_CHARS) ? (size_t)m.chars : (size_t)MAX_CHARS;
    unsigned short* dst = (unsigned short*)field;
    for (size_t i = 0; i < charCount; ++i) {
        dst[i] = m.text[i];
    }
    dst[charCount] = 0;
    if (flag) *(BYTE*)flag = flagValue;
    if (m.kind == KIND_BUILDING_RENAME) {
        RefreshStopLinesAfterBuildingRename(world, target);
    } else if (m.kind == KIND_LINE) {
        *(U64*)(world + WORLD_RENAME_REFRESH_DST_OFF) = *(U64*)(world + WORLD_RENAME_REFRESH_SRC_OFF);
    }
    RefreshOpenWindowsAfterRename(world, target);
    return nullptr;
}

static __int64 __fastcall H_CityCreate(void* world, float* pos, const unsigned short* name)
{
    if (g_inReplayCreate || !Commands_TakeLocal()) {
        return o_cityCreate ? o_cityCreate(world, pos, name) : 0;
    }
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    if (!pos || !SafeReadPod(&px, pos) || !SafeReadPod(&py, pos + 1) || !SafeReadPod(&pz, pos + 2)) {
        return 0;
    }
    if (!TwoStates_CheckBuildAllowed(px, pz, Tr("создание города", "creating city"))) {
        g_suppressNextCityReassign = true;
        Logf("republicmp  CITY: create blocked by border check at (%.1f, %.1f)", px, pz);
        return 0;
    }
    RenameMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.kind = KIND_CITY_CREATE;
    if (name) {
        CopyText((U64)name, CITY_CHARS, m);
    }
    EmbedCityPosExt(m, px, py, pz);
    m.check = Ref_FnvWords((const unsigned*)pos, 3);
    g_suppressNextCityReassign = true;
    Commands_Submit(LS_CMD_RENAME, &m, sizeof(m));
    Logf("republicmp  CITY: captured create of city at (%.1f, %.1f, %.1f)", px, py, pz);
    return 0;
}

static void __fastcall H_CityReassignBlds(void* world, float* pos)
{
    if (!g_inReplayCreate && g_suppressNextCityReassign) {
        g_suppressNextCityReassign = false;
        return;
    }
    if (Commands_Capturing() && !g_inReplayCreate) {
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        if (pos && SafeReadPod(&px, pos) && SafeReadPod(&py, pos + 1) && SafeReadPod(&pz, pos + 2)) {
            RenameMsg m;
            memset(&m, 0, sizeof(m));
            m.magic = MAGIC;
            m.kind = KIND_CITY_REASSIGN;
            EmbedCityPosExt(m, px, py, pz);
            Commands_Submit(LS_CMD_RENAME, &m, sizeof(m));
            Logf("republicmp  CITY: captured reassign buildings for city at (%.1f, %.1f, %.1f)", px, py, pz);
            return;
        }
    }
    if (o_cityReassignBlds) {
        o_cityReassignBlds(world, pos);
    }
}

static bool ApplyCityReassign(const RenameMsg& m)
{
    CityPosExt ext{};
    if (!ReadCityPosExt(m, ext) || !o_cityReassignBlds) return false;
    U64 world = (U64)World();
    g_inReplayCreate = true;
    __try {
        o_cityReassignBlds((void*)world, ext.pos);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_inReplayCreate = false;
    Logf("republicmp  CITY: reassigned buildings for city at (%.1f, %.1f, %.1f)", ext.pos[0], ext.pos[1], ext.pos[2]);
    return true;
}

static bool ApplyCityCreate(const RenameMsg& m)
{
    if (m.chars >= CITY_CHARS) return false;
    CityPosExt ext{};
    if (!ReadCityPosExt(m, ext) || !o_cityCreate) return false;

    unsigned short nameBuf[CITY_CHARS + 1] = {};
    memcpy(nameBuf, m.text, m.chars * sizeof(unsigned short));

    U64 world = (U64)World();
    g_inReplayCreate = true;
    __try {
        o_cityCreate((void*)world, ext.pos, nameBuf);
        if (o_cityReassignBlds) {
            o_cityReassignBlds((void*)world, ext.pos);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_inReplayCreate = false;
        return false;
    }
    g_inReplayCreate = false;
    Logf("republicmp  CITY: created city at (%.1f, %.1f, %.1f)", ext.pos[0], ext.pos[1], ext.pos[2]);
    return true;
}

static int FindCityIndex(void* p)
{
    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (!List(world + CITY_LIST, MAX_CITIES, begin, n) || !n) return -1;
    U64 target = (U64)p;
    for (unsigned i = 0; i < n; i++) {
        if (Q(begin + (U64)i * 8) == target) return (int)i;
    }
    if (target < n) return (int)target;
    if (target > 0x10000 && (target & 0x7) == 0) {
        __try {
            const U64* words = (const U64*)target;
            for (int w = 0; w < 64; ++w) {
                U64 candidate = words[w];
                for (unsigned i = 0; i < n; i++) {
                    if (Q(begin + (U64)i * 8) == candidate) return (int)i;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return -1;
}

static __int64 __fastcall H_CityDelete(void* rcx, void* rdx, void* r8, void* r9)
{
    if (g_inReplayDelete || !Commands_TakeLocal()) {
        return o_cityDelete ? o_cityDelete(rcx, rdx, r8, r9) : 0;
    }
    int idx = FindCityIndex(rcx);
    if (idx < 0) idx = FindCityIndex(rdx);
    if (idx < 0) {
        Logf("republicmp  CITY: delete for unknown target=%p, local only", rcx);
        return o_cityDelete ? o_cityDelete(rcx, rdx, r8, r9) : 0;
    }

    U64 world = (U64)World(), begin = 0;
    unsigned n = 0;
    if (!List(world + CITY_LIST, MAX_CITIES, begin, n) || (unsigned)idx >= n) return 0;
    U64 city = Q(begin + (U64)idx * 8);
    if (!city) return 0;

    float cx = *(float*)(city + CITY_POSITION);
    float cy = *(float*)(city + CITY_POSITION + 4);
    float cz = *(float*)(city + CITY_POSITION + 8);
    bool allowed = TwoStates_CheckBuildAllowed(cx, cz, Tr("удаление города", "deleting city"));
    if (!allowed) {
        Logf("republicmp  CITY: delete blocked by border check");
        return 0;
    }

    RenameMsg m;
    memset(&m, 0, sizeof(m));
    m.magic = MAGIC;
    m.kind = KIND_CITY_DELETE;
    m.index = (unsigned)idx;
    m.check = CityCheck(city);
    EmbedCityPosExt(m, cx, cy, cz);

    Commands_Submit(LS_CMD_CITY_DELETE, &m, sizeof(m));
    Logf("republicmp  CITY: captured delete of city #%d", idx);
    return 0;
}

static bool ApplyCityDelete(const RenameMsg& m)
{
    U64 world = (U64)World();
    unsigned matchIdx = m.index;
    U64 city = FindMatchingCity(world, m, &matchIdx, false);
    if (!city) return false;

    if (o_cityDelete) {
        g_inReplayDelete = true;
        __try {
            o_cityDelete((void*)world, (void*)city, nullptr, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_inReplayDelete = false;
    }

    U64 curBegin = 0;
    unsigned curCount = 0;
    if (List(world + CITY_LIST, MAX_CITIES, curBegin, curCount) && matchIdx < curCount) {
        if (Q(curBegin + (U64)matchIdx * 8) == city) {
            U64* ptrs = (U64*)curBegin;
            for (unsigned j = matchIdx; j + 1 < curCount; j++) {
                ptrs[j] = ptrs[j + 1];
            }
            *(U64*)(world + CITY_LIST + 8) = curBegin + (U64)(curCount - 1) * 8;
        }
    }
    Logf("republicmp  CITY: deleted city at index %u", matchIdx);
    return true;
}

bool Rename_ApplyDeleteCommand(const void* data, size_t size)
{
    if (size != sizeof(RenameMsg)) return false;
    RenameMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC || m.kind != KIND_CITY_DELETE) return false;
    return ApplyCityDelete(m);
}

bool Rename_ApplyCommand(const void* data, size_t size)
{
    if (size == sizeof(LsResearchCommand)) {
        return TwoStates_ApplyResearchCommand(*(const LsResearchCommand*)data);
    }
    if (size != sizeof(RenameMsg)) return false;
    RenameMsg m;
    memcpy(&m, data, sizeof(m));
    if (m.magic != MAGIC) return false;
    if (m.kind == KIND_CITY_CREATE) {
        return ApplyCityCreate(m);
    }
    if (m.kind == KIND_CITY_DELETE) {
        return ApplyCityDelete(m);
    }
    if (m.kind == KIND_CITY_REASSIGN) {
        return ApplyCityReassign(m);
    }
    if (m.kind == KIND_RESEARCH) {
        return TwoStates_ApplyResearchCommand(*(const LsResearchCommand*)m.text);
    }
    unsigned limit = ExpectedRenameLength(m.kind);
    if (!limit || m.chars >= limit) return false;
    const char* why;
    __try {
        why = Apply(m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        why = "a fault while writing";
    }
    Logf("republicmp  RENAME: %s name (%u characters): %s", KindLabel(m.kind), m.chars, why ? why : "made");
    return why == nullptr;
}
