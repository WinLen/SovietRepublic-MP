// SPDX-License-Identifier: GPL-3.0-only
// See road_sync.h for the overall design.
#include "road_sync.h"
#include "../../lockstep/commands.h"
#include "../../transport/network.h"
#include "../../ui/debug_log.h"
#include "../local_builds.h" // LocalBuilds_IsToolDragActive, g_origRoadCommit
#include "../terrain/terrain_sync.h"            // TerrainSync_Begin/EndForeignReplay
#include "../../ui/sync_block.h"
#include "../view_list_scope.h"
#include "../replay_menus.h"
#include "../../determinism/determinism.h"
#include "../../session/two_states.h"
#include "../../session/save_sync.h"
#include "../../ui/native_menu.h"
#include "../../ui/lang.h"
#include "../../core/engine_rva.h"
#include "../../core/safe_mem.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#include <vector>
#include <unordered_map>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "User32.lib")

// Set to 1 to log every transmitted/received point at full precision.
#define ROAD_SYNC_VERBOSE_LOG 0

// ---------------------------------------------------------------------------
// Engine addresses (RVAs into SOVIET64.exe) and object offsets
// ---------------------------------------------------------------------------

#define FC040_RVA 0x4FC040                // drag start
#define FCAF0_RVA 0x4FCAF0                // drag tick
#define COMMIT_RVA 0x518A80               // commit
#define VEC_ASSIGN_RVA 0x21D2B0           // std::vector<24-byte record>::operator=
#define MANAGER_RESET_RVA 0x542A30        // FUN_140542a30(manager, manager+0x408): clears the tool's preview state
#define ROAD_DEMOLISH_TYPE 0x65           // fcaf0 type argument of the demolish tool

#define LMB_HELD_RVA 0xA54E90             // byte: left mouse button held
#define LMB_JUST_PRESSED_RVA 0xA54E91     // byte: left mouse button pressed this frame
#define PRESET_CURVE_MODE_RVA 0x9E8088    // int: preset curve index, -1 = free curve

// Hover state the tools read from the local mouse.
//
// 0x9E4590 / 0x9E45A0: the road segment under the cursor. The footpath tool
// (manager+0x258 == 4) copies them at drag start (fc040, 0x4FC650) into
// manager+0x360 / +0x370, and its commit (0x51C9F6) then joins the path to
// that road segment, looking it up in the road manager's list (FUN_14053d510)
// and indexing with the result. Left set, a replayed footpath was joined to
// whatever road the LOCAL player happened to point at - and when that
// pointer was stale (the segment rebuilt meanwhile), the lookup gave -1 and
// the commit crashed half-way (0x56DFE4 via FUN_1405446d0), leaving the tool
// broken and the game crashing on the next frame.
static const unsigned int kHoverPtrRvas[] = { 0x9E45E8, 0x9E4560, 0x9E4558, 0x9E45E0, 0x9E45D8,
                                              0x9E4590, 0x9E45A0 };
#define HOVER_FLAG_RVA 0x9E45F8
// The footpath tool's road link (see RoadSync_ReadFootpathLinks).
#define HOVER_ROAD_EDGE_RVA 0x9E4590   // road edge under the cursor
#define HOVER_ROAD_POINT_RVA 0x9E4598  // int: its point the path would split it at
#define HOVER_ROAD_NODE_RVA 0x9E45A0   // road node under the cursor
#define MGR_ROAD_LINK_POINT_OFFSET 0x368
#define ROAD_NETWORK_OFFSET 0xf7a0      // world+0xf7a0 = DAT_1409e46b0, the road network
#define NET_NODES_OFFSET 0x298          // committed nodes: begin, end
#define HOVER_NODE_RVA 0x9E4560  // thisPtr+0xf650: hovered path node (fcaf0 DAT_1409e4560)
#define HOVER_PORT_BUILDING_RVA 0x9E45B0  // building whose connection port is hovered
#define HOVER_PORT_INDEX_RVA 0x9E45B8     // that port's index

// thisPtr fields used by the tool wrappers (TOOL_GUARD1/2/3_OFFSET in engine_rva.h).
#define CONTINUING_Y_OFFSET_OFFSET 0xf90  // added to cursor Y on drag ticks
#define SNAP_TARGET_PTR_OFFSET 0xf6d8     // drag-start snap target
#define SNAP_MODE_A_OFFSET 0xf658
#define SNAP_MODE_B_OFFSET 0xf668

// Manager fields.
#define MGR_CURVE_OFFSET 0x48
#define MGR_RELAX_TIMER_OFFSET 0x4c
#define MGR_VALID_OFFSET 0x315            // byte: 1 = current route is buildable
#define MGR_ERROR_CATEGORY_OFFSET 0x318   // int: -1 = no error
#define MGR_ERROR_SUBCODE_OFFSET 0x31c
#define MGR_HOVER_SEGMENT_COPY_OFFSET 0x3b8
#define MGR_ROAD_LINK_OFFSET 0x360        // footpath tool: road segment the path joins (fc040 copies 0x9E4590)
#define MGR_ROAD_LINK_EXTRA_OFFSET 0x370  // ... and 0x9E45A0
#define MGR_START_NODE_OFFSET 0x428
#define MGR_END_NODE_OFFSET 0x430
#define MGR_START_ATTACHED_OFFSET 0x748   // byte: start is an existing node
#define MGR_END_ATTACHED_OFFSET 0x749     // byte: end snapped to an existing node
// The network's committed edges. (+0xaa0/+0xaa8 is only what the local
// camera sees - lookups there missed every piece the player looked away
// from; see view_list_scope.h.)
#define MGR_SEGMENTS_BEGIN_OFFSET 0x2b0
#define MGR_SEGMENTS_END_OFFSET 0x2b8
// Upgrade branch of the commit (FUN_140518a80 line 303).
#define MGR_UPG_SEGMENT_OFFSET 0x380
#define MGR_UPG_RANGE_START_OFFSET 0x388
#define MGR_UPG_RANGE_END_OFFSET 0x38c
#define MGR_UPG_GATE_OFFSET 0x390
#define MGR_UPG_OLD_TYPE_OFFSET 0x394
#define MGR_UPG_OLD_VARIANT_OFFSET 0x398
#define MGR_UPG_DIR_OFFSET 0x39c
#define MGR_UPG_MULTI_OFFSET 0x3d0
#define MGR_TYPE_OFFSET 0x528
#define MGR_VARIANT_OFFSET 0x52c
#define MGR_FLAG_5C4_OFFSET 0x5c4

// Segment object.
#define SEG_POINTS_BEGIN_OFFSET 0x08
#define SEG_POINTS_END_OFFSET 0x10
#define SEG_NODE0_OFFSET 0x20
#define SEG_NODE1_OFFSET 0x28
#define SEG_TYPE_OFFSET 0x120
#define SEG_VARIANT_OFFSET 0x124

#define NODE_POS_OFFSET 4                 // node object: float x,y,z
#define BUILDING_POS_OFFSET 0x350         // building object: float x,y,z

typedef void (*ToolWrapperFn)(__int64 thisPtr, int typeId, unsigned int variant);
typedef void (*Fc040Fn)(__int64 manager, float* startPoint, unsigned __int64 p3, char fuzzyMatch,
                        int p5, unsigned __int64 existingNode, float* directionHint);
typedef void (*Fcaf0Fn)(__int64 manager, const float* cursor, int typeId, unsigned int variant,
                        unsigned char p5, unsigned char p6, unsigned char p7,
                        __int64 routeVector, unsigned char p9, unsigned char p10);
typedef __int64* (*VecAssignFn)(__int64* dst, const void* srcVectorHeader);
typedef void (*ManagerResetFn)(__int64 manager, __int64 state);

// ---------------------------------------------------------------------------
// Categories
// ---------------------------------------------------------------------------

// Manager offsets and wrapper RVAs come from each wrapper's decompile; tool
// names from the strcmp chain in FUN_14030d1a0. FACTORYCONN: drawing
// ("factoryconnection", wrapper mode = type id 0) and its demolish mode (100)
// drive the primitives exactly like the footpath wrapper. Only the
// factoryconnection_oneway mode (0x32) differs - it hit-tests existing
// connections (FUN_140546db0) and changes their direction without a commit;
// that travels as RoadSyncDirectionMsg.
static RoadSyncCategoryInfo g_categories[ROAD_SYNC_CATEGORY_COUNT] = {
    { "ROAD",        0xf7a0, true  },
    { "PIPE",        0xf858, true  },
    { "WATERPIPE",   0xfaf0, true  },
    { "HEATPIPE",    0xfa98, true  },
    { "RAIL",        0xf908, true  },
    { "PEDESTRIAN",  0xf968, true  },
    { "FENCE",       0xf8b0, true  },
    { "POWER_HIGH",  0xfa18, true  },
    { "POWER_LOW",   0xfa20, true  },
    { "CONVEYOR",    0xf800, true  },
    { "CABLEWAY",    0xfa40, true  },
    { "AIRROAD",     0xfa30, true  },
    { "FACTORYCONN", 0xf9c0, true  },
    // The second-road network (pavements and decoration paths: second_road_airplane type 0,
    // _basicroad 1, _l2 2, _l3 -2, _l4 -3). Its handler 3cf6f0 begins on the press and finishes
    // on the release; after a finish that appended an edge it starts the edge's construction
    // (5247f0) and clears the ground under it (3ae120) itself, so the replay's release through
    // the handler makes those steps too. (by SoylentGreed)
    { "SECOND_ROAD", 0xf7a8, true  },
};

static const unsigned int g_categoryWrapperRva[ROAD_SYNC_CATEGORY_COUNT] = {
    0x3D00F0, 0x3B0E60, 0x3B12B0, 0x3B4230, 0x3D4730, 0x3D68B0, 0x3B3820,
    0x3B8780, 0x3BAED0, 0x3AEC30, 0x3CBFA0, 0x3C9130, 0x3D6E70, 0x3CF6F0,
};

const RoadSyncCategoryInfo* RoadSync_GetCategoryInfo(int category)
{
    if (category < 0 || category >= ROAD_SYNC_CATEGORY_COUNT) return nullptr;
    return &g_categories[category];
}

int RoadSync_CategoryForManager(__int64 thisPtr, __int64 manager)
{
    if (!thisPtr || !manager) return -1;
    __try {
        for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; c++) {
            if (g_categories[c].enabled && *(__int64*)(thisPtr + g_categories[c].managerOffset) == manager)
                return c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

int RoadSync_SegmentCount(__int64 thisPtr, int category)
{
    if (category < 0 || category >= ROAD_SYNC_CATEGORY_COUNT || !thisPtr) return -1;
    __try {
        __int64 manager = *(__int64*)(thisPtr + g_categories[category].managerOffset);
        if (!manager) return -1;
        __int64 begin = *(__int64*)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64 end = *(__int64*)(manager + MGR_SEGMENTS_END_OFFSET);
        return (begin && end >= begin) ? (int)((end - begin) / 8) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static const char* CategoryName(int category)
{
    if (category == ROAD_SYNC_GROUP_TOOL_CATEGORY) return "GROUP TOOL";
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(category);
    return info ? info->name : "?";
}

// fc040's 5th argument as each wrapper passes it: 0xffffffec, except the two
// power-line wrappers, which pass the type id (fc040 compares it with 0x14).
static int Fc040Param5ForCategory(int category, int typeId)
{
    if (category == ROAD_SYNC_CATEGORY_POWER_HIGH || category == ROAD_SYNC_CATEGORY_POWER_LOW)
        return typeId;
    return (int)0xffffffec;
}

// ---------------------------------------------------------------------------
// Module state and small helpers
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_queueLock;
static volatile LONG g_applyingSegment = 0;

void RoadSync_Bind(const TsmHost* host)
{
    TsmBind(host);
    InitializeCriticalSection(&g_queueLock);
}

// Logs to the file log and to the Debug window.
static void RoadSyncStatus(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    DebugLog_FormatStatus(Logf, "ROAD_SYNC", fmt, ap);
    va_end(ap);
}

static __int64 GetWorldThisPtr()
{
    return (__int64)((uintptr_t)g_exe + UNIVERSAL_THIS_RVA);
}

static bool IsWorldLoaded()
{
    return SafeIsWorldLoaded(g_exeBase);
}

bool RoadSync_IsApplyingSegment()
{
    return InterlockedCompareExchange(&g_applyingSegment, 0, 0) != 0;
}

// True while the tool wrappers' guard bytes are set (some blocking UI is
// open); the wrappers silently do nothing then, so applying must wait.
static bool ToolWrapperGuardBusy(__int64 thisPtr)
{
    return SafeIsToolGuardBusy((uintptr_t)thisPtr);
}

// Clears the local mouse-hover globals so a replay never snaps to whatever
// the local player is pointing at. Not restored afterwards: the commit may
// delete the hovered object, and the game recomputes hover next frame.
static void HoverGlobals_Clear()
{
    __try {
        for (size_t i = 0; i < sizeof(kHoverPtrRvas) / sizeof(kHoverPtrRvas[0]); i++)
            *(long long*)((uintptr_t)g_exe + kHoverPtrRvas[i]) = 0;
        *(unsigned char*)((uintptr_t)g_exe + HOVER_FLAG_RVA) = 0;
        *(unsigned char*)((uintptr_t)g_exe + 0x9E4568) = 0;
        memset((void*)((uintptr_t)g_exe + 0x9E456C), 0, 12);
        *(unsigned char*)((uintptr_t)g_exe + 0x9E4578) = 0;
        memset((void*)((uintptr_t)g_exe + 0x9E457C), 0, 12);
        *(unsigned char*)((uintptr_t)g_exe + 0x9E45A8) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void RoadSync_ClearHover()
{
    HoverGlobals_Clear();
    __try {
        *(long long*)((uintptr_t)g_exe + HOVER_PORT_BUILDING_RVA) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Local building at `pos` (0.5m in x/z, 1m in y), or 0.
static __int64 FindLocalBuildingAt(const float pos[3])
{
    __try {
        const unsigned int kBeginRvas[2] = { VEC_BUILDINGS_BEGIN_RVA, VEC_CONSTRUCTIONS_BEGIN_RVA };
        const unsigned int kEndRvas[2]   = { VEC_BUILDINGS_END_RVA,   VEC_CONSTRUCTIONS_END_RVA };
        for (int v = 0; v < 2; ++v) {
            __int64* it = *(__int64**)((uintptr_t)g_exeBase + kBeginRvas[v]);
            __int64* end = *(__int64**)((uintptr_t)g_exeBase + kEndRvas[v]);
            for (; it && it < end; ++it) {
                __int64 b = *it;
                if (!b) continue;
                const float* p = (const float*)(b + BUILDING_POS_OFFSET);
                if (fabsf(p[0] - pos[0]) < 0.5f && fabsf(p[2] - pos[2]) < 0.5f && fabsf(p[1] - pos[1]) < 1.0f)
                    return b;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// Sets the hovered-port globals to the local copy of `port`'s building, or
// clears them when the end is not on a building.
static void SetHoveredPort(const RoadSyncBuildingPort& port, const char* which)
{
    __int64 bld = 0;
    if (port.valid) {
        bld = FindLocalBuildingAt(port.bldPos);
        Logf("republicmp  ROAD_SYNC:   %s port: building at (%.2f,%.2f,%.2f) port=%d -> %s",
             which, port.bldPos[0], port.bldPos[1], port.bldPos[2], port.portIndex,
             bld ? "found" : "NOT FOUND (end left unconnected)");
    }
    __try {
        *(long long*)((uintptr_t)g_exe + HOVER_PORT_BUILDING_RVA) = bld;
        if (bld) *(int*)((uintptr_t)g_exe + HOVER_PORT_INDEX_RVA) = port.portIndex;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void ClearHoveredPort()
{
    __try {
        *(long long*)((uintptr_t)g_exe + HOVER_PORT_BUILDING_RVA) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Writes a position into a node object.
static void SetNodePos(__int64 node, const float pos[3])
{
    if (!node) return;
    float* p = (float*)(node + NODE_POS_OFFSET);
    p[0] = pos[0]; p[1] = pos[1]; p[2] = pos[2];
}

static bool NodeNear(__int64 node, const float pos[3])
{
    const float* p = (const float*)(node + NODE_POS_OFFSET);
    return fabsf(p[0] - pos[0]) < 0.5f && fabsf(p[2] - pos[2]) < 0.5f && fabsf(p[1] - pos[1]) < 1.0f;
}

static bool IsPowerLine(int category)
{
    return category == ROAD_SYNC_CATEGORY_POWER_HIGH || category == ROAD_SYNC_CATEGORY_POWER_LOW;
}

// The committed node of `network` the drag tick would take for an end at
// `pos` (its own test, 0x4FEFF0: within 1m on each axis), nearest first; 0
// if none.
static __int64 FindNetworkNodeAt(__int64 network, const float pos[3])
{
    __int64 best = 0;
    float bestD = 0.0f;
    __try {
        __int64* it = *(__int64**)(network + NET_NODES_OFFSET);
        __int64* end = *(__int64**)(network + NET_NODES_OFFSET + 8);
        for (; it && it < end; ++it) {
            __int64 node = *it;
            if (!node) continue;
            const float* p = (const float*)(node + NODE_POS_OFFSET);
            float dx = fabsf(p[0] - pos[0]), dy = fabsf(p[1] - pos[1]), dz = fabsf(p[2] - pos[2]);
            if (dx >= 1.0f || dy >= 1.0f || dz >= 1.0f) continue;
            float d = dx * dx + dy * dy + dz * dz;
            if (!best || d < bestD) {
                best = node;
                bestD = d;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return best;
}

// A power-line drag end that took an existing node must sit where the sender's
// did; logs and puts it back if the engine moved it anyway.
static void CheckAttachedPowerNode(__int64 node, const float want[3], const char* which)
{
    if (!node) return;
    const float* p = (const float*)(node + NODE_POS_OFFSET);
    if (fabsf(p[1] - want[1]) <= 0.5f) return;
    Logf("republicmp  ROAD_SYNC:   WARNING %s node at (%.2f,%.2f,%.2f), the sender's at (%.2f,%.2f,%.2f) - put back",
         which, p[0], p[1], p[2], want[0], want[1], want[2]);
    SetNodePos(node, want);
}

// ---------------------------------------------------------------------------
// Footpath road links & Own-network hover links (rail switches, T-junctions)
// ---------------------------------------------------------------------------

static bool InPointerList(__int64 list, __int64 p)
{
    __int64* it = *(__int64**)list;
    __int64* end = *(__int64**)(list + 8);
    for (; it && it < end; ++it)
        if (*it == p) return true;
    return false;
}

static __int64 RoadNetwork()
{
    return *(__int64*)(GetWorldThisPtr() + ROAD_NETWORK_OFFSET);
}

// Caller wraps in __try.
static void ReadNetworkLink(__int64 network, __int64 edge, int pointIndex, __int64 node, RoadSyncRoadLink& out)
{
    memset(&out, 0, sizeof(out));
    if (!network) return;
    if (edge && InPointerList(network + MGR_SEGMENTS_BEGIN_OFFSET, edge)) {
        __int64 n0 = *(__int64*)(edge + SEG_NODE0_OFFSET);
        __int64 n1 = *(__int64*)(edge + SEG_NODE1_OFFSET);
        if (n0 && n1) {
            out.hasEdge = 1;
            out.pointIndex = pointIndex;
            memcpy(out.edgeA, (const float*)(n0 + NODE_POS_OFFSET), 12);
            memcpy(out.edgeB, (const float*)(n1 + NODE_POS_OFFSET), 12);
            out.edgePoints = (int)((*(__int64*)(edge + SEG_POINTS_END_OFFSET) -
                                    *(__int64*)(edge + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
        }
    }
    if (node && InPointerList(network + NET_NODES_OFFSET, node)) {
        out.hasNode = 1;
        memcpy(out.node, (const float*)(node + NODE_POS_OFFSET), 12);
    }
}

static void ReadLink(__int64 edge, int pointIndex, __int64 node, RoadSyncRoadLink& out)
{
    ReadNetworkLink(RoadNetwork(), edge, pointIndex, node, out);
}

void RoadSync_ReadFootpathLinks(__int64 manager, int category, RoadSyncSegment& seg)
{
    memset(&seg.startLink, 0, sizeof(seg.startLink));
    memset(&seg.endLink, 0, sizeof(seg.endLink));
    memset(&seg.startOwnLink, 0, sizeof(seg.startOwnLink));
    memset(&seg.endOwnLink, 0, sizeof(seg.endOwnLink));
    seg.snapModeStart = 0;
    seg.snapModeEnd = 0;
    seg.needsTerrainLevel = 0;
    seg.snapFlags = 0;
    memset(seg.snapOffsetStart, 0, sizeof(seg.snapOffsetStart));
    memset(seg.snapOffsetEnd, 0, sizeof(seg.snapOffsetEnd));

    __try {
        // Own-network hover links for ALL categories (rail switches, mid-segment T-junctions):
        // fc040 copies the start hover edge/node from 0x9E4558/0x9E45D8 & 0x9E4560/0x9E45E0
        // into manager+0x340 (edge) and manager+0x348 (node). At commit time, the end hover
        // edge/node is still in 0x9E4558/0x9E45D8 & 0x9E4560/0x9E45E0.
        __int64 startOwnEdge = *(__int64*)(manager + 0x340);
        __int64 startOwnNode = *(__int64*)(manager + 0x348);
        ReadNetworkLink(manager, startOwnEdge, 0, startOwnNode, seg.startOwnLink);

        __int64 endOwnEdge = *(__int64*)((uintptr_t)g_exe + 0x9E4558);
        if (!endOwnEdge || !InPointerList(manager + MGR_SEGMENTS_BEGIN_OFFSET, endOwnEdge))
            endOwnEdge = *(__int64*)((uintptr_t)g_exe + 0x9E45D8);
        __int64 endOwnNode = *(__int64*)((uintptr_t)g_exe + 0x9E4560);
        if (!endOwnNode || !InPointerList(manager + NET_NODES_OFFSET, endOwnNode))
            endOwnNode = *(__int64*)((uintptr_t)g_exe + 0x9E45E0);
        ReadNetworkLink(manager, endOwnEdge, 0, endOwnNode, seg.endOwnLink);

        seg.snapModeStart = *(unsigned char*)(manager + 0x74A);
        seg.snapModeEnd   = *(unsigned char*)(manager + 0x74B);
        seg.needsTerrainLevel = *(unsigned char*)(manager + 0x312);
        seg.snapFlags = (*(unsigned char*)(manager + 0x748) ? 0x01 : 0) |
                        (*(unsigned char*)(manager + 0x749) ? 0x02 : 0) |
                        (*(unsigned char*)(manager + 0x5CB) ? 0x04 : 0) |
                        (*(unsigned char*)(manager + 0x5CC) ? 0x08 : 0);
        memcpy(seg.snapOffsetStart, (const void*)(manager + 0x74C), 12);
        memcpy(seg.snapOffsetEnd,   (const void*)(manager + 0x758), 12);

        if (category == ROAD_SYNC_CATEGORY_PEDESTRIAN) {
            ReadLink(*(__int64*)(manager + MGR_ROAD_LINK_OFFSET), *(int*)(manager + MGR_ROAD_LINK_POINT_OFFSET),
                     *(__int64*)(manager + MGR_ROAD_LINK_EXTRA_OFFSET), seg.startLink);
            ReadLink(*(__int64*)((uintptr_t)g_exe + HOVER_ROAD_EDGE_RVA), *(int*)((uintptr_t)g_exe + HOVER_ROAD_POINT_RVA),
                     *(__int64*)((uintptr_t)g_exe + HOVER_ROAD_NODE_RVA), seg.endLink);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(&seg.startLink, 0, sizeof(seg.startLink));
        memset(&seg.endLink, 0, sizeof(seg.endLink));
        memset(&seg.startOwnLink, 0, sizeof(seg.startOwnLink));
        memset(&seg.endOwnLink, 0, sizeof(seg.endOwnLink));
        seg.snapModeStart = 0;
        seg.snapModeEnd = 0;
        seg.needsTerrainLevel = 0;
        seg.snapFlags = 0;
    }
}

// The local edge / node of a link in `network`, 0 if not here. Caller wraps in __try.
static __int64 FindNetworkLinkEdge(__int64 network, const RoadSyncRoadLink& l)
{
    if (!network || !l.hasEdge) return 0;
    __int64* it = *(__int64**)(network + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(network + MGR_SEGMENTS_END_OFFSET);
    for (; it && it < end; ++it) {
        __int64 e = *it;
        if (!e) continue;
        __int64 n0 = *(__int64*)(e + SEG_NODE0_OFFSET);
        __int64 n1 = *(__int64*)(e + SEG_NODE1_OFFSET);
        if (!n0 || !n1 || !NodeNear(n0, l.edgeA) || !NodeNear(n1, l.edgeB)) continue;
        int count = (int)((*(__int64*)(e + SEG_POINTS_END_OFFSET) - *(__int64*)(e + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
        if (count == l.edgePoints) return e;
    }
    return 0;
}

static __int64 FindNetworkLinkNode(__int64 network, const RoadSyncRoadLink& l)
{
    if (!network || !l.hasNode) return 0;
    __int64* it = *(__int64**)(network + NET_NODES_OFFSET);
    __int64* end = *(__int64**)(network + NET_NODES_OFFSET + 8);
    for (; it && it < end; ++it)
        if (*it && NodeNear(*it, l.node)) return *it;
    return 0;
}

static __int64 FindLinkEdge(const RoadSyncRoadLink& l)
{
    return FindNetworkLinkEdge(RoadNetwork(), l);
}

static __int64 FindLinkNode(const RoadSyncRoadLink& l)
{
    return FindNetworkLinkNode(RoadNetwork(), l);
}

// Sets own-network hover globals (0x9E4558/0x9E45D8 for edge, 0x9E4560/0x9E45E0 for node)
// so fc040 and fcaf0/518A80 split the target segment or attach to the target node identically.
static void SetOwnHover(__int64 manager, const RoadSyncRoadLink& l, const char* which)
{
    if (!l.hasEdge && !l.hasNode) return;
    __try {
        __int64 edge = FindNetworkLinkEdge(manager, l);
        __int64 node = FindNetworkLinkNode(manager, l);
        *(long long*)((uintptr_t)g_exe + 0x9E4558) = edge;
        *(long long*)((uintptr_t)g_exe + 0x9E45D8) = edge;
        *(long long*)((uintptr_t)g_exe + 0x9E4560) = node;
        *(long long*)((uintptr_t)g_exe + 0x9E45E0) = node;
        if (node) {
            *(unsigned char*)((uintptr_t)g_exe + 0x9E4568) = 1;
            memcpy((void*)((uintptr_t)g_exe + 0x9E456C), (const void*)(node + NODE_POS_OFFSET), 12);
        } else {
            *(unsigned char*)((uintptr_t)g_exe + 0x9E4568) = 0;
        }
        Logf("republicmp  ROAD_SYNC:   %s own-network link: edge %s, node %s", which,
             !l.hasEdge ? "-" : edge ? "found" : "NOT FOUND",
             !l.hasNode ? "-" : node ? "found" : "NOT FOUND");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Puts a link into the hover globals (what the footpath tool reads); false
// if a part of it is not here (that part stays 0 - the end only touches).
static bool SetRoadHover(const RoadSyncRoadLink& l, const char* which)
{
    if (!l.hasEdge && !l.hasNode) return true;
    __int64 edge = 0, node = 0;
    __try {
        edge = FindLinkEdge(l);
        node = FindLinkNode(l);
        if (edge && (l.pointIndex < 0 || l.pointIndex >= l.edgePoints)) edge = 0;
        *(long long*)((uintptr_t)g_exe + HOVER_ROAD_EDGE_RVA) = edge;
        *(int*)((uintptr_t)g_exe + HOVER_ROAD_POINT_RVA) = edge ? l.pointIndex : 0;
        *(long long*)((uintptr_t)g_exe + HOVER_ROAD_NODE_RVA) = node;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    bool ok = (!l.hasEdge || edge) && (!l.hasNode || node);
    Logf("republicmp  ROAD_SYNC:   %s joins a road: edge %s (point %d), node %s", which,
         !l.hasEdge ? "-" : edge ? "found" : "NOT FOUND", l.pointIndex,
         !l.hasNode ? "-" : node ? "found" : "NOT FOUND");
    return ok;
}

// Layout of an MSVC std::vector header; lets fcaf0 / FUN_14021d2b0 read a
// route we own without constructing a std::vector inside an SEH function.
struct RawVectorHeader {
    const void* first;
    const void* last;
    const void* end;
};

// ---------------------------------------------------------------------------
// Segment replay
// ---------------------------------------------------------------------------

// True if the same path is already built: a segment between the same two
// nodes with the same number of interior points and the same middle point
// (either direction). Two different routes between the same nodes are both
// kept.
static bool SegmentBetweenNodesExists(const RoadSyncSegment& want)
{
    const float* a = want.startNode;
    const float* b = want.endNode;
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(want.category);
    if (!info) return false;
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
        if (!manager) return false;
        __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
        for (; it && it < end; ++it) {
            __int64 seg = *it;
            if (!seg) continue;
            __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
            __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
            if (!n0 || !n1) continue;
            bool forward = NodeNear(n0, a) && NodeNear(n1, b);
            bool backward = NodeNear(n0, b) && NodeNear(n1, a);
            if (!forward && !backward) continue;
            const RoadSyncPoint* pts = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
            const RoadSyncPoint* pend = *(const RoadSyncPoint**)(seg + SEG_POINTS_END_OFFSET);
            int count = (pts && pend > pts) ? (int)(pend - pts) : 0;
            if (count != want.pointCount) continue;
            if (count == 0) return true;
            int mid = count / 2;
            const float* q = pts[forward ? mid : count - 1 - mid].pos;
            const float* w = want.points[mid].pos;
            if (fabsf(q[0] - w[0]) < 0.5f && fabsf(q[2] - w[2]) < 0.5f) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// Rebuilds `seg` on this machine through the tool's own primitives (see
// road_sync.h). Returns false only if the replay threw; *outEngineAccepted is
// the engine's verdict right before the commit (+0x315 valid, no error).
// All spoofed input state is restored before returning.
// Set by ApplySegmentImpl: the last replay threw inside the engine.
static bool g_lastApplyThrew = false;

// Margin around a replayed piece for the view-list scope (80 m around a
// road, 150 m around a building).
#define SCOPE_ROAD_MARGIN 80.0f

struct ScopeBox {
    float minX, minZ, maxX, maxZ;
    bool any;
};

static void BoxAdd(ScopeBox& b, const float p[3])
{
    if (!b.any) { b.minX = b.maxX = p[0]; b.minZ = b.maxZ = p[2]; b.any = true; return; }
    if (p[0] < b.minX) b.minX = p[0];
    if (p[0] > b.maxX) b.maxX = p[0];
    if (p[2] < b.minZ) b.minZ = p[2];
    if (p[2] > b.maxZ) b.maxZ = p[2];
}

static bool BeginScope(const ScopeBox& b, float margin)
{
    return b.any && ViewListScope_Begin(b.minX - margin, b.minZ - margin, b.maxX + margin, b.maxZ + margin);
}

static bool ApplySegmentImplInner(const RoadSyncSegment& seg, bool* outEngineAccepted);

static void ResetManagerDragState(__int64 manager)
{
    if (!manager) return;
    __try {
        typedef void (*QueueDeleteMeshFn)(__int64 mgr, void* mesh);
        QueueDeleteMeshFn queueDeleteMesh = (QueueDeleteMeshFn)((uintptr_t)g_exe + 0x542900);
        const int kExtraPreviewOffsets[2] = { 0x5F8, 0x600 };
        for (int off : kExtraPreviewOffsets) {
            void* p = *(void**)(manager + off);
            if (p) {
                queueDeleteMesh(manager, p);
                *(void**)(manager + off) = nullptr;
            }
        }
        *(long long*)(manager + MGR_ROAD_LINK_OFFSET) = 0;
        *(long long*)(manager + MGR_ROAD_LINK_EXTRA_OFFSET) = 0;
        *(long long*)(manager + 0x340) = 0;
        *(long long*)(manager + 0x348) = 0;
        *(long long*)(manager + 0x350) = 0;
        *(int*)(manager + 0x358) = 0;
        *(long long*)(manager + 0x360) = 0;
        *(int*)(manager + 0x368) = 0;
        *(long long*)(manager + 0x370) = 0;
        *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = 0;
        *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) = 0;
        *(long long*)(manager + MGR_HOVER_SEGMENT_COPY_OFFSET) = 0;
        *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) = 0;
        *(unsigned char*)(manager + MGR_VALID_OFFSET) = 0;
        *(int*)(manager + MGR_ERROR_CATEGORY_OFFSET) = -1;
        *(unsigned int*)(manager + MGR_ERROR_SUBCODE_OFFSET) = 0;
        *(unsigned char*)(manager + 0x5CB) = 0;
        *(unsigned char*)(manager + 0x5CC) = 0;
        *(long long*)(manager + 0x740) = 0;
        *(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) = 0;
        *(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) = 0;
        *(unsigned char*)(manager + 0x74A) = 0;
        *(unsigned char*)(manager + 0x74B) = 0;
        ((ManagerResetFn)((uintptr_t)g_exe + MANAGER_RESET_RVA))(manager, manager + MANAGER_RESET_STATE_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The replay sees the same "camera-visible" edges as the sender's does.
static bool ApplySegmentImpl(const RoadSyncSegment& seg, bool* outEngineAccepted)
{
    ScopeBox box = {};
    BoxAdd(box, seg.startNode);
    BoxAdd(box, seg.endNode);
    for (int i = 0; i < seg.pointCount && i < ROAD_SYNC_MAX_POINTS; i++) BoxAdd(box, seg.points[i].pos);
    bool scoped = BeginScope(box, SCOPE_ROAD_MARGIN);
    bool ok = ApplySegmentImplInner(seg, outEngineAccepted);
    if (scoped) ViewListScope_End();
    return ok;
}

static bool ApplySegmentImplInner(const RoadSyncSegment& seg, bool* outEngineAccepted)
{
    *outEngineAccepted = false;
    g_lastApplyThrew = false;

    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(seg.category);
    if (!info || !info->enabled || seg.pointCount < 0 || seg.pointCount > ROAD_SYNC_MAX_POINTS) {
        Logf("republicmp  ROAD_SYNC: refusing segment category=%d points=%d", seg.category, seg.pointCount);
        return false;
    }

    __int64 thisPtr = GetWorldThisPtr();
    unsigned char* lmbHeld = (unsigned char*)((uintptr_t)g_exe + LMB_HELD_RVA);
    unsigned char* lmbJustPressed = (unsigned char*)((uintptr_t)g_exe + LMB_JUST_PRESSED_RVA);
    int* presetCurveMode = (int*)((uintptr_t)g_exe + PRESET_CURVE_MODE_RVA);
    ToolWrapperFn wrapper = (ToolWrapperFn)((uintptr_t)g_exe + g_categoryWrapperRva[seg.category]);

    __int64 manager = 0;
    __try {
        manager = *(__int64*)(thisPtr + info->managerOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!manager) {
        Logf("republicmp  ROAD_SYNC: %s manager is null", info->name);
        return false;
    }
    if (ToolWrapperGuardBusy(thisPtr)) {
        Logf("republicmp  ROAD_SYNC: tool wrapper guard set, not applying");
        return false;
    }

    bool saved = false;
    unsigned char savedLmbHeld = 0, savedLmbJustPressed = 0, savedSnapModeA = 0, savedSnapModeB = 0;
    long long savedSnapTarget = 0;
    float savedContinuingY = 0.0f, savedCurve = 0.0f, savedRelax = 0.0f;
    float savedCursor[3] = { 0.0f, 0.0f, 0.0f };
    int savedPresetCurveMode = -1;
    bool threw = false;

    __try {
        __try {
            Logf("republicmp  ROAD_SYNC: applying %s type=%d variant=%d points=%d start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f)",
                 info->name, seg.typeId, seg.variant, seg.pointCount,
                 seg.startNode[0], seg.startNode[1], seg.startNode[2],
                 seg.endNode[0], seg.endNode[1], seg.endNode[2]);

            // Save and neutralise the local player's input state before clearing hover globals.
            savedLmbHeld = *lmbHeld;
            savedLmbJustPressed = *lmbJustPressed;
            savedSnapTarget = *(long long*)(thisPtr + SNAP_TARGET_PTR_OFFSET);
            savedSnapModeA = *(unsigned char*)(thisPtr + SNAP_MODE_A_OFFSET);
            savedSnapModeB = *(unsigned char*)(thisPtr + SNAP_MODE_B_OFFSET);
            HoverGlobals_Clear();
            savedContinuingY = *(float*)(thisPtr + CONTINUING_Y_OFFSET_OFFSET);
            savedCursor[0] = *(float*)(thisPtr + CURSOR_X_OFFSET);
            savedCursor[1] = *(float*)(thisPtr + CURSOR_Y_OFFSET);
            savedCursor[2] = *(float*)(thisPtr + CURSOR_Z_OFFSET);
            ResetManagerDragState(manager);
            *(unsigned char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) = 0;
            savedCurve = *(float*)(manager + MGR_CURVE_OFFSET);
            savedRelax = *(float*)(manager + MGR_RELAX_TIMER_OFFSET);
            savedPresetCurveMode = *presetCurveMode;
            saved = true;

            *(long long*)(thisPtr + SNAP_TARGET_PTR_OFFSET) = 0;
            *(unsigned char*)(thisPtr + SNAP_MODE_A_OFFSET) = 0;
            *(unsigned char*)(thisPtr + SNAP_MODE_B_OFFSET) = 0;
            *(float*)(thisPtr + CONTINUING_Y_OFFSET_OFFSET) = 0.0f;
            *presetCurveMode = -1;
            *(float*)(manager + MGR_CURVE_OFFSET) = seg.curveValue;

            // Drag start: the wrapper sets up its drag state and calls fc040;
            // fc040 is then re-run with fuzzy node matching so the start
            // attaches to the node already at this position. A footpath takes
            // its start's road link from the hover globals there; own-network
            // hover links (rail switches, mid-segment T-junctions) are also set.
            if (seg.category == ROAD_SYNC_CATEGORY_PEDESTRIAN) SetRoadHover(seg.startLink, "start");
            SetOwnHover(manager, seg.startOwnLink, "start");
            SetHoveredPort(seg.startPort, "start");
            bool wantStartAttached = (seg.snapFlags & 0x01) != 0;
            if (wantStartAttached && *(long long*)((uintptr_t)g_exe + 0x9E4560) == 0) {
                __int64 fallbackStart = FindNetworkNodeAt(manager, seg.startNode);
                if (fallbackStart) {
                    *(long long*)((uintptr_t)g_exe + 0x9E4560) = fallbackStart;
                    *(long long*)((uintptr_t)g_exe + 0x9E45E0) = fallbackStart;
                    *(unsigned char*)((uintptr_t)g_exe + 0x9E4568) = 1;
                    memcpy((void*)((uintptr_t)g_exe + 0x9E456C), (const void*)(fallbackStart + NODE_POS_OFFSET), 12);
                }
            }
            if (seg.snapModeStart == 2) {
                *(unsigned char*)((uintptr_t)g_exe + 0x9E4578) = 1;
                float* snapPtA = (float*)((uintptr_t)g_exe + 0x9E457C);
                snapPtA[0] = seg.startNode[0] - seg.snapOffsetStart[0];
                snapPtA[1] = seg.startNode[1] - seg.snapOffsetStart[1];
                snapPtA[2] = seg.startNode[2] - seg.snapOffsetStart[2];
            }
            *(float*)(thisPtr + CURSOR_X_OFFSET) = seg.startNode[0];
            *(float*)(thisPtr + CURSOR_Y_OFFSET) = seg.startNode[1];
            *(float*)(thisPtr + CURSOR_Z_OFFSET) = seg.startNode[2];
            *lmbHeld = 0;
            *lmbJustPressed = 1;
            wrapper(thisPtr, seg.typeId, (unsigned int)seg.variant);
            *lmbJustPressed = 0;

            float start[3] = { seg.startNode[0], seg.startNode[1], seg.startNode[2] };
            float startDir[3] = {
                (seg.snapModeStart == 2) ? (seg.startNode[0] - seg.snapOffsetStart[0]) : 0.0f,
                (seg.snapModeStart == 2) ? (seg.startNode[1] - seg.snapOffsetStart[1]) : 0.0f,
                (seg.snapModeStart == 2) ? (seg.startNode[2] - seg.snapOffsetStart[2]) : 0.0f
            };
            if (wantStartAttached) {
                unsigned __int64 existingStartHover = *(unsigned __int64*)((uintptr_t)g_exe + 0x9E4560);
                char fuzzyStart = (seg.snapModeStart == 2) ? 0 : 1;
                ((Fc040Fn)((uintptr_t)g_exe + FC040_RVA))(manager, start, 0, fuzzyStart,
                    Fc040Param5ForCategory(seg.category, seg.typeId), existingStartHover, startDir);
            } else {
                ((Fc040Fn)((uintptr_t)g_exe + FC040_RVA))(manager, start, 0, 0,
                    Fc040Param5ForCategory(seg.category, seg.typeId), 0, startDir);
            }
            if ((seg.snapFlags & 0x01) != 0 && *(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) == 0) {
                __int64 existingStart = FindNetworkNodeAt(manager, seg.startNode);
                if (existingStart) {
                    *(__int64*)(manager + MGR_START_NODE_OFFSET) = existingStart;
                    *(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) = 1;
                }
            }
            // Keep snapModeStart/End (+0x74A/+0x74B) zero while fcaf0 runs:
            // 0x4FCAF0 (0x140514FB7..0x140515186) inserts startNode/endNode into manager+0x410
            // whenever +0x74A/+0x74B == 1 or 3, and seg.points captured at 0x518A80 entry
            // already contains those points. Setting +0x74A/+0x74B before fcaf0 duplicated
            // the point at index 0/N-1 (distance == 0 -> 0/0 = -nan(ind) in 0x552090 -> crash).
            *(unsigned char*)(manager + 0x74A) = 0;
            *(unsigned char*)(manager + 0x74B) = 0;
            *(long long*)(manager + MGR_HOVER_SEGMENT_COPY_OFFSET) = 0;
            // A fresh start node gets manager+0x24 added to its height; the
            // sender's position already includes it.
            if (*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) == 0)
                SetNodePos(*(__int64*)(manager + MGR_START_NODE_OFFSET), seg.startNode);
            else if (IsPowerLine(seg.category))
                CheckAttachedPowerNode(*(__int64*)(manager + MGR_START_NODE_OFFSET), seg.startNode, "start");
            *(float*)(manager + MGR_RELAX_TIMER_OFFSET) = 2.0f;

            // One drag tick with the sender's route as param_8: the engine
            // copies it into its route buffer instead of routing itself, and
            // matches the end node against existing nodes within +-1m.
            HoverGlobals_Clear();
            SetHoveredPort(seg.endPort, "end");
            if (seg.category == ROAD_SYNC_CATEGORY_PEDESTRIAN) {
                SetRoadHover(seg.endLink, "end");
            }
            SetOwnHover(manager, seg.endOwnLink, "end");
            if (seg.snapModeEnd == 2) {
                *(unsigned char*)((uintptr_t)g_exe + 0x9E4578) = 1;
                float* snapPt = (float*)((uintptr_t)g_exe + 0x9E457C);
                snapPt[0] = seg.endNode[0] - seg.snapOffsetEnd[0];
                snapPt[1] = seg.endNode[1] - seg.snapOffsetEnd[1];
                snapPt[2] = seg.endNode[2] - seg.snapOffsetEnd[2];
            }
            // Power lines: the tick then lifts the end by the pole height
            // (manager+0x24, 0x4FF3D4) unless a node or an edge is hovered -
            // even when the end matched a node. The sender pointed at the
            // node it ends on, so the replay hovers that node too; without it
            // the end hung 20m above the node, a separate node, and the line
            // carried no power on that PC (#22) and was missed by the
            // sender's demolition (#23).
            __int64 endHoverNode = 0;
            if (IsPowerLine(seg.category) && !seg.endPort.valid) {
                endHoverNode = FindNetworkNodeAt(manager, seg.endNode);
                if (endHoverNode) *(long long*)((uintptr_t)g_exe + HOVER_NODE_RVA) = endHoverNode;
            }
            RawVectorHeader route = { seg.points, seg.points + seg.pointCount, seg.points + seg.pointCount };
            if (endHoverNode) {
                *(unsigned char*)((uintptr_t)g_exe + 0x9E4568) = 1;
                memcpy((void*)((uintptr_t)g_exe + 0x9E456C), (const void*)(endHoverNode + NODE_POS_OFFSET), 12);
            }
            float end[3] = { seg.endNode[0], seg.endNode[1], seg.endNode[2] };
            *(float*)(thisPtr + CURSOR_X_OFFSET) = seg.endNode[0];
            *(float*)(thisPtr + CURSOR_Y_OFFSET) = seg.endNode[1];
            *(float*)(thisPtr + CURSOR_Z_OFFSET) = seg.endNode[2];
            *lmbHeld = 1;
            ((Fcaf0Fn)((uintptr_t)g_exe + FCAF0_RVA))(manager, end, seg.typeId, (unsigned int)seg.variant,
                                                      0, 0, 0, (__int64)&route, 0, 0);
            // Explicitly assign seg.points (route) to manager+0x408 after fcaf0 in case fcaf0
            // took an early exit on its internal Bézier curve [rbp+0x170] or mutated +0x410.
            ((VecAssignFn)((uintptr_t)g_exe + VEC_ASSIGN_RVA))((__int64*)(manager + MGR_ROUTE_BEGIN_OFFSET), &route);

            // Restore startNode and endNode positions after fcaf0:
            // - fcaf0 can overwrite startNode->pos (0x140517BB1) and endNode->pos (0x1404FF420,
            //   0x140511C44, 0x140515A5C).
            // - manager+0x430 (endNode) is always a temporary node before 0x518A80 runs, and
            //   0x518A80 uses endNode->pos with exact float equality (ucomiss) both to match
            //   attached end nodes (0x14051AC30) and to split mid-segments (0x14051AD70).
            if (*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) == 0 && *(__int64*)(manager + MGR_START_NODE_OFFSET))
                SetNodePos(*(__int64*)(manager + MGR_START_NODE_OFFSET), seg.startNode);
            if (*(__int64*)(manager + MGR_END_NODE_OFFSET))
            {
                bool wantEndAttached = ((seg.snapFlags & 0x02) != 0) || (endHoverNode != 0);
                if (wantEndAttached) {
                    __int64 existingEnd = endHoverNode ? endHoverNode : FindNetworkNodeAt(manager, seg.endNode);
                    if (existingEnd) {
                        SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), (const float*)(existingEnd + NODE_POS_OFFSET));
                        *(__int64*)(manager + 0x740) = existingEnd;
                        *(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) = 1;
                    } else {
                        SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), seg.endNode);
                        *(__int64*)(manager + 0x740) = 0;
                        *(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) = 0;
                    }
                } else {
                    SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), seg.endNode);
                    *(__int64*)(manager + 0x740) = 0;
                    *(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) = 0;
                }
            }
            if (IsPowerLine(seg.category) && *(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET) != 0)
                CheckAttachedPowerNode(*(__int64*)(manager + MGR_END_NODE_OFFSET), seg.endNode, "end");
            if (IsPowerLine(seg.category))
                Logf("republicmp  ROAD_SYNC:   power line end: %s", endHoverNode ? "on an existing node (hovered)" : "free");

            // Restore snap modes, offsets, +0x5CB/+0x5CC, and terrain auto-level flag (+0x312)
            // for RoadCommit (0x518A80).
            *(unsigned char*)(manager + 0x74A) = seg.snapModeStart;
            memcpy((void*)(manager + 0x74C), seg.snapOffsetStart, 12);
            *(unsigned char*)(manager + 0x5CB) = (seg.snapFlags & 0x04) ? 1 : 0;
            *(unsigned char*)(manager + 0x74B) = seg.snapModeEnd;
            memcpy((void*)(manager + 0x758), seg.snapOffsetEnd, 12);
            *(unsigned char*)(manager + 0x5CC) = (seg.snapFlags & 0x08) ? 1 : 0;
            *(unsigned char*)(manager + 0x312) = seg.needsTerrainLevel;

            // Ensure hover globals are still set for RoadCommit (0x518A80)
            SetHoveredPort(seg.endPort, "end");
            if (seg.category == ROAD_SYNC_CATEGORY_PEDESTRIAN) SetRoadHover(seg.endLink, "end");
            SetOwnHover(manager, seg.endOwnLink, "end");
            if (endHoverNode) *(long long*)((uintptr_t)g_exe + HOVER_NODE_RVA) = endHoverNode;

            // Force valid state so a false-positive obstacle/slope check on fcaf0's internal
            // curve [rbp+0x170] never drops a segment that the sender already validated.
            if (endHoverNode) {
                *(unsigned char*)((uintptr_t)g_exe + 0x9E4568) = 1;
                memcpy((void*)((uintptr_t)g_exe + 0x9E456C), (const void*)(endHoverNode + NODE_POS_OFFSET), 12);
            }
            *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = 0;
            *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) = 0;
            *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) = 0;
            *(unsigned char*)(manager + MGR_VALID_OFFSET) = 1;
            *(int*)(manager + MGR_ERROR_CATEGORY_OFFSET) = -1;
            *(unsigned int*)(manager + MGR_ERROR_SUBCODE_OFFSET) = 0;

            unsigned char valid = *(unsigned char*)(manager + MGR_VALID_OFFSET);
            int errCategory = *(int*)(manager + MGR_ERROR_CATEGORY_OFFSET);
            unsigned int errSubcode = *(unsigned int*)(manager + MGR_ERROR_SUBCODE_OFFSET);
            Logf("republicmp  ROAD_SYNC:   attach start=%d end=%d snap=%d/%d terrLvl=%d valid=%d errCategory=%d errSubcode=0x%x",
                 (int)*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET),
                 (int)*(unsigned char*)(manager + MGR_END_ATTACHED_OFFSET),
                 (int)*(unsigned char*)(manager + 0x74A),
                 (int)*(unsigned char*)(manager + 0x74B),
                 (int)*(unsigned char*)(manager + 0x312),
                 (int)valid, errCategory, errSubcode);
            *outEngineAccepted = (valid == 1) && (errCategory == -1);

            // Release: the wrapper commits and ends its drag.
            *lmbHeld = 0;
            wrapper(thisPtr, seg.typeId, (unsigned int)seg.variant);
            if (*(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) != 0)
                Logf("republicmp  ROAD_SYNC:   WARNING drag still active after release");
            *(unsigned char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) = 0;
            ResetManagerDragState(manager);
            if (endHoverNode) *(long long*)((uintptr_t)g_exe + HOVER_NODE_RVA) = 0;
        } __finally {
            if (saved) {
                *lmbHeld = savedLmbHeld;
                *lmbJustPressed = savedLmbJustPressed;
                *(long long*)(thisPtr + SNAP_TARGET_PTR_OFFSET) = savedSnapTarget;
                *(unsigned char*)(thisPtr + SNAP_MODE_A_OFFSET) = savedSnapModeA;
                *(unsigned char*)(thisPtr + SNAP_MODE_B_OFFSET) = savedSnapModeB;
                *(float*)(thisPtr + CONTINUING_Y_OFFSET_OFFSET) = savedContinuingY;
                *(float*)(thisPtr + CURSOR_X_OFFSET) = savedCursor[0];
                *(float*)(thisPtr + CURSOR_Y_OFFSET) = savedCursor[1];
                *(float*)(thisPtr + CURSOR_Z_OFFSET) = savedCursor[2];
                *presetCurveMode = savedPresetCurveMode;
                *(float*)(manager + MGR_CURVE_OFFSET) = savedCurve;
                *(float*)(manager + MGR_RELAX_TIMER_OFFSET) = savedRelax;
            }
            *(unsigned char*)((uintptr_t)g_exe + 0x9E4578) = 0;
            ClearHoveredPort();
            HoverGlobals_Clear();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        threw = true;
    }

    g_lastApplyThrew = threw;
    if (threw) {
        Logf("republicmp  ROAD_SYNC: <exception applying segment>");
        *outEngineAccepted = false;
        // The release never reached the wrapper's own end of the drag
        // (0x3D6A3F: thisPtr+0xd2ac = 0). Left set, the per-frame tool code
        // (FUN_14030d1a0) keeps working on the half-built drag and crashes
        // the game a frame later (0x310E58). End it the way the wrapper and
        // the demolish replay do.
        __try {
            *(unsigned char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) = 0;
            ResetManagerDragState(manager);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Logf("republicmp  ROAD_SYNC: resetting the tool after the exception threw too");
        }
        HoverGlobals_Clear();
        return false;
    }
    Logf("republicmp  ROAD_SYNC:   apply done - %s", *outEngineAccepted ? "engine ACCEPTED" : "engine REJECTED");
    return true;
}

static bool ApplySegment(const RoadSyncSegment& seg, bool* outEngineAccepted)
{
    InterlockedIncrement(&g_applyingSegment);
    // In legacy non-lockstep mode, the sender's terrain deformation arrives over
    // terrain sync; in Lockstep, the commit was intercepted before running locally,
    // so both PCs must execute the commit's own terrain deformation identically.
    bool foreignReplay = !Determinism_IsActive();
    if (foreignReplay) TerrainSync_BeginForeignReplay();
    bool result = ApplySegmentImpl(seg, outEngineAccepted);
    if (foreignReplay) TerrainSync_EndForeignReplay();
    InterlockedDecrement(&g_applyingSegment);
    return result;
}

// ---------------------------------------------------------------------------
// Upgrade replay
// ---------------------------------------------------------------------------

// Local segment matching the piece by node positions and point count, in
// either orientation. Caller wraps in __try.
static __int64 FindLocalSegment(__int64 manager, const RoadSyncUpgradePiece& piece, bool* outReversed)
{
    __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
    for (; it && it < end; ++it) {
        __int64 seg = *it;
        if (!seg) continue;
        long long count = (*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18;
        if (count != piece.segPointCount) continue;
        __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
        __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
        if (!n0 || !n1) continue;
        if (NodeNear(n0, piece.segStart) && NodeNear(n1, piece.segEnd)) { *outReversed = false; return seg; }
        if (NodeNear(n0, piece.segEnd) && NodeNear(n1, piece.segStart)) { *outReversed = true; return seg; }
    }
    return 0;
}

// Re-types one segment range: resets the manager with fc040, fills in the
// fields the engine's own re-type drag leaves behind (fcaf0 lines 1864-1907)
// and runs the commit, which takes its upgrade branch.
static bool ApplyUpgradePieceInner(int category, const RoadSyncUpgradeMsg& msg, const RoadSyncUpgradePiece& piece);

static bool ApplyUpgradePiece(int category, const RoadSyncUpgradeMsg& msg, const RoadSyncUpgradePiece& piece)
{
    ScopeBox box = {};
    BoxAdd(box, piece.segStart);
    BoxAdd(box, piece.segEnd);
    bool scoped = BeginScope(box, SCOPE_ROAD_MARGIN * 2);
    bool ok = ApplyUpgradePieceInner(category, msg, piece);
    if (scoped) ViewListScope_End();
    return ok;
}

static bool ApplyUpgradePieceInner(int category, const RoadSyncUpgradeMsg& msg, const RoadSyncUpgradePiece& piece)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(category);
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
        if (!manager) return false;

        bool reversed = false;
        __int64 seg = FindLocalSegment(manager, piece, &reversed);
        if (!seg) {
            Logf("republicmp  ROAD_SYNC: upgrade: no local %s segment at start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f) points=%d",
                 info->name, piece.segStart[0], piece.segStart[1], piece.segStart[2],
                 piece.segEnd[0], piece.segEnd[1], piece.segEnd[2], piece.segPointCount);
            return false;
        }

        int count = piece.segPointCount;
        int rs = piece.rangeStart, re = piece.rangeEnd, dir = piece.flag39c;
        if (reversed) {
            int nrs = count - 1 - re, nre = count - 1 - rs;
            rs = nrs; re = nre;
            if (dir == 1) dir = 2; else if (dir == 2) dir = 1;
        }
        if (rs < -1 || re > count || rs >= re) {
            Logf("republicmp  ROAD_SYNC: upgrade: bad range (%d,%d) for %d points", rs, re, count);
            return false;
        }

        // Range ends: a node or an interior point of the segment.
        const RoadSyncPoint* pts = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
        const float* a = (rs < 0) ? (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET) : pts[rs].pos;
        const float* b = (re >= count) ? (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET) : pts[re].pos;
        float splitStart[3] = { a[0], a[1], a[2] };
        float splitEnd[3] = { b[0], b[1], b[2] };

        float noDirection[3] = { 0.0f, 0.0f, 0.0f };
        ((Fc040Fn)((uintptr_t)g_exe + FC040_RVA))(manager, splitStart, 0, 1,
            Fc040Param5ForCategory(category, msg.newType), 0, noDirection);
        *(long long*)(manager + MGR_HOVER_SEGMENT_COPY_OFFSET) = 0;
        if (*(unsigned char*)(manager + MGR_START_ATTACHED_OFFSET) == 0)
            SetNodePos(*(__int64*)(manager + MGR_START_NODE_OFFSET), splitStart);
        SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), splitEnd);

        // Route buffer = the segment's own records strictly inside the range.
        int first = rs + 1, n = re - first;
        RawVectorHeader route = { pts + first, pts + first + (n > 0 ? n : 0), pts + first + (n > 0 ? n : 0) };
        ((VecAssignFn)((uintptr_t)g_exe + VEC_ASSIGN_RVA))((__int64*)(manager + MGR_ROUTE_BEGIN_OFFSET), &route);

        *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = seg;
        *(int*)(manager + MGR_UPG_RANGE_START_OFFSET) = rs;
        *(int*)(manager + MGR_UPG_RANGE_END_OFFSET) = re;
        *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) = 1;
        *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) = 0;
        *(int*)(manager + MGR_UPG_OLD_TYPE_OFFSET) = piece.oldType;
        *(int*)(manager + MGR_UPG_OLD_VARIANT_OFFSET) = piece.oldVariant;
        *(int*)(manager + MGR_UPG_DIR_OFFSET) = dir;
        *(int*)(manager + MGR_TYPE_OFFSET) = msg.newType;
        *(int*)(manager + MGR_VARIANT_OFFSET) = msg.newVariant;
        *(int*)(manager + MGR_FLAG_5C4_OFFSET) = msg.flag5c4;

        Logf("republicmp  ROAD_SYNC: %s %s%s range=(%d,%d)/%d type %d/%d -> %d/%d type394=%d dir=%d",
             piece.oldType == 0x65 ? "DEMOLISH" : "upgrade",
             info->name, reversed ? " (reversed)" : "", rs, re, count,
             *(int*)(seg + SEG_TYPE_OFFSET), *(int*)(seg + SEG_VARIANT_OFFSET), msg.newType, msg.newVariant,
             piece.oldType, dir);

        RoadCommitFn commit = g_origRoadCommit ? g_origRoadCommit : (RoadCommitFn)((uintptr_t)g_exe + COMMIT_RVA);
        commit(manager, 0, 0, 0, 0, 0, 0);
        ResetManagerDragState(manager);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  ROAD_SYNC: <exception applying upgrade piece>");
        return false;
    }
}

// Returns the number of pieces applied.
static int ApplyUpgrade(const RoadSyncUpgradeMsg& msg)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(msg.category);
    if (!info || !info->enabled || msg.pieceCount < 1 || msg.pieceCount > ROAD_SYNC_MAX_UPGRADE_PIECES) {
        Logf("republicmp  ROAD_SYNC: refusing upgrade seq=%d category=%d pieces=%d", msg.seq, msg.category, msg.pieceCount);
        return 0;
    }
    if (ToolWrapperGuardBusy(GetWorldThisPtr())) return 0;

    HoverGlobals_Clear();
    InterlockedIncrement(&g_applyingSegment);
    bool foreignReplay = !Determinism_IsActive();
    if (foreignReplay) TerrainSync_BeginForeignReplay();
    int applied = 0;
    for (int i = 0; i < msg.pieceCount; i++) {
        if (ApplyUpgradePiece(msg.category, msg, msg.pieces[i])) applied++;
    }
    if (foreignReplay) TerrainSync_EndForeignReplay();
    InterlockedDecrement(&g_applyingSegment);
    return applied;
}

// ---------------------------------------------------------------------------
// Demolition (terrain_demolish tool)
// ---------------------------------------------------------------------------

// Where `pos` lies on segment `seg`: -1 = its start node, count = its end
// node, an interior point index, or -2 if it is none of them.
static int IndexOnSegment(__int64 seg, const float pos[3])
{
    const RoadSyncPoint* pts = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
    const RoadSyncPoint* end = *(const RoadSyncPoint**)(seg + SEG_POINTS_END_OFFSET);
    int count = (pts && end > pts) ? (int)(end - pts) : 0;
    __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
    __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
    if (n0 && NodeNear(n0, pos)) return -1;
    if (n1 && NodeNear(n1, pos)) return count;
    for (int i = 0; i < count; i++) {
        const float* q = pts[i].pos;
        if (fabsf(q[0] - pos[0]) < 0.05f && fabsf(q[2] - pos[2]) < 0.05f && fabsf(q[1] - pos[1]) < 0.5f)
            return i;
    }
    return -2;
}

// A segment of the manager that has both `a` and `b` on it (nodes or
// interior points), or 0.
static __int64 FindSegmentWithRange(__int64 manager, const float a[3], const float b[3])
{
    __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
    for (; it && it < end; ++it) {
        __int64 seg = *it;
        if (!seg || !*(__int64*)(seg + SEG_NODE0_OFFSET) || !*(__int64*)(seg + SEG_NODE1_OFFSET)) continue;
        int ia = IndexOnSegment(seg, a);
        if (ia == -2) continue;
        int ib = IndexOnSegment(seg, b);
        if (ib != -2 && ib != ia) return seg;
    }
    return 0;
}

#define NODE_ALLOC_RVA 0x84DECC // operator new
#define NODE_CTOR_RVA 0x0FF950  // node constructor (returns the node)
#define NODE_OBJECT_SIZE 0xE8
typedef __int64 (*AllocFn)(unsigned long long size);
typedef __int64 (*NodeCtorFn)(__int64 node);

// A pick that reached a segment end may hold that segment's own node as the
// drag's start/end node (+0x748/+0x749 set, the node in the network's list).
// Moving it to a cut point dragged the rest of the line along: a whole-segment
// pick cut down to the sender's middle piece moved the segment's first node
// (at a switch's port) onto the cut, and the next demolitions removed the
// wrong pieces - a couple of spans stayed on the receiver (#23). Before the
// node moves, put a fresh copy in its place, as the drag tick itself does
// (0x4FE448..0x4FE48A).
// A drag node of the manager (+0x428 / +0x430), made when missing.
#define OPERATOR_NEW_RVA 0x84DECC
#define NODE_CTOR_RVA 0x0FF950
static const unsigned char kOperatorNewEntry[16] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                                     0xd9, 0xeb, 0x21, 0x48, 0x8b, 0xcb, 0xe8, 0xd5 };
static void EnsureDragNode(__int64 manager, int nodeOffset)
{
    if (*(__int64*)(manager + nodeOffset)) return;
    if (memcmp((const void*)((uintptr_t)g_exe + OPERATOR_NEW_RVA), kOperatorNewEntry, sizeof(kOperatorNewEntry)) != 0) return;
    typedef void* (*NewFn)(size_t);
    typedef __int64 (*NodeCtorFn)(void*);
    void* raw = ((NewFn)((uintptr_t)g_exe + OPERATOR_NEW_RVA))(0xE8);
    if (!raw) return;
    __int64 node = ((NodeCtorFn)((uintptr_t)g_exe + NODE_CTOR_RVA))(raw);
    *(__int64*)(manager + nodeOffset) = node;
    Logf("republicmp  ROAD_SYNC:   made the manager's %s drag node for the demolition", nodeOffset == MGR_START_NODE_OFFSET ? "start" : "end");
}

static void DetachDragNode(__int64 manager, int nodeOffset, int attachedOffset, const float target[3])
{
    __int64 node = *(__int64*)(manager + nodeOffset);
    if (!node) return;
    const float* p = (const float*)(node + NODE_POS_OFFSET);
    if (p[0] == target[0] && p[1] == target[1] && p[2] == target[2]) return; // stays where it is
    if (!*(unsigned char*)(manager + attachedOffset) && !InPointerList(manager + NET_NODES_OFFSET, node)) return;
    __int64 fresh = ((AllocFn)((uintptr_t)g_exe + NODE_ALLOC_RVA))(NODE_OBJECT_SIZE);
    if (!fresh) return;
    ((NodeCtorFn)((uintptr_t)g_exe + NODE_CTOR_RVA))(fresh);
    memcpy((void*)(fresh + NODE_POS_OFFSET), p, 12);
    *(__int64*)(manager + nodeOffset) = fresh;
    *(unsigned char*)(manager + attachedOffset) = 0;
    Logf("republicmp  ROAD_SYNC:   the cut's %s held the segment's own node at (%.2f,%.2f,%.2f) - a fresh node takes its place",
         nodeOffset == MGR_START_NODE_OFFSET ? "start" : "end", p[0], p[1], p[2]);
}

// After fcaf0 armed a demolish on segment +0x380: if its range is not the
// sender's, cut the sender's range instead - indices, the route buffer of
// the points strictly inside, and the two end nodes on the range ends (the
// same state the upgrade replay builds). If the range is on another segment
// of the same type, that segment is cut instead. Returns false if the
// sender's range was not found (nothing is changed then).
static bool AdoptSenderRange(__int64 manager, const RoadSyncDemolishMsg& m, int* engineRs, int* engineRe,
                             int* rsOut, int* reOut)
{
    __int64 seg = *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET);
    *engineRs = *(int*)(manager + MGR_UPG_RANGE_START_OFFSET);
    *engineRe = *(int*)(manager + MGR_UPG_RANGE_END_OFFSET);
    *rsOut = *engineRs;
    *reOut = *engineRe;
    if (!seg || !m.hasRange || *(unsigned char*)(manager + MGR_UPG_MULTI_OFFSET) != 0) return false;

    int a = IndexOnSegment(seg, m.rangeA);
    int b = IndexOnSegment(seg, m.rangeB);
    bool switched = false;
    if (a == -2 || b == -2 || a == b) {
        // The engine picked another segment (e.g. the first one of a hovered
        // junction node): move to the sender's if it is of the same kind.
        __int64 other = FindSegmentWithRange(manager, m.rangeA, m.rangeB);
        if (!other || *(int*)(other + SEG_TYPE_OFFSET) != *(int*)(seg + SEG_TYPE_OFFSET) ||
            *(int*)(other + SEG_VARIANT_OFFSET) != *(int*)(seg + SEG_VARIANT_OFFSET))
            return false;
        seg = other;
        a = IndexOnSegment(seg, m.rangeA);
        b = IndexOnSegment(seg, m.rangeB);
        switched = true; // written to +0x380 only once the range is accepted
    }
    int rs = a < b ? a : b, re = a < b ? b : a;
    const RoadSyncPoint* pts = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
    int count = (int)(*(const RoadSyncPoint**)(seg + SEG_POINTS_END_OFFSET) - pts);
    if (!switched && rs == *engineRs && re == *engineRe) return true; // same piece - nothing to change
    // A cut at the first/last interior point would leave a stub between it
    // and the node with no points at all; the game's frame code reads the
    // first point of every segment on a node (SOVIET64+0x526605) and crashes
    // on it. The engine snaps such ends to the node - do the same (the sender
    // may have had more points on its copy of the segment). Power-line spans
    // need no points (the game lays them with none), so a power line is cut
    // exactly where the sender cut it - snapped, it lost the next span too and
    // the sender's demolition of that span found it already gone.
    if (!IsPowerLine(m.category)) {
        if (rs == 0) rs = -1;
        if (re == count - 1) re = count;
    }
    if (rs >= re) return false;
    *rsOut = rs;
    *reOut = re;
    if (!switched && rs == *engineRs && re == *engineRe) return true;

    const float* pa = (rs < 0) ? (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET) : pts[rs].pos;
    const float* pb = (re >= count) ? (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET) : pts[re].pos;
    float splitStart[3] = { pa[0], pa[1], pa[2] };
    float splitEnd[3] = { pb[0], pb[1], pb[2] };

    int first = rs + 1, n = re - first;
    RawVectorHeader route = { pts + first, pts + first + (n > 0 ? n : 0), pts + first + (n > 0 ? n : 0) };
    ((VecAssignFn)((uintptr_t)g_exe + VEC_ASSIGN_RVA))((__int64*)(manager + MGR_ROUTE_BEGIN_OFFSET), &route);
    *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = seg;
    *(int*)(manager + MGR_UPG_RANGE_START_OFFSET) = rs;
    *(int*)(manager + MGR_UPG_RANGE_END_OFFSET) = re;
    DetachDragNode(manager, MGR_START_NODE_OFFSET, MGR_START_ATTACHED_OFFSET, splitStart);
    DetachDragNode(manager, MGR_END_NODE_OFFSET, MGR_END_ATTACHED_OFFSET, splitEnd);
    SetNodePos(*(__int64*)(manager + MGR_START_NODE_OFFSET), splitStart);
    SetNodePos(*(__int64*)(manager + MGR_END_NODE_OFFSET), splitEnd);
    return true;
}

// Repeats one frame of the demolish tool at the sender's cursor: reset the
// manager's preview state (as the tool does every frame), let fcaf0 pick the
// piece under the cursor, and commit if it found one. outRemoved = the engine
// armed and ran the commit.
static bool ApplyDemolishInner(const RoadSyncDemolishMsg& m, bool* outRemoved);

// The pick finds the piece wherever this player's camera is.
// Lockstep: the hovered node / edge a hover demolition carries (main thread, during its apply).
static const RoadSyncDemolishHover* g_applyHover = nullptr;
static const unsigned kHoverNodeRvas[2] = { 0x9E4560, 0x9E45E0 };
static const unsigned kHoverEdgeRvas[2] = { 0x9E4558, 0x9E45D8 };

static bool SamePos(const float* a, const float* b)
{
    return fabsf(a[0] - b[0]) < 0.01f && fabsf(a[1] - b[1]) < 0.01f && fabsf(a[2] - b[2]) < 0.01f;
}

static int EdgePointCount(__int64 edge)
{
    return (int)((*(__int64*)(edge + SEG_POINTS_END_OFFSET) - *(__int64*)(edge + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
}

bool RoadSync_ReadDemolishHover(__int64 manager, RoadSyncDemolishHover* h)
{
    memset(h, 0, sizeof(*h));
    __try {
        __int64* begin = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
        for (int k = 0; k < 2 && !h->nodeKind; k++) {
            __int64 node = *(__int64*)((uintptr_t)g_exe + kHoverNodeRvas[k]);
            for (__int64* it = begin; node && it && it < end; ++it)
                if (*it && (*(__int64*)(*it + SEG_NODE0_OFFSET) == node || *(__int64*)(*it + SEG_NODE1_OFFSET) == node)) {
                    h->nodeKind = k + 1;
                    memcpy(h->node, (const float*)(node + NODE_POS_OFFSET), 12);
                    break;
                }
        }
        for (int k = 0; k < 2 && !h->edgeKind; k++) {
            __int64 edge = *(__int64*)((uintptr_t)g_exe + kHoverEdgeRvas[k]);
            for (__int64* it = begin; edge && it && it < end; ++it)
                if (*it == edge) {
                    __int64 n0 = *(__int64*)(edge + SEG_NODE0_OFFSET), n1 = *(__int64*)(edge + SEG_NODE1_OFFSET);
                    if (!n0 || !n1) break;
                    h->edgeKind = k + 1;
                    memcpy(h->edgeA, (const float*)(n0 + NODE_POS_OFFSET), 12);
                    memcpy(h->edgeB, (const float*)(n1 + NODE_POS_OFFSET), 12);
                    h->edgePoints = EdgePointCount(edge);
                    break;
                }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(h, 0, sizeof(*h));
        return false;
    }
    return h->nodeKind || h->edgeKind;
}

// Sets this PC's hovered node / edge to the carried ones (after the replay cleared them).
static void ApplyHoverRefs(__int64 manager, const RoadSyncDemolishHover& h)
{
    __int64* begin = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
    __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
    __int64 node = 0, edge = 0;
    for (__int64* it = begin; it && it < end; ++it) {
        __int64 e = *it;
        if (!e) continue;
        __int64 n0 = *(__int64*)(e + SEG_NODE0_OFFSET), n1 = *(__int64*)(e + SEG_NODE1_OFFSET);
        if (!n0 || !n1) continue;
        const float* p0 = (const float*)(n0 + NODE_POS_OFFSET);
        const float* p1 = (const float*)(n1 + NODE_POS_OFFSET);
        if (h.nodeKind && !node) {
            if (SamePos(p0, h.node)) node = n0;
            else if (SamePos(p1, h.node)) node = n1;
        }
        if (h.edgeKind && !edge && SamePos(p0, h.edgeA) && SamePos(p1, h.edgeB) && EdgePointCount(e) == h.edgePoints) edge = e;
    }
    if (node) *(__int64*)((uintptr_t)g_exe + kHoverNodeRvas[h.nodeKind - 1]) = node;
    if (edge) *(__int64*)((uintptr_t)g_exe + kHoverEdgeRvas[h.edgeKind - 1]) = edge;
    Logf("republicmp  ROAD_SYNC:   hover demolition: node %s, edge %s", !h.nodeKind ? "none" : node ? "found" : "MISSING",
         !h.edgeKind ? "none" : edge ? "found" : "MISSING");
}

bool RoadSync_ApplyCommandDemolishHover(const LsDemolishHoverMsg& msg)
{
    g_applyHover = &msg.hover;
    bool ok = RoadSync_ApplyCommandDemolish(msg.m);
    g_applyHover = nullptr;
    return ok;
}

static bool ApplyDemolish(const RoadSyncDemolishMsg& m, bool* outRemoved)
{
    ScopeBox box = {};
    BoxAdd(box, m.cursor);
    if (m.hasRange) { BoxAdd(box, m.rangeA); BoxAdd(box, m.rangeB); }
    bool scoped = BeginScope(box, SCOPE_ROAD_MARGIN);
    bool ok = ApplyDemolishInner(m, outRemoved);
    if (scoped) ViewListScope_End();
    return ok;
}

static bool ApplyDemolishInner(const RoadSyncDemolishMsg& m, bool* outRemoved)
{
    *outRemoved = false;
    int engineRs = 0, engineRe = 0, rs = 0, re = 0;
    bool adopted = false, viaNode = false, direct = false;
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(m.category);
    if (!info || !info->enabled) {
        Logf("republicmp  ROAD_SYNC: refusing demolish category=%d", m.category);
        return false;
    }
    __int64 thisPtr = GetWorldThisPtr();
    if (ToolWrapperGuardBusy(thisPtr)) return false;

    unsigned char* lmbHeld = (unsigned char*)((uintptr_t)g_exe + LMB_HELD_RVA);
    unsigned char* lmbJustPressed = (unsigned char*)((uintptr_t)g_exe + LMB_JUST_PRESSED_RVA);
    ManagerResetFn reset = (ManagerResetFn)((uintptr_t)g_exe + MANAGER_RESET_RVA);
    Fcaf0Fn fcaf0 = (Fcaf0Fn)((uintptr_t)g_exe + FCAF0_RVA);
    RoadCommitFn commit = g_origRoadCommit ? g_origRoadCommit : (RoadCommitFn)((uintptr_t)g_exe + COMMIT_RVA);

    bool threw = false;
    InterlockedIncrement(&g_applyingSegment);
    bool foreignReplay = !Determinism_IsActive();
    if (foreignReplay) TerrainSync_BeginForeignReplay();
    __try {
        __int64 manager = *(__int64*)(thisPtr + info->managerOffset);
        if (manager) {
            int demType = (m.demolishType >= 100 || m.demolishType == -20) ? m.demolishType : ROAD_DEMOLISH_TYPE;
            BuildMode savedMode = BuildMode_Read();
            unsigned char savedHeld = *lmbHeld, savedPressed = *lmbJustPressed;
            float* cursor = (float*)(thisPtr + CURSOR_X_OFFSET);
            float savedCursor[3] = { cursor[0], cursor[1], cursor[2] };
            __try {
                BuildMode_Write(m.mode);
                HoverGlobals_Clear();
                if (g_applyHover) ApplyHoverRefs(manager, *g_applyHover);
                *lmbHeld = 0;
                *lmbJustPressed = 0;
                cursor[0] = m.cursor[0]; cursor[1] = m.cursor[1]; cursor[2] = m.cursor[2];
                reset(manager, manager + MANAGER_RESET_STATE_OFFSET);
                float c[3] = { m.cursor[0], m.cursor[1], m.cursor[2] };
                fcaf0(manager, c, demType, 0, 0, 0, 0, 0, 0, 0);
                if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) == 0 && m.hasRange) {
                    // Picking by cursor skips short segments; the tool removes
                    // those through the hovered node (FUN_1403abc90 ~290-320,
                    // fcaf0 ~1833 / ~2032). Do the same: hover the local node
                    // at an end of the sender's piece and run the frame again.
                    __int64 seg = FindSegmentWithRange(manager, m.rangeA, m.rangeB);
                    if (seg) {
                        int ia = IndexOnSegment(seg, m.rangeA), ib = IndexOnSegment(seg, m.rangeB);
                        int lo = ia < ib ? ia : ib, hi = ia < ib ? ib : ia;
                        const RoadSyncPoint* pb = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
                        const RoadSyncPoint* pe = *(const RoadSyncPoint**)(seg + SEG_POINTS_END_OFFSET);
                        int count = (pb && pe > pb) ? (int)(pe - pb) : 0;
                        // The node the piece touches (node 0 for an interior piece)
                        // first, then the other end: a building's whole access
                        // road starts at the building's port node, whose first
                        // edge is not the road.
                        __int64 first = *(__int64*)(seg + (lo < 0 || hi < count ? SEG_NODE0_OFFSET : SEG_NODE1_OFFSET));
                        __int64 other = *(__int64*)(seg + (lo < 0 || hi < count ? SEG_NODE1_OFFSET : SEG_NODE0_OFFSET));
                        __int64 tryNodes[2] = { first, other };
                        for (int k = 0; k < 2 && !viaNode; k++) {
                            if (!tryNodes[k]) continue;
                            *(__int64*)((uintptr_t)g_exe + HOVER_NODE_RVA) = tryNodes[k];
                            reset(manager, manager + MANAGER_RESET_STATE_OFFSET);
                            fcaf0(manager, c, demType, 0, 0, 0, 0, 0, 0, 0);
                            viaNode = *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0;
                        }
                        // The sender's tool hit this piece with its camera's ray
                        // (the hover edge, 0x9E4558); the replay has only the
                        // cursor, and next to a building neither the cursor nor
                        // a node picks it. Arm it the way fcaf0's node path does
                        // (0x4FEA52: +0x380 piece, +0x388/+0x38c range, +0x390
                        // hit) - AdoptSenderRange then writes the sender's range,
                        // route and cut points - if the drag nodes are there.
                        // The drag nodes exist only where this network's tool was
                        // used: a PC without them made the fallback, the other did not
                        // (a forklift junction stayed on one PC). They are made here
                        // as fcaf0 makes them (operator new 0xe8, node constructor ff950).
                        if (!viaNode) {
                            EnsureDragNode(manager, MGR_START_NODE_OFFSET);
                            EnsureDragNode(manager, MGR_END_NODE_OFFSET);
                        }
                        if (!viaNode && *(__int64*)(manager + MGR_START_NODE_OFFSET) &&
                            *(__int64*)(manager + MGR_END_NODE_OFFSET)) {
                            *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET) = seg;
                            *(int*)(manager + MGR_UPG_RANGE_START_OFFSET) = -2; // never the sender's, so it is written
                            *(int*)(manager + MGR_UPG_RANGE_END_OFFSET) = -2;
                            *(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) = 1;
                            direct = true;
                        }
                    }
                }
                if (*(unsigned char*)(manager + MGR_UPG_GATE_OFFSET) != 0) {
                    adopted = AdoptSenderRange(manager, m, &engineRs, &engineRe, &rs, &re);
                    if ((viaNode || direct) && m.hasRange) {
                        // For the log: what the fallback is about to cut.
                        __int64 seg = *(__int64*)(manager + MGR_UPG_SEGMENT_OFFSET);
                        if (seg) {
                            const float* n0 = (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET);
                            const float* n1 = (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET);
                            const RoadSyncPoint* pb = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
                            const RoadSyncPoint* pe = *(const RoadSyncPoint**)(seg + SEG_POINTS_END_OFFSET);
                            Logf("republicmp  ROAD_SYNC:   fallback segment (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) %d point(s), cut (%d,%d); sender's piece (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f)",
                                 n0[0], n0[1], n0[2], n1[0], n1[1], n1[2], (pb && pe > pb) ? (int)(pe - pb) : 0,
                                 *(int*)(manager + MGR_UPG_RANGE_START_OFFSET), *(int*)(manager + MGR_UPG_RANGE_END_OFFSET),
                                 m.rangeA[0], m.rangeA[1], m.rangeA[2], m.rangeB[0], m.rangeB[1], m.rangeB[2]);
                        }
                    }
                    // Through the node, or armed directly, the engine's own pick
                    // is only a stand-in for the sender's piece - never cut it on
                    // its own.
                    if (adopted || (!viaNode && !direct)) {
                        commit(manager, 0, 0, 0, 0, 0, 0);
                        *outRemoved = true;
                    }
                }
                reset(manager, manager + MANAGER_RESET_STATE_OFFSET);
            } __finally {
                BuildMode_Write(savedMode);
                HoverGlobals_Clear(); // the hovered node may be gone now
                *lmbHeld = savedHeld;
                *lmbJustPressed = savedPressed;
                cursor[0] = savedCursor[0]; cursor[1] = savedCursor[1]; cursor[2] = savedCursor[2];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        threw = true;
    }
    if (foreignReplay) TerrainSync_EndForeignReplay();
    InterlockedDecrement(&g_applyingSegment);
    if (threw) Logf("republicmp  ROAD_SYNC: <exception applying demolish>");
    else if (*outRemoved) {
        if (direct)
            Logf("republicmp  ROAD_SYNC:   not picked at the cursor or its nodes - armed directly on the piece");
        else if (viaNode)
            Logf("republicmp  ROAD_SYNC:   nothing picked at the cursor - armed through the piece's node");
        if (!m.hasRange)
            Logf("republicmp  ROAD_SYNC:   range (%d,%d) chosen here (sender sent no range)", engineRs, engineRe);
        else if (!adopted)
            Logf("republicmp  ROAD_SYNC:   sender's range is not on the segment here - kept this engine's (%d,%d)",
                 engineRs, engineRe);
        else if (rs != engineRs || re != engineRe)
            Logf("republicmp  ROAD_SYNC:   range (%d,%d) -> sender's (%d,%d)", engineRs, engineRe, rs, re);
        else
            Logf("republicmp  ROAD_SYNC:   range (%d,%d) = sender's", rs, re);
    }
    return !threw;
}

static void SendAck(int seq, int category, bool success, int attempts);

// Multi-segment demolish: each whole segment found here by its nodes is
// demolished on its own - cursor on its middle, range node to node.
static bool ApplyDemolishPieces(const RoadSyncDemolishMsg& m, bool* outRemoved)
{
    *outRemoved = false;
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(m.category);
    if (!info || !info->enabled) return false;
    bool ok = true;
    for (int i = 0; i < m.pieceCount && i < 8; i++) {
        RoadSyncDemolishMsg one = {};
        one.seq = m.seq;
        one.category = m.category;
        one.demolishType = m.demolishType;
        one.mode = m.mode;
        bool found = false;
        __try {
            __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
            bool reversed = false;
            __int64 seg = manager ? FindLocalSegment(manager, m.pieces[i], &reversed) : 0;
            if (seg) {
                const RoadSyncPoint* pts = *(const RoadSyncPoint**)(seg + SEG_POINTS_BEGIN_OFFSET);
                const RoadSyncPoint* pend = *(const RoadSyncPoint**)(seg + SEG_POINTS_END_OFFSET);
                int count = (pts && pend > pts) ? (int)(pend - pts) : 0;
                const float* n0 = (const float*)(*(__int64*)(seg + SEG_NODE0_OFFSET) + NODE_POS_OFFSET);
                const float* n1 = (const float*)(*(__int64*)(seg + SEG_NODE1_OFFSET) + NODE_POS_OFFSET);
                if (count > 0) memcpy(one.cursor, pts[count / 2].pos, 12);
                else for (int k = 0; k < 3; k++) one.cursor[k] = (n0[k] + n1[k]) * 0.5f;
                memcpy(one.rangeA, n0, 12);
                memcpy(one.rangeB, n1, 12);
                one.hasRange = 1;
                found = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!found) {
            Logf("republicmp  ROAD_SYNC:   demolish piece %d: no local segment (start=(%.2f,%.2f,%.2f) end=(%.2f,%.2f,%.2f) points=%d)",
                 i, m.pieces[i].segStart[0], m.pieces[i].segStart[1], m.pieces[i].segStart[2],
                 m.pieces[i].segEnd[0], m.pieces[i].segEnd[1], m.pieces[i].segEnd[2], m.pieces[i].segPointCount);
            continue;
        }
        bool removed = false;
        if (!ApplyDemolish(one, &removed)) ok = false;
        if (removed) *outRemoved = true;
    }
    return ok;
}

// True if the piece the sender demolished is still in this network's
// committed edges.
// The local segment of the piece the sender demolished, or 0 if it is gone.
static __int64 FindDemolishPiece(const RoadSyncDemolishMsg& m)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(m.category);
    if (!info) return 0;
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + info->managerOffset);
        if (!manager) return 0;
        if (m.pieceCount > 0) {
            for (int i = 0; i < m.pieceCount && i < 8; i++) {
                bool reversed = false;
                __int64 seg = FindLocalSegment(manager, m.pieces[i], &reversed);
                if (seg) return seg;
            }
            return 0;
        }
        return m.hasRange ? FindSegmentWithRange(manager, m.rangeA, m.rangeB) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool DemolishPieceStillHere(const RoadSyncDemolishMsg& m)
{
    return FindDemolishPiece(m) != 0;
}

// The building a segment belongs to (segment +0x138, e.g. the building whose
// access road it is). The demolish tool skips a segment whose owner is being
// demolished (owner +0xea8 set - fcaf0 at 0x4FE0B3): a piece next to a
// building the other PC is still pulling down cannot be picked until that
// demolition is over. Fills owner details for the log.
#define SEG_OWNER_OFFSET 0x138
#define OWNER_DEMOLISHING_OFFSET 0xea8
#define OWNER_PROGRESS_OFFSET 0x604
#define OWNER_POS_OFFSET 0x350

struct PieceOwner {
    bool has;
    bool demolishing;
    float pos[3];
    float progress;
};

// One line with a segment's state words, to compare a piece the demolish
// tool will not pick with one it does.
static void LogSegmentState(const char* what, __int64 seg)
{
    __try {
        Logf("republicmp  ROAD_SYNC:   %s: type %d variant %d +128=%d +129=%d planned(+140)=%d kind(+12c)=%d owner(+138)=%p points=%d",
             what, *(int*)(seg + 0x120), *(int*)(seg + 0x124), (int)*(unsigned char*)(seg + 0x128),
             (int)*(unsigned char*)(seg + 0x129), (int)*(unsigned char*)(seg + 0x140), *(int*)(seg + 0x12c),
             *(void**)(seg + 0x138),
             (int)((*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static PieceOwner ReadPieceOwner(__int64 seg)
{
    PieceOwner o = {};
    __try {
        __int64 owner = seg ? *(__int64*)(seg + SEG_OWNER_OFFSET) : 0;
        if (!owner) return o;
        o.has = true;
        o.demolishing = *(unsigned char*)(owner + OWNER_DEMOLISHING_OFFSET) != 0;
        memcpy(o.pos, (const float*)(owner + OWNER_POS_OFFSET), 12);
        o.progress = *(float*)(owner + OWNER_PROGRESS_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o.has = false;
    }
    return o;
}

// Outcome of one try at a remote demolition.
enum DemolishOutcome {
    DEMOLISH_DONE,       // removed, gone already, or failed - acked
    DEMOLISH_RETRY,      // not picked although still here - try again shortly
    DEMOLISH_WAIT_OWNER  // its building is still being demolished here - wait for that
};

// DEMOLISH_RETRY: in a batch of demolitions the neighbouring one, applied in
// the same frame, has just changed the junction, and the game settles it
// only in its next frame. DEMOLISH_WAIT_OWNER: see ReadPieceOwner.
static DemolishOutcome ApplyDemolishAndAck(const RoadSyncDemolishMsg& m, bool lastTry, bool ownerWaitOver)
{
    bool removed = false;
    bool ok = m.pieceCount > 0 ? ApplyDemolishPieces(m, &removed) : ApplyDemolish(m, &removed);
    const char* result;
    bool success;
    if (!ok) {
        result = "FAILED";
        success = false;
    } else if (removed) {
        result = "removed";
        success = true;
    } else if (!DemolishPieceStillHere(m)) {
        result = "already gone here";
        success = true;
    } else {
        PieceOwner owner = ReadPieceOwner(FindDemolishPiece(m));
        if (owner.demolishing && !ownerWaitOver) {
            if (!lastTry) // logged once, on the first try
                Logf("republicmp  ROAD_SYNC: demolish seq=%d %s at (%.2f,%.2f,%.2f) - its building at (%.1f,%.1f) is still being demolished here - waiting for it",
                     m.seq, CategoryName(m.category), m.cursor[0], m.cursor[1], m.cursor[2], owner.pos[0], owner.pos[2]);
            return DEMOLISH_WAIT_OWNER;
        }
        if (!lastTry) {
            Logf("republicmp  ROAD_SYNC: demolish seq=%d %s at (%.2f,%.2f,%.2f) - not picked, the piece is still here - trying again",
                 m.seq, CategoryName(m.category), m.cursor[0], m.cursor[1], m.cursor[2]);
            return DEMOLISH_RETRY;
        }
        LogSegmentState("the piece", FindDemolishPiece(m));
        if (owner.has)
            Logf("republicmp  ROAD_SYNC:   the piece belongs to a building at (%.1f,%.1f): %s, progress %.2f",
                 owner.pos[0], owner.pos[2], owner.demolishing ? "still being demolished" : "not being demolished",
                 owner.progress);
        result = "NOT removed - the piece is still here";
        success = false;
    }
    Logf("republicmp  ROAD_SYNC: demolish seq=%d %s at (%.2f,%.2f,%.2f) - %s", m.seq, CategoryName(m.category),
         m.cursor[0], m.cursor[1], m.cursor[2], result);
    SendAck(m.seq, m.category, success, 1);
    return DEMOLISH_DONE;
}

bool RoadSync_SegmentExists(const RoadSyncSegment& seg)
{
    return SegmentBetweenNodesExists(seg);
}

bool RoadSync_ApplySegmentForBuild(const RoadSyncSegment& seg)
{
    if (SegmentBetweenNodesExists(seg)) {
        Logf("republicmp  ROAD_SYNC: access road %s already exists here - skipped", CategoryName(seg.category));
        return true;
    }
    bool accepted = false;
    ApplySegment(seg, &accepted);
    return accepted;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

// One queued/received unit of work; segments and upgrades share one ordered
// stream so an upgrade never overtakes the segment it re-types.
struct RoadSyncItem {
    int tag; // ROAD_SYNC_WIRE_SEGMENT, _UPGRADE, _DEMOLISH or _DIRECTION
    RoadSyncMsg seg;
    RoadSyncUpgradeMsg up;
    RoadSyncDemolishMsg dem;
    RoadSyncDirectionMsg dir;
    RoadSyncPlanStartMsg plan;
    RoadSyncGroupToolMsg group;
    int attempts;                // demolish: tries so far
    unsigned long long notBeforeMs; // demolish: GetTickCount64() of the next try
    unsigned long long waitSinceMs; // demolish: waiting for its building's demolition since
};

#define DEMOLISH_MAX_ATTEMPTS 4
#define DEMOLISH_RETRY_MS 250
// A piece whose building is still being demolished here waits aside (the
// queue goes on), checked every DEMOLISH_OWNER_POLL_MS for up to
// DEMOLISH_OWNER_WAIT_MS - the other PC's demolition sites work at their
// own pace.
#define DEMOLISH_OWNER_POLL_MS 1000
#define DEMOLISH_OWNER_WAIT_MS 180000
static std::vector<RoadSyncItem> g_waitingDemolish;

static volatile bool g_connected = false;
static volatile bool g_connecting = false;
// Set by Reset to abandon a pending accept/connect (polled every ~200ms).
static volatile bool g_cancelConnect = false;
static volatile bool g_isHost = false;
static SOCKET g_sock = INVALID_SOCKET;
static char g_targetIp[64] = ROAD_SYNC_TEST_HOST_IP;
static std::vector<RoadSyncItem> g_incoming;          // receive thread -> game thread
static std::vector<RoadSyncAckMsg> g_incomingAcks;    // receive thread -> game thread

static void CloseConnection()
{
    g_connected = false;
    NetworkCloseSocket(g_sock);
    g_sock = INVALID_SOCKET;
}

static bool SendEnvelope(int tag, const void* payload, size_t payloadSize)
{
    if (!NetworkSendExact(g_sock, &tag, sizeof(tag))) return false;
    return NetworkSendExact(g_sock, payload, payloadSize);
}

static void LogSegmentPoints(const char* label, int seq, const RoadSyncSegment& seg)
{
#if ROAD_SYNC_VERBOSE_LOG
    for (int i = 0; i < seg.pointCount; i++) {
        Logf("republicmp  ROAD_SYNC:   %s[seq=%d][%d] x=%.9g y=%.9g z=%.9g", label, seq, i,
             seg.points[i].pos[0], seg.points[i].pos[1], seg.points[i].pos[2]);
    }
#else
    (void)label; (void)seq; (void)seg;
#endif
}

// Receives tagged payloads and hands them to the game thread.
static DWORD WINAPI ReceiveLoopThread(LPVOID)
{
    while (g_connected) {
        int tag = 0;
        if (!NetworkRecvExact(g_sock, &tag, sizeof(tag))) {
            RoadSyncStatus("[ROAD_SYNC] connection lost");
            CloseConnection();
            break;
        }

        if (tag == ROAD_SYNC_WIRE_SEGMENT || tag == ROAD_SYNC_WIRE_UPGRADE) {
            RoadSyncItem* item = new RoadSyncItem(); // ~50KB, keep it off the thread stack
            item->tag = tag;
            bool ok, badCount = false;
            if (tag == ROAD_SYNC_WIRE_SEGMENT) {
                // Header, then exactly pointCount points (see road_sync.h).
                ok = NetworkRecvExact(g_sock, &item->seg, ROAD_SYNC_MSG_HEADER_BYTES);
                if (ok && !RoadSync_PointCountValid(item->seg.seg.pointCount)) badCount = true;
                else if (ok && item->seg.seg.pointCount > 0)
                    ok = NetworkRecvExact(g_sock, item->seg.seg.points, RoadSync_PointBytes(item->seg.seg.pointCount));
            } else {
                ok = NetworkRecvExact(g_sock, &item->up, sizeof(item->up));
            }
            if (badCount) {
                // The next message starts after the points - with a bad count
                // there is no telling where, so the stream cannot go on.
                Logf("republicmp  ROAD_SYNC: received segment with bad pointCount=%d - dropping the connection",
                     item->seg.seg.pointCount);
                delete item;
                RoadSyncStatus("[ROAD_SYNC] bad segment on the wire, dropping connection");
                CloseConnection();
                break;
            }
            if (!ok) {
                delete item;
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }

            if (tag == ROAD_SYNC_WIRE_SEGMENT) {
                const RoadSyncSegment& seg = item->seg.seg;
                Logf("republicmp  ROAD_SYNC: received seq=%d %s type=%d variant=%d points=%d",
                     item->seg.seq, CategoryName(seg.category), seg.typeId, seg.variant, seg.pointCount);
                LogSegmentPoints("recv", item->seg.seq, seg);
            } else {
                Logf("republicmp  ROAD_SYNC: received upgrade seq=%d %s type=%d variant=%d pieces=%d",
                     item->up.seq, CategoryName(item->up.category), item->up.newType, item->up.newVariant, item->up.pieceCount);
            }

            EnterCriticalSection(&g_queueLock);
            g_incoming.push_back(*item);
            LeaveCriticalSection(&g_queueLock);
            delete item;
        } else if (tag == ROAD_SYNC_WIRE_DEMOLISH) {
            RoadSyncItem* item = new RoadSyncItem();
            item->tag = tag;
            if (!NetworkRecvExact(g_sock, &item->dem, sizeof(item->dem))) {
                delete item;
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            Logf("republicmp  ROAD_SYNC: received demolish seq=%d %s at (%.2f,%.2f,%.2f)", item->dem.seq,
                 CategoryName(item->dem.category), item->dem.cursor[0], item->dem.cursor[1], item->dem.cursor[2]);
            EnterCriticalSection(&g_queueLock);
            g_incoming.push_back(*item);
            LeaveCriticalSection(&g_queueLock);
            delete item;
        } else if (tag == ROAD_SYNC_WIRE_PLAN_START) {
            RoadSyncItem* item = new RoadSyncItem();
            item->tag = tag;
            if (!NetworkRecvExact(g_sock, &item->plan, sizeof(item->plan))) {
                delete item;
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            Logf("republicmp  ROAD_SYNC: received plan start seq=%d %s", item->plan.seq, CategoryName(item->plan.category));
            EnterCriticalSection(&g_queueLock);
            g_incoming.push_back(*item);
            LeaveCriticalSection(&g_queueLock);
            delete item;
        } else if (tag == ROAD_SYNC_WIRE_GROUP_TOOL) {
            RoadSyncItem* item = new RoadSyncItem();
            item->tag = tag;
            if (!NetworkRecvExact(g_sock, &item->group, sizeof(item->group))) {
                delete item;
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            Logf("republicmp  ROAD_SYNC: received group tool seq=%d mode %d", item->group.seq, item->group.mode);
            EnterCriticalSection(&g_queueLock);
            g_incoming.push_back(*item);
            LeaveCriticalSection(&g_queueLock);
            delete item;
        } else if (tag == ROAD_SYNC_WIRE_DIRECTION) {
            RoadSyncItem* item = new RoadSyncItem();
            item->tag = tag;
            if (!NetworkRecvExact(g_sock, &item->dir, sizeof(item->dir))) {
                delete item;
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            Logf("republicmp  ROAD_SYNC: received direction seq=%d %s -> %d", item->dir.seq,
                 CategoryName(item->dir.category), item->dir.direction);
            EnterCriticalSection(&g_queueLock);
            g_incoming.push_back(*item);
            LeaveCriticalSection(&g_queueLock);
            delete item;
        } else if (tag == ROAD_SYNC_WIRE_ACK) {
            RoadSyncAckMsg ack = {};
            if (!NetworkRecvExact(g_sock, &ack, sizeof(ack))) {
                RoadSyncStatus("[ROAD_SYNC] connection lost");
                CloseConnection();
                break;
            }
            EnterCriticalSection(&g_queueLock);
            g_incomingAcks.push_back(ack);
            LeaveCriticalSection(&g_queueLock);
        } else {
            // Framing is out of sync (mismatched builds) - cannot recover.
            RoadSyncStatus("[ROAD_SYNC] unknown wire tag %d, dropping connection (mismatched build?)", tag);
            CloseConnection();
            break;
        }
    }
    return 0;
}

static void StartConnection(SOCKET sock)
{
    NetworkActivateChannelSocket(&g_sock, sock, &g_connected, ReceiveLoopThread);
    RoadSyncStatus("[ROAD_SYNC] connection established");
}

static DWORD WINAPI HostAcceptWorkerThread(LPVOID)
{
    RoadSyncStatus("[ROAD_SYNC] HOST, waiting for a client on port %d...", ROAD_SYNC_PORT);
    if (!NetworkInit()) {
        RoadSyncStatus("[ROAD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkHostAcceptOne(ROAD_SYNC_PORT, 300000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        RoadSyncStatus("[ROAD_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
    if (sock == INVALID_SOCKET) {
        RoadSyncStatus("[ROAD_SYNC] no client connected within 5 min, aborting - press Host##rs again to retry");
        g_connecting = false;
        return 0;
    }
    RoadSyncStatus("[ROAD_SYNC] client connected");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

static DWORD WINAPI ClientConnectWorkerThread(LPVOID)
{
    RoadSyncStatus("[ROAD_SYNC] CLIENT, connecting to host %s:%d...", g_targetIp, ROAD_SYNC_PORT);
    if (!NetworkInit()) {
        RoadSyncStatus("[ROAD_SYNC] network init failed, aborting");
        g_connecting = false;
        return 0;
    }
    SOCKET sock = NetworkConnect(g_targetIp, ROAD_SYNC_PORT, 15000, 30000, &g_cancelConnect);
    if (sock == INVALID_SOCKET && g_cancelConnect) {
        RoadSyncStatus("[ROAD_SYNC] stopped waiting for the connection");
        g_connecting = false;
        return 0;
    }
    if (sock == INVALID_SOCKET) {
        RoadSyncStatus("[ROAD_SYNC] couldn't connect - is the host listening (Host##rs)? Is the IP right? - press Connect##rs again to retry");
        g_connecting = false;
        return 0;
    }
    RoadSyncStatus("[ROAD_SYNC] connected to host");
    StartConnection(sock);
    g_connecting = false;
    return 0;
}

void RoadSync_StartHost()
{
    NetworkLaunchChannelWorker(true, nullptr, g_targetIp, sizeof(g_targetIp),
                               &g_connected, &g_connecting, &g_isHost, &g_cancelConnect,
                               HostAcceptWorkerThread);
}

void RoadSync_StartClient(const char* hostIp)
{
    NetworkLaunchChannelWorker(false, hostIp, g_targetIp, sizeof(g_targetIp),
                               &g_connected, &g_connecting, &g_isHost, &g_cancelConnect,
                               ClientConnectWorkerThread);
}

void RoadSync_Reset()
{
    if (g_connecting) g_cancelConnect = true;
    if (g_connected) {
        RoadSyncStatus("[ROAD_SYNC] connection reset by user");
        CloseConnection();
    }
}

bool RoadSync_IsConnected()  { return g_connected; }
bool RoadSync_IsConnecting() { return g_connecting; }
bool RoadSync_IsHostRole()   { return g_isHost; }
const char* RoadSync_GetTargetIp() { return g_targetIp; }

// ---------------------------------------------------------------------------
// Sending (game thread only)
// ---------------------------------------------------------------------------

// Sent items waiting for their ack - used only to flag unmatched acks.
static std::vector<int> g_pendingSends;
static int g_nextSeq = 0;
// Items captured while disconnected, sent in order once connected.
static std::vector<RoadSyncItem> g_outboundQueue;

static void SendItemNow(RoadSyncItem& item)
{
    int seq = ++g_nextSeq;
    bool sent;
    if (item.tag == ROAD_SYNC_WIRE_PLAN_START) {
        RoadSyncPlanStartMsg& plan = item.plan;
        plan.seq = seq;
        char mode[96];
        Logf("republicmp  ROAD_SYNC: sending plan start seq=%d %s%s dollars=%d rubles=%d count=%d group=%d pay=%s - %d plan(s) started here",
             seq, CategoryName(plan.category), plan.exceptGroup ? " (every group but one)" : "", plan.dollars,
             plan.rubles, plan.count, plan.group, BuildMode_Describe(plan.mode, mode, sizeof(mode)), plan.started);
        sent = SendEnvelope(ROAD_SYNC_WIRE_PLAN_START, &plan, sizeof(plan));
    } else if (item.tag == ROAD_SYNC_WIRE_GROUP_TOOL) {
        RoadSyncGroupToolMsg& g = item.group;
        g.seq = seq;
        char mode[96];
        Logf("republicmp  ROAD_SYNC: sending group tool seq=%d mode %d box (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) yaw %.4f pay=%s - %d planned piece(s) started here",
             seq, g.mode, g.box[0], g.box[1], g.box[2], g.box[3], g.box[4], g.box[5], g.yaw,
             BuildMode_Describe(g.buildMode, mode, sizeof(mode)), g.started);
        sent = SendEnvelope(ROAD_SYNC_WIRE_GROUP_TOOL, &g, sizeof(g));
    } else if (item.tag == ROAD_SYNC_WIRE_DIRECTION) {
        RoadSyncDirectionMsg& dir = item.dir;
        dir.seq = seq;
        Logf("republicmp  ROAD_SYNC: sending direction seq=%d %s (%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) -> %d",
             seq, CategoryName(dir.category), dir.piece.segStart[0], dir.piece.segStart[1], dir.piece.segStart[2],
             dir.piece.segEnd[0], dir.piece.segEnd[1], dir.piece.segEnd[2], dir.direction);
        sent = SendEnvelope(ROAD_SYNC_WIRE_DIRECTION, &dir, sizeof(dir));
    } else if (item.tag == ROAD_SYNC_WIRE_DEMOLISH) {
        RoadSyncDemolishMsg& dem = item.dem;
        dem.seq = seq;
        Logf("republicmp  ROAD_SYNC: sending demolish seq=%d %s at (%.2f,%.2f,%.2f)",
             seq, CategoryName(dem.category), dem.cursor[0], dem.cursor[1], dem.cursor[2]);
        sent = SendEnvelope(ROAD_SYNC_WIRE_DEMOLISH, &dem, sizeof(dem));
    } else if (item.tag == ROAD_SYNC_WIRE_UPGRADE) {
        RoadSyncUpgradeMsg& up = item.up;
        up.seq = seq;
        char mode[96];
        Logf("republicmp  ROAD_SYNC: sending upgrade seq=%d %s type=%d variant=%d pieces=%d pay=%s",
             seq, CategoryName(up.category), up.newType, up.newVariant, up.pieceCount,
             BuildMode_Describe(up.mode, mode, sizeof(mode)));
        sent = SendEnvelope(ROAD_SYNC_WIRE_UPGRADE, &up, sizeof(up));
    } else {
        RoadSyncMsg& msg = item.seg;
        msg.seq = seq;
        char mode[96];
        Logf("republicmp  ROAD_SYNC: sending seq=%d %s type=%d variant=%d points=%d pay=%s",
             seq, CategoryName(msg.seg.category), msg.seg.typeId, msg.seg.variant, msg.seg.pointCount,
             BuildMode_Describe(msg.mode, mode, sizeof(mode)));
        LogSegmentPoints("send", seq, msg.seg);
        sent = RoadSync_PointCountValid(msg.seg.pointCount) &&
               SendEnvelope(ROAD_SYNC_WIRE_SEGMENT, &msg,
                            ROAD_SYNC_MSG_HEADER_BYTES + RoadSync_PointBytes(msg.seg.pointCount));
    }

    if (!sent) {
        RoadSyncStatus("[ROAD_SYNC] send failed, dropping connection");
        CloseConnection();
        return;
    }
    g_pendingSends.push_back(seq);
}

static void FlushOutboundQueue()
{
    while (g_connected && !g_outboundQueue.empty()) {
        SendItemNow(g_outboundQueue.front());
        if (!g_connected) break; // send failed - keep it for the next connection
        g_outboundQueue.erase(g_outboundQueue.begin());
    }
}

static int SendOrQueue(RoadSyncItem& item)
{
    if (!g_connected || !g_outboundQueue.empty()) {
        g_outboundQueue.push_back(item);
        Logf("republicmp  ROAD_SYNC: queued %s (%zu waiting for connection)",
             item.tag == ROAD_SYNC_WIRE_UPGRADE ? "upgrade" : item.tag == ROAD_SYNC_WIRE_DIRECTION ? "direction"
             : item.tag == ROAD_SYNC_WIRE_PLAN_START ? "plan start"
             : item.tag == ROAD_SYNC_WIRE_GROUP_TOOL ? "group tool" : "segment",
             g_outboundQueue.size());
        return 0;
    }
    SendItemNow(item);
    return g_connected ? g_nextSeq : -1;
}

template <typename PayloadT, typename AssignFn>
static int SendRoadWirePayload(int count, int minCount, int maxCount, const char* what, int wireTag,
                               const PayloadT& payload, AssignFn assignFn)
{
    if (count < minCount || count > maxCount) {
        Logf("republicmp  ROAD_SYNC: refusing to send %s, count=%d", what, count);
        return -1;
    }
    static RoadSyncItem s_item; // large - game thread only
    memset(&s_item, 0, sizeof(s_item));
    s_item.tag = wireTag;
    assignFn(s_item, payload);
    return SendOrQueue(s_item);
}

int RoadSync_SendSegment(const RoadSyncSegment& seg)
{
    return SendRoadWirePayload<RoadSyncSegment>(
        seg.pointCount, 0, ROAD_SYNC_MAX_POINTS, "segment", ROAD_SYNC_WIRE_SEGMENT, seg,
        [](RoadSyncItem& item, const RoadSyncSegment& s) { item.seg.seg = s; item.seg.terrainFence = TerrainSync_GetFenceSeq(); item.seg.mode = BuildMode_Read(); });
}

int RoadSync_SendDemolish(int category, const float cursor[3], const float* rangeA, const float* rangeB,
                          const RoadSyncUpgradePiece* pieces, int pieceCount)
{
    static RoadSyncItem s_item; // large - game thread only
    memset(&s_item, 0, sizeof(s_item));
    s_item.tag = ROAD_SYNC_WIRE_DEMOLISH;
    s_item.dem.category = category;
    s_item.dem.cursor[0] = cursor[0];
    s_item.dem.cursor[1] = cursor[1];
    s_item.dem.cursor[2] = cursor[2];
    s_item.dem.terrainFence = TerrainSync_GetFenceSeq();
    if (rangeA && rangeB) {
        s_item.dem.hasRange = 1;
        memcpy(s_item.dem.rangeA, rangeA, sizeof(s_item.dem.rangeA));
        memcpy(s_item.dem.rangeB, rangeB, sizeof(s_item.dem.rangeB));
    }
    if (pieces && pieceCount > 0) {
        int n = pieceCount < 8 ? pieceCount : 8;
        memcpy(s_item.dem.pieces, pieces, n * sizeof(RoadSyncUpgradePiece));
        s_item.dem.pieceCount = n;
    }
    return SendOrQueue(s_item);
}

int RoadSync_SendUpgrade(const RoadSyncUpgradeMsg& up)
{
    return SendRoadWirePayload<RoadSyncUpgradeMsg>(
        up.pieceCount, 1, ROAD_SYNC_MAX_UPGRADE_PIECES, "upgrade", ROAD_SYNC_WIRE_UPGRADE, up,
        [](RoadSyncItem& item, const RoadSyncUpgradeMsg& u) { item.up = u; item.up.terrainFence = TerrainSync_GetFenceSeq(); item.up.mode = BuildMode_Read(); });
}

// ---------------------------------------------------------------------------
// Applying received work (game thread only)
// ---------------------------------------------------------------------------

static std::vector<RoadSyncItem> g_pending;
static bool g_warnedWorldNotLoaded = false;
static ULONGLONG g_fenceWaitStartMs = 0; // when the head item started waiting for its terrain fence

// True when the item may be applied now: its terrain fence is reached, or it
// has waited ROAD_SYNC_FENCE_TIMEOUT_MS for it.
static bool TerrainFenceAllows(const RoadSyncItem& item)
{
    if (item.tag == ROAD_SYNC_WIRE_DIRECTION) return true; // no terrain involved
    int fence = item.tag == ROAD_SYNC_WIRE_UPGRADE ? item.up.terrainFence
              : item.tag == ROAD_SYNC_WIRE_DEMOLISH ? item.dem.terrainFence
              : item.tag == ROAD_SYNC_WIRE_PLAN_START ? item.plan.terrainFence
              : item.tag == ROAD_SYNC_WIRE_GROUP_TOOL ? item.group.terrainFence
              : item.seg.terrainFence;
    if (TerrainSync_FenceReached(fence)) {
        if (g_fenceWaitStartMs)
            Logf("republicmp  ROAD_SYNC: terrain fence %d reached after %llums", fence, GetTickCount64() - g_fenceWaitStartMs);
        g_fenceWaitStartMs = 0;
        return true;
    }
    ULONGLONG now = GetTickCount64();
    if (!g_fenceWaitStartMs) g_fenceWaitStartMs = now;
    if (now - g_fenceWaitStartMs < ROAD_SYNC_FENCE_TIMEOUT_MS) return false;
    Logf("republicmp  ROAD_SYNC: terrain fence %d not reached after %dms - applying anyway", fence, ROAD_SYNC_FENCE_TIMEOUT_MS);
    g_fenceWaitStartMs = 0;
    return true;
}

static void SendAck(int seq, int category, bool success, int attempts)
{
    RoadSyncAckMsg ack = {};
    ack.seq = seq;
    ack.category = category;
    ack.success = success ? 1 : 0;
    ack.attempts = attempts;
    if (g_connected && !SendEnvelope(ROAD_SYNC_WIRE_ACK, &ack, sizeof(ack))) {
        RoadSyncStatus("[ROAD_SYNC] ack send failed, dropping connection");
        CloseConnection();
    }
}

// Applies a received segment, retrying only if the replay threw, and acks
// the engine's verdict. A segment already present between the same nodes is
// acked as built without replaying it.
static void ApplySegmentAndAck(const RoadSyncMsg& msg)
{
    const RoadSyncSegment& seg = msg.seg;
    if (SegmentBetweenNodesExists(seg)) {
        Logf("republicmp  ROAD_SYNC: seq=%d %s already exists between these nodes - skipped",
             msg.seq, CategoryName(seg.category));
        SendAck(msg.seq, seg.category, true, 0);
        return;
    }

    // Built the way the sender chose (rubles / dollars / realistic); the
    // local player's own choice is put back right after.
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(msg.mode);
    bool ok = false;
    bool engineAccepted = false;
    int attempt = 1;
    for (; attempt <= ROAD_SYNC_MAX_APPLY_ATTEMPTS; attempt++) {
        ok = ApplySegment(seg, &engineAccepted);
        if (ok) break;
        // A throw inside the engine is not transient, and the throw may have
        // left a half-built path behind (a start node, say): a retry on top
        // of that is what turned one failed path into a crash. A re-sync
        // brings the path over instead.
        if (g_lastApplyThrew) {
            Logf("republicmp  ROAD_SYNC: seq=%d threw inside the engine - not retried", msg.seq);
            break;
        }
    }
    BuildMode_Write(mine);
    if (!ok && attempt > ROAD_SYNC_MAX_APPLY_ATTEMPTS) attempt = ROAD_SYNC_MAX_APPLY_ATTEMPTS;

    char theirs[96], local[96];
    Logf("republicmp  ROAD_SYNC: seq=%d %s after %d attempt(s), pay=%s (mine: %s)", msg.seq,
         !ok ? "GAVE UP" : engineAccepted ? "applied" : "REJECTED by the engine", attempt,
         BuildMode_Describe(msg.mode, theirs, sizeof(theirs)), BuildMode_Describe(mine, local, sizeof(local)));
    SendAck(msg.seq, seg.category, ok && engineAccepted, attempt);
}

static void ApplyUpgradeAndAck(const RoadSyncUpgradeMsg& up)
{
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(up.mode);
    int applied = ApplyUpgrade(up);
    BuildMode_Write(mine);
    char theirs[96], local[96];
    Logf("republicmp  ROAD_SYNC: upgrade seq=%d %s applied %d/%d piece(s), pay=%s (mine: %s)",
         up.seq, CategoryName(up.category), applied, up.pieceCount,
         BuildMode_Describe(up.mode, theirs, sizeof(theirs)), BuildMode_Describe(mine, local, sizeof(local)));
    SendAck(up.seq, up.category, applied == up.pieceCount, 1);
}

// ---------------------------------------------------------------------------
// Lockstep commands (see road_sync.h)
// ---------------------------------------------------------------------------

// The PC that drew the segment did not make its commit, so its tool still
// shows the finished drag; the game's own preview cleanup (542a30) clears it
// before the replay starts a drag of its own on the same manager. Skipped
// while the local player is dragging again.
static void ClearCommandPreview(int category)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(category);
    if (!info) return;
    __try {
        __int64 thisPtr = GetWorldThisPtr();
        __int64 manager = *(__int64*)(thisPtr + info->managerOffset);
        if (manager && *(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) == 0) {
            ResetManagerDragState(manager);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void RoadSync_ClearCategoryPreview(int category)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(category);
    if (!info) return;
    __try {
        __int64 thisPtr = GetWorldThisPtr();
        __int64 manager = *(__int64*)(thisPtr + info->managerOffset);
        if (manager) {
            ResetManagerDragState(manager);
        }
        if (category == ROAD_SYNC_CATEGORY_PEDESTRIAN) {
            *(long long*)((uintptr_t)g_exe + HOVER_ROAD_EDGE_RVA) = 0;
            *(int*)((uintptr_t)g_exe + HOVER_ROAD_POINT_RVA) = 0;
            *(long long*)((uintptr_t)g_exe + HOVER_ROAD_NODE_RVA) = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void RoadSync_CancelLocalDrag()
{
    __try {
        __int64 thisPtr = GetWorldThisPtr();
        if (*(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) == 0) return;
        __int64 secondRoadMgr = *(__int64*)(thisPtr + 0xF7A8);
        __int64* sqBegin = *(__int64**)(thisPtr + 0xF608);
        __int64* sqEnd = *(__int64**)(thisPtr + 0xF610);
        if (secondRoadMgr && sqBegin && sqEnd > sqBegin && (sqEnd - sqBegin) <= 4096) {
            typedef void (*CancelSegFn)(__int64, __int64, int, int, unsigned char);
            CancelSegFn cancelSeg = (CancelSegFn)((uintptr_t)g_exe + 0x5434C0);
            for (__int64* it = sqBegin; it < sqEnd; ++it) {
                if (*it) cancelSeg(secondRoadMgr, *it, 0, -1, 0);
            }
            *(__int64**)(thisPtr + 0xF610) = sqBegin;
        }
        for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; c++) {
            __int64 manager = *(__int64*)(thisPtr + g_categories[c].managerOffset);
            if (manager) ResetManagerDragState(manager);
        }
        ClearHoveredPort();
        HoverGlobals_Clear();
        *(char*)(thisPtr + TOOL_DRAG_ACTIVE_OFFSET) = 0;
        Logf("republicmp  ROAD_SYNC: the local drag was cancelled for a lockstep command");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool RoadSync_ApplyCommandSegment(const RoadSyncMsg& msg)
{
    const RoadSyncSegment& seg = msg.seg;
    ClearCommandPreview(seg.category);
    if (SegmentBetweenNodesExists(seg)) {
        Logf("republicmp  ROAD_SYNC: command %s already exists between these nodes - skipped", CategoryName(seg.category));
        return false;
    }
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(msg.mode);
    bool engineAccepted = false;
    bool ok = ApplySegment(seg, &engineAccepted);
    BuildMode_Write(mine);
    return ok && engineAccepted;
}

bool RoadSync_ApplyCommandUpgrade(const RoadSyncUpgradeMsg& msg)
{
    ClearCommandPreview(msg.category);
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(msg.mode);
    int applied = ApplyUpgrade(msg);
    BuildMode_Write(mine);
    Logf("republicmp  ROAD_SYNC: command upgrade %s applied %d/%d piece(s)", CategoryName(msg.category), applied, msg.pieceCount);
    return applied == msg.pieceCount;
}

bool RoadSync_ApplyCommandDemolish(const RoadSyncDemolishMsg& msg)
{
    bool removed = false;
    bool ok = msg.pieceCount > 0 ? ApplyDemolishPieces(msg, &removed) : ApplyDemolish(msg, &removed);
    return ok && removed;
}

// ---------------------------------------------------------------------------
// Factory connection direction (game thread only, see RoadSyncDirectionMsg)
// ---------------------------------------------------------------------------

#define SEG_DIRECTION_OFFSET 0x1bc       // int: 0 both ways, 1 / 2 one way (node 0 / node 1 relative)
#define NODE_BUILDINGS_BEGIN_OFFSET 0x70 // node: buildings attached to it
#define NODE_BUILDINGS_END_OFFSET 0x78
#define REFRESH_BUILDING_RVA 0x191950    // FUN_140191950(thisPtr, building)
#define DIR_SCAN_INTERVAL_MS 250
#define DIR_SCAN_MAX 4096

typedef void (*RefreshBuildingFn)(__int64 thisPtr, __int64 building);

// Last direction seen per local connection segment.
static std::unordered_map<__int64, int> g_knownDirections;
static ULONGLONG g_lastDirScanMs = 0;

struct DirSnapshot {
    __int64 seg;
    int direction;
    RoadSyncUpgradePiece piece;
};
static DirSnapshot g_dirSnapshot[DIR_SCAN_MAX];

// Copies every factory connection's direction and identity; -1 on a fault.
static int ReadFactoryDirections()
{
    int n = 0;
    __try {
        __int64 manager = *(__int64*)(GetWorldThisPtr() + g_categories[ROAD_SYNC_CATEGORY_FACTORYCONN].managerOffset);
        if (!manager) return 0;
        __int64* it = *(__int64**)(manager + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(manager + MGR_SEGMENTS_END_OFFSET);
        for (; it && it < end && n < DIR_SCAN_MAX; ++it) {
            __int64 seg = *it;
            if (!seg) continue;
            __int64 n0 = *(__int64*)(seg + SEG_NODE0_OFFSET);
            __int64 n1 = *(__int64*)(seg + SEG_NODE1_OFFSET);
            if (!n0 || !n1) continue;
            DirSnapshot& s = g_dirSnapshot[n++];
            memset(&s, 0, sizeof(s));
            s.seg = seg;
            s.direction = *(int*)(seg + SEG_DIRECTION_OFFSET);
            memcpy(s.piece.segStart, (const float*)(n0 + NODE_POS_OFFSET), sizeof(s.piece.segStart));
            memcpy(s.piece.segEnd, (const float*)(n1 + NODE_POS_OFFSET), sizeof(s.piece.segEnd));
            s.piece.segPointCount =
                (int)((*(__int64*)(seg + SEG_POINTS_END_OFFSET) - *(__int64*)(seg + SEG_POINTS_BEGIN_OFFSET)) / 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// Sends every connection whose direction changed since the last scan. New
// connections are only recorded - they start out the same on both sides.
static void ScanFactoryDirections()
{
    int n = ReadFactoryDirections();
    if (n < 0) return;
    std::unordered_map<__int64, int> seen;
    seen.reserve(n);
    for (int i = 0; i < n; i++) {
        const DirSnapshot& s = g_dirSnapshot[i];
        seen[s.seg] = s.direction;
        auto known = g_knownDirections.find(s.seg);
        if (known == g_knownDirections.end() || known->second == s.direction) continue;
        static RoadSyncItem s_item; // large - game thread only
        memset(&s_item, 0, sizeof(s_item));
        s_item.tag = ROAD_SYNC_WIRE_DIRECTION;
        s_item.dir.category = ROAD_SYNC_CATEGORY_FACTORYCONN;
        s_item.dir.piece = s.piece;
        s_item.dir.direction = s.direction;
        SendOrQueue(s_item);
    }
    g_knownDirections.swap(seen);
}

// Writes the direction into the local segment and refreshes its buildings.
// Returns the segment (0 = not found or faulted).
static __int64 ApplyDirection(const RoadSyncDirectionMsg& m, int* outLocalDirection)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(m.category);
    if (!info || m.direction < 0 || m.direction > 2) return 0;
    __int64 thisPtr = GetWorldThisPtr();
    RefreshBuildingFn refresh = (RefreshBuildingFn)((uintptr_t)g_exe + REFRESH_BUILDING_RVA);
    __try {
        __int64 manager = *(__int64*)(thisPtr + info->managerOffset);
        bool reversed = false;
        __int64 seg = manager ? FindLocalSegment(manager, m.piece, &reversed) : 0;
        if (!seg) return 0;
        int dir = (reversed && m.direction) ? 3 - m.direction : m.direction;
        *(int*)(seg + SEG_DIRECTION_OFFSET) = dir;
        __int64 nodes[2] = { *(__int64*)(seg + SEG_NODE0_OFFSET), *(__int64*)(seg + SEG_NODE1_OFFSET) };
        for (__int64 node : nodes) {
            if (!node) continue;
            __int64* first = *(__int64**)(node + NODE_BUILDINGS_BEGIN_OFFSET);
            __int64* last = *(__int64**)(node + NODE_BUILDINGS_END_OFFSET);
            if (first && first < last && *first) refresh(thisPtr, *first);
        }
        *outLocalDirection = dir;
        return seg;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static void ApplyDirectionAndAck(const RoadSyncDirectionMsg& m)
{
    int localDirection = 0;
    __int64 seg = ApplyDirection(m, &localDirection);
    // Recorded as seen, so the next scan does not send it back.
    if (seg) g_knownDirections[seg] = localDirection;
    Logf("republicmp  ROAD_SYNC: direction seq=%d %s -> %d %s", m.seq, CategoryName(m.category), m.direction,
         seg ? "applied" : "NOT APPLIED - connection not found");
    SendAck(m.seq, m.category, seg != 0, 1);
}

// ---------------------------------------------------------------------------
// Starting planned construction (see RoadSyncPlanStartMsg)
// ---------------------------------------------------------------------------

#define PLAN_START_RVA 0x5258E0        // FUN_1405258e0(network, dollars, rubles, count, group)
#define PLAN_START_EXCEPT_RVA 0x525C30 // FUN_140525c30(network, dollars, rubles, -, group)
#define EDGE_PLANNED_OFFSET 0x140      // byte: 1 = a plan not started yet
#define EDGE_GROUP_OFFSET 0x124
#define PLAN_CAPTURE_MAX 16

// mov rax,rsp / mov [rax+10h],dl / push rbp,rsi,rdi,r12,r13,r14 - 15 bytes
static const unsigned char kPlanStartExpect[15] = {
    0x48, 0x8B, 0xC4, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56
};
// mov rax,rsp / mov [rax+18h],r8b / mov [rax+10h],dl / push rbp,rsi,rdi,r12 - 15 bytes
static const unsigned char kPlanStartExceptExpect[15] = {
    0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x88, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54
};

// The interface's calls (return addresses = CALL + 5): the construction
// menu (FUN_140086f40), the construction window and the network tool panel
// (FUN_140395c90). The others - a building's placement, the area paste -
// start their own plans as part of an action that is synced itself.
static const unsigned int kPlanStartUiReturns[] = {
    0x7ECE0, 0x7EF10, 0x7F100, 0x881B1, 0x88A11, 0x88CC1, 0x39F0C0, 0x39F6F8, 0x39F8D8
};
static const unsigned int kPlanStartExceptUiReturns[] = { 0x7ECBA, 0x7EEEA, 0x7F0DA, 0x8818D, 0x889ED, 0x88C9D };

typedef __int64 (*PlanStartFn)(__int64 network, unsigned char dollars, unsigned char rubles, __int64 count, __int64 group);
static PlanStartFn g_origPlanStart = nullptr;
static PlanStartFn g_origPlanStartExcept = nullptr;

// Captured in the hook (interface thread), sent from RoadSync_Tick.
static RoadSyncPlanStartMsg g_planCaptures[PLAN_CAPTURE_MAX];
static int g_planCaptureCount = 0;

static bool IsUiCall(void* ret, const unsigned int* rvas, size_t n)
{
    unsigned long long rva = (unsigned long long)((BYTE*)ret - (BYTE*)g_exe);
    for (size_t i = 0; i < n; i++)
        if (rva == rvas[i]) return true;
    return false;
}

// Plans the call would start: planned edges among the last `count` (all if
// count <= 0), in `group` (or every group but it, or all when group < 0).
static int CountStartablePlans(__int64 network, bool exceptGroup, int count, int group)
{
    int n = 0;
    __try {
        __int64* begin = *(__int64**)(network + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(network + MGR_SEGMENTS_END_OFFSET);
        if (!begin || end < begin) return 0;
        long long size = end - begin;
        long long from = (!exceptGroup && count > 0 && count < size) ? size - count : 0;
        for (long long i = from; i < size; i++) {
            __int64 e = begin[i];
            if (!e || *(unsigned char*)(e + EDGE_PLANNED_OFFSET) != 1) continue;
            int g = *(int*)(e + EDGE_GROUP_OFFSET);
            if (group >= 0 && (exceptGroup ? g == group : g != group)) continue;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

static __int64 PlanStartCommon(bool exceptGroup, void* ret, __int64 network, unsigned char dollars,
                               unsigned char rubles, __int64 count, __int64 group)
{
    PlanStartFn orig = exceptGroup ? g_origPlanStartExcept : g_origPlanStart;
    if (SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning() && !IsDualTestbed())
        return orig(network, dollars, rubles, count, group);

    if (InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
        (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded()))) {
        SaveSync_NotifyWaitingLobbyActionBlocked();
        return 0;
    }
    // Lockstep: the button press becomes a command, made on every PC.
    if (Commands_TakeLocal() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
        (exceptGroup ? IsUiCall(ret, kPlanStartExceptUiReturns, sizeof(kPlanStartExceptUiReturns) / sizeof(kPlanStartExceptUiReturns[0]))
                     : IsUiCall(ret, kPlanStartUiReturns, sizeof(kPlanStartUiReturns) / sizeof(kPlanStartUiReturns[0])))) {
        int lsCategory = RoadSync_CategoryForManager(GetWorldThisPtr(), network);
        if (lsCategory >= 0) {
            RoadSyncPlanStartMsg m = {};
            m.category = lsCategory;
            m.exceptGroup = exceptGroup ? 1 : 0;
            m.dollars = dollars ? 1 : 0;
            m.rubles = rubles ? 1 : 0;
            m.count = (int)count;
            m.group = (int)group;
            m.mode = BuildMode_Read();
            Commands_Submit(LS_CMD_PLAN_START, &m, sizeof(m));
            return 0;
        }
    }
    bool capture = g_connected && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
                   (exceptGroup ? IsUiCall(ret, kPlanStartExceptUiReturns, sizeof(kPlanStartExceptUiReturns) / sizeof(kPlanStartExceptUiReturns[0]))
                                : IsUiCall(ret, kPlanStartUiReturns, sizeof(kPlanStartUiReturns) / sizeof(kPlanStartUiReturns[0])));
    int category = capture ? RoadSync_CategoryForManager(GetWorldThisPtr(), network) : -1;
    int before = category >= 0 ? CountStartablePlans(network, exceptGroup, (int)count, (int)group) : 0;
    __int64 result = orig(network, dollars, rubles, count, group);
    if (category < 0 || before == 0) return result;
    int started = before - CountStartablePlans(network, exceptGroup, (int)count, (int)group);
    EnterCriticalSection(&g_queueLock);
    if (g_planCaptureCount < PLAN_CAPTURE_MAX) {
        RoadSyncPlanStartMsg& m = g_planCaptures[g_planCaptureCount++];
        memset(&m, 0, sizeof(m));
        m.category = category;
        m.exceptGroup = exceptGroup ? 1 : 0;
        m.dollars = dollars ? 1 : 0;
        m.rubles = rubles ? 1 : 0;
        m.count = (int)count;
        m.group = (int)group;
        m.started = started;
        m.mode = BuildMode_Read();
    }
    LeaveCriticalSection(&g_queueLock);
    return result;
}

static __int64 HookedPlanStart(__int64 network, unsigned char dollars, unsigned char rubles, __int64 count, __int64 group)
{
    return PlanStartCommon(false, _ReturnAddress(), network, dollars, rubles, count, group);
}

static __int64 HookedPlanStartExcept(__int64 network, unsigned char dollars, unsigned char rubles, __int64 count, __int64 group)
{
    return PlanStartCommon(true, _ReturnAddress(), network, dollars, rubles, count, group);
}

typedef __int64 (*CancelSegFn)(__int64 network, __int64 seg, unsigned char p3, int p4, unsigned char p5);
#define CANCEL_SEG_RVA 0x5434C0
#define BOTTOM_BAR_CANCEL_CALL_RVA 0x39F497

static ULONGLONG g_lastBottomBarCancelMs = 0;
static __int64 g_lastBottomBarCancelNet = 0;

static __int64 HookedBottomBarCancelSeg(__int64 network, __int64 seg, unsigned char p3, int p4, unsigned char p5)
{
    if (SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning() && !IsDualTestbed()) {
        typedef __int64 (*CancelSegFn)(__int64, __int64, unsigned char, int, unsigned char);
        return ((CancelSegFn)(g_exeBase + CANCEL_SEG_RVA))(network, seg, p3, p4, p5);
    }
    if (InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
        (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded()))) {
        SaveSync_NotifyWaitingLobbyActionBlocked();
        return 0;
    }
    if (Commands_TakeLocal() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0) {
        ULONGLONG now = GetTickCount64();
        if (g_lastBottomBarCancelNet != network || now - g_lastBottomBarCancelMs > 50) {
            g_lastBottomBarCancelNet = network;
            g_lastBottomBarCancelMs = now;
            int lsCategory = RoadSync_CategoryForManager(GetWorldThisPtr(), network);
            if (lsCategory >= 0) {
                RoadSyncPlanStartMsg m = {};
                m.category = lsCategory;
                m.exceptGroup = 2;
                m.mode = BuildMode_Read();
                Commands_Submit(LS_CMD_PLAN_START, &m, sizeof(m));
            }
        }
        return 0;
    }
    return ((CancelSegFn)(g_exeBase + CANCEL_SEG_RVA))(network, seg, p3, p4, p5);
}

bool RoadSync_InstallPlanStartHooks()
{
    bool a = InstallInlineHook(g_exeBase + PLAN_START_RVA, (void*)HookedPlanStart, (void**)&g_origPlanStart,
                               kPlanStartExpect, sizeof(kPlanStartExpect), "republicmp plan start");
    bool b = InstallInlineHook(g_exeBase + PLAN_START_EXCEPT_RVA, (void*)HookedPlanStartExcept,
                               (void**)&g_origPlanStartExcept, kPlanStartExceptExpect, sizeof(kPlanStartExceptExpect),
                               "republicmp plan start except");
    BYTE* callAt = g_exeBase + BOTTOM_BAR_CANCEL_CALL_RVA;
    int rel = 0;
    memcpy(&rel, callAt + 1, 4);
    if (callAt[0] == 0xE8 && BOTTOM_BAR_CANCEL_CALL_RVA + 5 + rel == CANCEL_SEG_RVA) {
        BYTE* relay = AllocNear(callAt, 64);
        if (relay) {
            void* target = (void*)&HookedBottomBarCancelSeg;
            DWORD old;
            VirtualProtect(relay, 16, PAGE_EXECUTE_READWRITE, &old);
            relay[0] = 0xFF; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
            memcpy(relay + 6, &target, 8);
            VirtualProtect(relay, 16, PAGE_EXECUTE_READ, &old);
            FlushInstructionCache(GetCurrentProcess(), relay, 16);
            int relToRelay = (int)((long long)relay - (long long)(callAt + 5));
            unsigned char patch[5] = { 0xE8, 0, 0, 0, 0 };
            memcpy(patch + 1, &relToRelay, 4);
            VirtualProtect(callAt, 5, PAGE_EXECUTE_READWRITE, &old);
            memcpy(callAt, patch, 5);
            VirtualProtect(callAt, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), callAt, 5);
        }
    }
    // Patch SECOND_ROAD wrapper (0x3CF6F0) empty-segments guard at RVA 0x3CFA48:
    // When HookedRoadCommit intercepts a commit in Lockstep without pushing a segment,
    // segments_begin == segments_end (0 == 0). The original `je +0x11` (74 11) jumped to
    // 0x3CFA5B (`mov rbp, [rcx-8]`) and read `[0 - 8]` -> AV crash when building from an
    // existing road node. Redirecting to +0x0B lands on `je 0x1403D00C8` (0F 84 6B 06 00 00).
    BYTE* secRoadJe = g_exeBase + 0x3CFA48;
    if (secRoadJe[0] == 0x74 && secRoadJe[1] == 0x11 &&
        secRoadJe[2 + 0x0B] == 0x0F && secRoadJe[2 + 0x0B + 1] == 0x84) {
        DWORD oldProt = 0;
        VirtualProtect(secRoadJe, 2, PAGE_EXECUTE_READWRITE, &oldProt);
        secRoadJe[1] = 0x0B;
        VirtualProtect(secRoadJe, 2, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), secRoadJe, 2);
    }
    return a && b;
}

template <typename MsgT, size_t CapN, typename PackFn>
static void FlushCapturedRoadQueue(MsgT (&queue)[CapN], int& count, int wireTag, PackFn packFn)
{
    MsgT captured[CapN];
    EnterCriticalSection(&g_queueLock);
    int n = count;
    if (n > (int)CapN) n = (int)CapN;
    if (n > 0) memcpy(captured, queue, sizeof(MsgT) * (size_t)n);
    count = 0;
    LeaveCriticalSection(&g_queueLock);
    for (int i = 0; i < n; i++) {
        static RoadSyncItem s_item; // large - game thread only
        memset(&s_item, 0, sizeof(s_item));
        s_item.tag = wireTag;
        packFn(s_item, captured[i]);
        SendOrQueue(s_item);
    }
}

static void SendPlanCaptures()
{
    FlushCapturedRoadQueue(g_planCaptures, g_planCaptureCount, ROAD_SYNC_WIRE_PLAN_START,
                           [](RoadSyncItem& item, const RoadSyncPlanStartMsg& m) { item.plan = m; item.plan.terrainFence = TerrainSync_GetFenceSeq(); });
}

static bool CallPlanStart(PlanStartFn fn, __int64 network, const RoadSyncPlanStartMsg& m)
{
    __try {
        fn(network, m.dollars ? 1 : 0, m.rubles ? 1 : 0, m.count, m.group);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool CallCancelAllPlanned(__int64 network, int category, int* outRemoved)
{
    *outRemoved = 0;
    __try {
        __int64* begin = *(__int64**)(network + MGR_SEGMENTS_BEGIN_OFFSET);
        __int64* end = *(__int64**)(network + MGR_SEGMENTS_END_OFFSET);
        if (begin && end > begin) {
            for (long long i = (end - begin) - 1; i >= 0; --i) {
                __int64 seg = begin[i];
                if (!seg || *(unsigned char*)(seg + EDGE_PLANNED_OFFSET) == 0) continue;
                ((CancelSegFn)(g_exeBase + CANCEL_SEG_RVA))(network, seg, 0, -1, 0);
                (*outRemoved)++;
                begin = *(__int64**)(network + MGR_SEGMENTS_BEGIN_OFFSET);
                end = *(__int64**)(network + MGR_SEGMENTS_END_OFFSET);
                if (!begin || i > end - begin) i = end - begin;
            }
        }
        ((void(*)(__int64))(g_exeBase + 0x4FAE50))(network);
        if (category == ROAD_SYNC_CATEGORY_RAIL) {
            ((void(*)(__int64))(g_exeBase + 0x592990))(GetWorldThisPtr() + 0xF908);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ApplyPlanStartAndAck(const RoadSyncPlanStartMsg& m)
{
    const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(m.category);
    __int64 network = 0;
    __try {
        network = info ? *(__int64*)(GetWorldThisPtr() + info->managerOffset) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        network = 0;
    }
    if (m.exceptGroup == 2) {
        if (!network) {
            SendAck(m.seq, m.category, false, 1);
            return;
        }
        int removed = 0;
        InterlockedIncrement(&g_applyingSegment);
        TerrainSync_BeginForeignReplay();
        bool ok = CallCancelAllPlanned(network, m.category, &removed);
        TerrainSync_EndForeignReplay();
        InterlockedDecrement(&g_applyingSegment);
        Logf("republicmp  ROAD_SYNC: cancel all planned %s - %s: %d plan(s) removed",
             CategoryName(m.category), ok ? "applied" : "THREW", removed);
        SendAck(m.seq, m.category, ok, 1);
        return;
    }
    PlanStartFn fn = m.exceptGroup ? g_origPlanStartExcept : g_origPlanStart;
    if (!network || !fn) {
        Logf("republicmp  ROAD_SYNC: plan start seq=%d %s - no network here, skipped", m.seq, CategoryName(m.category));
        SendAck(m.seq, m.category, false, 1);
        return;
    }
    bool except = m.exceptGroup != 0;
    int before = CountStartablePlans(network, except, m.count, m.group);
    // The new sites take the sender's build mode, not this PC's: with
    // "rubles" here a realistic plan from the other PC finished at once.
    BuildMode mine = BuildMode_Read();
    BuildMode_Write(m.mode);
    InterlockedIncrement(&g_applyingSegment);
    TerrainSync_BeginForeignReplay();
    bool ok = CallPlanStart(fn, network, m);
    TerrainSync_EndForeignReplay();
    InterlockedDecrement(&g_applyingSegment);
    BuildMode_Write(mine);
    int started = before - CountStartablePlans(network, except, m.count, m.group);
    char theirs[96];
    Logf("republicmp  ROAD_SYNC: plan start seq=%d %s dollars=%d rubles=%d pay=%s - %s: %d plan(s) started here, %d on the sender%s",
         m.seq, CategoryName(m.category), m.dollars, m.rubles, BuildMode_Describe(m.mode, theirs, sizeof(theirs)),
         ok ? "applied" : "THREW", started, m.started, ok && started != m.started ? " (DIFFERENT)" : "");
    SendAck(m.seq, m.category, ok && started > 0, 1);
}

// ---------------------------------------------------------------------------
// Group construction tools (see RoadSyncGroupToolMsg)
// ---------------------------------------------------------------------------

#define GROUP_TOOL_RVA 0x21C110
#define CTRL_RECT_DEMOLISH_RVA 0x213870
#define CTRL_RECT_UPGRADE_RVA 0x214530
#define SHIFT_CANCEL_PLAN_RVA 0x214220
#define GROUP_TOOL_YAW_RVA 0x9E9D20   // float: camera yaw that turns the box
#define GROUP_TOOL_DOWN_RVA 0xA54E90  // byte: mouse button held
#define GROUP_TOOL_PRESS_RVA 0xA54E91 // byte: mouse button pressed this frame
#define WORLD_GROUP_BOX_OFFSET 0x13f5c
#define WORLD_GROUP_DRAG_OFFSET 0x14033
#define WORLD_CURSOR_OFFSET 0xf6c
#define WORLD_NETWORK_COUNT_OFFSET 0x14034 // int: entries below
#define WORLD_NETWORK_ENTRIES_OFFSET 0x14038 // entries of 0x50, the network at +0
#define WORLD_NETWORK_ENTRY_SIZE 0x50
#define GROUP_CAPTURE_MAX 8

// mov rax,rsp / push rbp,rsi,rdi,r12,r13,r14,r15 / lea rbp,[rsp-70h] - 19 bytes
static const unsigned char kGroupToolExpect[19] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0x90
};

typedef __int64 (*GroupToolFn)(__int64 world, int mode);
static GroupToolFn g_origGroupTool = nullptr;

static RoadSyncGroupToolMsg g_groupCaptures[GROUP_CAPTURE_MAX];
static int g_groupCaptureCount = 0;

// What the commit reads besides the menus (replay_menus.h closes those).
struct GroupToolState {
    float box[6];
    float cursor[3];
    float yaw;
    unsigned char drag, down, press;
};

static void ReadGroupToolState(__int64 world, GroupToolState& s)
{
    BYTE* exe = g_exeBase;
    memcpy(s.box, (void*)(world + WORLD_GROUP_BOX_OFFSET), sizeof(s.box));
    memcpy(s.cursor, (void*)(world + WORLD_CURSOR_OFFSET), sizeof(s.cursor));
    s.yaw = *(float*)(exe + GROUP_TOOL_YAW_RVA);
    s.drag = *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET);
    s.down = exe[GROUP_TOOL_DOWN_RVA];
    s.press = exe[GROUP_TOOL_PRESS_RVA];
}

static void WriteGroupToolState(__int64 world, const GroupToolState& s)
{
    BYTE* exe = g_exeBase;
    memcpy((void*)(world + WORLD_GROUP_BOX_OFFSET), s.box, sizeof(s.box));
    memcpy((void*)(world + WORLD_CURSOR_OFFSET), s.cursor, sizeof(s.cursor));
    *(float*)(exe + GROUP_TOOL_YAW_RVA) = s.yaw;
    *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = s.drag;
    exe[GROUP_TOOL_DOWN_RVA] = s.down;
    exe[GROUP_TOOL_PRESS_RVA] = s.press;
}

// Planned pieces (edge +0x140 = 1) over every network the tool walks.
static int CountPlannedPieces(__int64 world)
{
    int n = 0;
    __try {
        int networks = *(int*)(world + WORLD_NETWORK_COUNT_OFFSET);
        for (int k = 0; k < networks && k < 256; k++) {
            __int64 network = *(__int64*)(world + WORLD_NETWORK_ENTRIES_OFFSET + (__int64)k * WORLD_NETWORK_ENTRY_SIZE);
            if (!network) continue;
            __int64* begin = *(__int64**)(network + MGR_SEGMENTS_BEGIN_OFFSET);
            __int64* end = *(__int64**)(network + MGR_SEGMENTS_END_OFFSET);
            if (!begin || end < begin) continue;
            for (__int64* p = begin; p < end; p++)
                if (*p && *(unsigned char*)(*p + EDGE_PLANNED_OFFSET) == 1) n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

static __int64 HookedGroupTool(__int64 world, int mode)
{
    if (SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning() && !IsDualTestbed())
        return g_origGroupTool(world, mode);

    if (InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
        (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded()))) {
        __try { *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    // Lockstep: the commit frame becomes a command; this PC drops its drag
    // mark instead of committing, so the next frame does not commit again.
    if (Commands_TakeLocal() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0) {
        GroupToolState s;
        bool commit = false;
        __try {
            BYTE* w = (BYTE*)world;
            ReadGroupToolState(world, s);
            commit = !w[0xfbc] && !w[0xfbe] && !w[0xfc3] && !s.down && s.drag;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            commit = false;
        }
        if (!commit) return g_origGroupTool(world, mode);
        const char* grpBuildDesc = Tr("групповое строительство", "group construction");
        if (!TwoStates_CheckBuildAllowed(s.box[0], s.box[2], grpBuildDesc) ||
            !TwoStates_CheckBuildAllowed(s.box[3], s.box[5], grpBuildDesc)) {
            *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = 0;
            return 0;
        }
        RoadSyncGroupToolMsg m = {};
        m.category = ROAD_SYNC_GROUP_TOOL_CATEGORY;
        m.mode = mode;
        memcpy(m.box, s.box, sizeof(m.box));
        memcpy(m.cursor, s.cursor, sizeof(m.cursor));
        m.yaw = s.yaw;
        m.buildMode = BuildMode_Read();
        Commands_Submit(LS_CMD_GROUP_TOOL, &m, sizeof(m));
        *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = 0;
        return 0;
    }
    if (!g_connected || InterlockedCompareExchange(&g_applyingSegment, 0, 0) != 0)
        return g_origGroupTool(world, mode);

    // The commit frame as the game decides it (menus closed, button up, a
    // drag marked); every other frame only tracks the drag.
    GroupToolState s;
    bool commit = false;
    __try {
        BYTE* w = (BYTE*)world;
        ReadGroupToolState(world, s);
        commit = !w[0xfbc] && !w[0xfbe] && !w[0xfc3] && !s.down && s.drag;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        commit = false;
    }
    if (!commit) return g_origGroupTool(world, mode);

    int before = CountPlannedPieces(world);
    __int64 result = g_origGroupTool(world, mode);
    int started = before - CountPlannedPieces(world);
    EnterCriticalSection(&g_queueLock);
    if (g_groupCaptureCount < GROUP_CAPTURE_MAX) {
        RoadSyncGroupToolMsg& m = g_groupCaptures[g_groupCaptureCount++];
        memset(&m, 0, sizeof(m));
        m.category = ROAD_SYNC_GROUP_TOOL_CATEGORY;
        m.mode = mode;
        memcpy(m.box, s.box, sizeof(m.box));
        memcpy(m.cursor, s.cursor, sizeof(m.cursor));
        m.yaw = s.yaw;
        m.buildMode = BuildMode_Read();
        m.started = started;
    }
    LeaveCriticalSection(&g_queueLock);
    return result;
}

static void HandleCtrlRectTool(__int64 world, int mode, unsigned int rva)
{
    typedef void (*RectToolFn)(__int64);
    RectToolFn orig = (RectToolFn)(g_exeBase + rva);
    if (SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning() && !IsDualTestbed()) {
        orig(world);
        return;
    }
    if (InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
        (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded()))) {
        __try { *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    if (Commands_TakeLocal() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0) {
        GroupToolState s;
        bool commit = false;
        __try {
            BYTE* w = (BYTE*)world;
            ReadGroupToolState(world, s);
            commit = !w[0xfbc] && !w[0xfbe] && !w[0xfc3] && !s.down && s.drag;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            commit = false;
        }
        if (!commit) {
            orig(world);
            return;
        }
        const char* grpDemDesc = Tr("групповой снос", "group demolition");
        const char* grpUpgDesc = Tr("групповая модернизация", "group upgrade");
        bool allowed = mode == 10
            ? (TwoStates_CheckDemolishAllowed(s.box[0], s.box[2], grpDemDesc) &&
               TwoStates_CheckDemolishAllowed(s.box[3], s.box[5], grpDemDesc))
            : (TwoStates_CheckBuildAllowed(s.box[0], s.box[2], grpUpgDesc) &&
               TwoStates_CheckBuildAllowed(s.box[3], s.box[5], grpUpgDesc));
        if (!allowed) {
            *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = 0;
            return;
        }
        RoadSyncGroupToolMsg m = {};
        m.category = ROAD_SYNC_GROUP_TOOL_CATEGORY;
        m.mode = mode;
        memcpy(m.box, s.box, sizeof(m.box));
        memcpy(m.cursor, s.cursor, sizeof(m.cursor));
        m.yaw = s.yaw;
        m.buildMode = BuildMode_Read();
        __try {
            const char* tool = *(const char**)(world + 0xD428);
            if (tool) strncpy_s(m.toolName, sizeof(m.toolName), tool, _TRUNCATE);
            for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; c++) {
                __int64 mgr = *(__int64*)(world + g_categories[c].managerOffset);
                if (mgr) {
                    m.managerTypes[c] = *(int*)(mgr + MGR_TYPE_OFFSET);
                    m.managerVariants[c] = *(int*)(mgr + MGR_VARIANT_OFFSET);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        Commands_Submit(LS_CMD_GROUP_TOOL, &m, sizeof(m));
        *(unsigned char*)(world + WORLD_GROUP_DRAG_OFFSET) = 0;
        return;
    }
    orig(world);
}

void RoadSync_CtrlRectDemolishTool(__int64 world)
{
    HandleCtrlRectTool(world, 10, CTRL_RECT_DEMOLISH_RVA);
}

void RoadSync_CtrlRectUpgradeTool(__int64 world)
{
    HandleCtrlRectTool(world, 11, CTRL_RECT_UPGRADE_RVA);
}

void RoadSync_ShiftCancelTool(__int64 world)
{
    typedef void (*ShiftCancelFn)(__int64);
    ShiftCancelFn orig = (ShiftCancelFn)(g_exeBase + SHIFT_CANCEL_PLAN_RVA);
    if (SaveSync_GetState() == SESSION_IDLE && !Determinism_SessionRunning() && !IsDualTestbed()) {
        orig(world);
        return;
    }
    if (InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0 &&
        (SaveSync_IsWaitingLobbyActive() || (NativeMenu_IsHostOnNextLoad() && SaveSync_GetState() != SESSION_IDLE && SaveSync_IsWorldLoaded()))) {
        return;
    }
    if (Commands_TakeLocal() && InterlockedCompareExchange(&g_applyingSegment, 0, 0) == 0) {
        BYTE* w = (BYTE*)world;
        BYTE* press = g_exeBase + GROUP_TOOL_PRESS_RVA;
        bool clicked = false;
        __try {
            clicked = !w[0xfbc] && !w[0xfbe] && !w[0xfc3] && *press != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            clicked = false;
        }
        if (clicked) {
            float cx = *(float*)(world + WORLD_CURSOR_OFFSET);
            float cz = *(float*)(world + WORLD_CURSOR_OFFSET + 8);
            if (TwoStates_CheckDemolishAllowed(cx, cz, Tr("отмена планируемого участка", "cancelling planned segment"))) {
                RoadSyncGroupToolMsg m = {};
                m.category = ROAD_SYNC_GROUP_TOOL_CATEGORY;
                m.mode = 12;
                __try {
                    memcpy(m.box, (const void*)(world + 0xF60), 12);
                    memcpy(m.box + 3, (const void*)(g_exeBase + 0x9E9D10), 12);
                    memcpy(m.cursor, (const void*)(world + WORLD_CURSOR_OFFSET), 12);
                    m.buildMode = BuildMode_Read();
                    const char* tool = *(const char**)(world + 0xD428);
                    if (tool) strncpy_s(m.toolName, sizeof(m.toolName), tool, _TRUNCATE);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                Commands_Submit(LS_CMD_GROUP_TOOL, &m, sizeof(m));
            }
            BYTE savedPress = *press;
            *press = 0;
            orig(world);
            *press = savedPress;
            return;
        }
    }
    orig(world);
}

bool RoadSync_InstallGroupToolHook()
{
    return InstallInlineHook(g_exeBase + GROUP_TOOL_RVA, (void*)HookedGroupTool, (void**)&g_origGroupTool,
                             kGroupToolExpect, sizeof(kGroupToolExpect), "republicmp group tool");
}

static void SendGroupToolCaptures()
{
    FlushCapturedRoadQueue(g_groupCaptures, g_groupCaptureCount, ROAD_SYNC_WIRE_GROUP_TOOL,
                           [](RoadSyncItem& item, const RoadSyncGroupToolMsg& m) { item.group = m; item.group.terrainFence = TerrainSync_GetFenceSeq(); });
}

static bool CallGroupTool(__int64 world, const RoadSyncGroupToolMsg& m)
{
    __try {
        if (m.mode >= 0 && m.mode <= 3) {
            g_origGroupTool(world, m.mode);
            return true;
        }
        static char s_fakeToolRecord[0x400];
        memset(s_fakeToolRecord, 0, sizeof(s_fakeToolRecord));
        strncpy_s(s_fakeToolRecord, sizeof(s_fakeToolRecord), m.toolName, _TRUNCATE);
        void* savedTool = *(void**)(world + 0xD428);
        *(void**)(world + 0xD428) = s_fakeToolRecord;
        if (m.mode == 10) {
            ((void(*)(__int64))(g_exeBase + CTRL_RECT_DEMOLISH_RVA))(world);
        } else if (m.mode == 11) {
            int savedTypes[ROAD_SYNC_CATEGORY_COUNT] = {};
            int savedVars[ROAD_SYNC_CATEGORY_COUNT] = {};
            for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; c++) {
                __int64 mgr = *(__int64*)(world + g_categories[c].managerOffset);
                if (mgr) {
                    savedTypes[c] = *(int*)(mgr + MGR_TYPE_OFFSET);
                    savedVars[c] = *(int*)(mgr + MGR_VARIANT_OFFSET);
                    *(int*)(mgr + MGR_TYPE_OFFSET) = m.managerTypes[c];
                    *(int*)(mgr + MGR_VARIANT_OFFSET) = m.managerVariants[c];
                }
            }
            ((void(*)(__int64))(g_exeBase + CTRL_RECT_UPGRADE_RVA))(world);
            for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT; c++) {
                __int64 mgr = *(__int64*)(world + g_categories[c].managerOffset);
                if (mgr) {
                    *(int*)(mgr + MGR_TYPE_OFFSET) = savedTypes[c];
                    *(int*)(mgr + MGR_VARIANT_OFFSET) = savedVars[c];
                }
            }
        } else if (m.mode == 12) {
            float savedRay[3], savedEye[3];
            memcpy(savedRay, (const void*)(world + 0xF60), 12);
            memcpy(savedEye, (const void*)(g_exeBase + 0x9E9D10), 12);
            memcpy((void*)(world + 0xF60), m.box, 12);
            memcpy((void*)(g_exeBase + 0x9E9D10), m.box + 3, 12);
            g_exeBase[GROUP_TOOL_PRESS_RVA] = 1;
            ((void(*)(__int64))(g_exeBase + SHIFT_CANCEL_PLAN_RVA))(world);
            memcpy((void*)(world + 0xF60), savedRay, 12);
            memcpy((void*)(g_exeBase + 0x9E9D10), savedEye, 12);
        }
        *(void**)(world + 0xD428) = savedTool;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The menus are already closed by the caller's ReplayUiScope.
static void ApplyGroupToolAndAck(const RoadSyncGroupToolMsg& m)
{
    __int64 world = GetWorldThisPtr();
    bool validMode = (m.mode >= 0 && m.mode <= 3) || (m.mode >= 10 && m.mode <= 12);
    if (!g_origGroupTool || !validMode) {
        Logf("republicmp  ROAD_SYNC: group tool seq=%d mode %d - not applied", m.seq, m.mode);
        SendAck(m.seq, m.category, false, 1);
        return;
    }
    // All of it lives in the exe image (the world object is a global).
    GroupToolState own, replay;
    int before = CountPlannedPieces(world);
    BuildMode mine = BuildMode_Read();
    ReadGroupToolState(world, own);
    memcpy(replay.box, m.box, sizeof(replay.box));
    memcpy(replay.cursor, m.cursor, sizeof(replay.cursor));
    replay.yaw = m.yaw;
    replay.drag = 1;
    replay.down = 0;
    replay.press = 0;
    WriteGroupToolState(world, replay);
    BuildMode_Write(m.buildMode);
    InterlockedIncrement(&g_applyingSegment);
    TerrainSync_BeginForeignReplay();
    bool ok = CallGroupTool(world, m);
    TerrainSync_EndForeignReplay();
    InterlockedDecrement(&g_applyingSegment);
    WriteGroupToolState(world, own); // this PC's own drag, cursor, camera and input
    BuildMode_Write(mine);
    int started = before - CountPlannedPieces(world);
    char theirs[96];
    Logf("republicmp  ROAD_SYNC: group tool seq=%d mode %d pay=%s - %s: %d planned piece(s) started here, %d on the sender%s",
         m.seq, m.mode, BuildMode_Describe(m.buildMode, theirs, sizeof(theirs)), ok ? "applied" : "THREW",
         started, m.started, ok && started != m.started ? " (DIFFERENT)" : "");
    SendAck(m.seq, m.category, ok, 1);
}

bool RoadSync_ApplyCommandPlanStart(const RoadSyncPlanStartMsg& msg)
{
    ApplyPlanStartAndAck(msg); // the ack goes nowhere without the replay channel
    return true;
}

bool RoadSync_ApplyCommandGroupTool(const RoadSyncGroupToolMsg& msg)
{
    ReplayUiScope menus;
    ApplyGroupToolAndAck(msg);
    return true;
}

static std::unordered_map<__int64, int> g_lsDirections;

void RoadSync_LockstepDirectionsBaseline()
{
    g_lsDirections.clear();
    int n = ReadFactoryDirections();
    for (int i = 0; i < n; i++) {
        g_lsDirections[g_dirSnapshot[i].seg] = g_dirSnapshot[i].direction;
    }
}

void RoadSync_LockstepDirectionsDetect()
{
    int n = ReadFactoryDirections();
    for (int i = 0; i < n; i++) {
        const DirSnapshot& s = g_dirSnapshot[i];
        auto old = g_lsDirections.find(s.seg);
        if (old == g_lsDirections.end() || old->second == s.direction) continue;
        RoadSyncDirectionMsg m = {};
        m.category = ROAD_SYNC_CATEGORY_FACTORYCONN;
        m.piece = s.piece;
        m.direction = s.direction;
        Commands_Submit(LS_CMD_DIRECTION, &m, sizeof(m));
        RoadSyncDirectionMsg back = m; // this PC's own segment, as it was
        back.direction = old->second;
        int local = 0;
        ApplyDirection(back, &local);
    }
}

bool RoadSync_ApplyCommandDirection(const RoadSyncDirectionMsg& msg)
{
    int local = 0;
    bool ok = ApplyDirection(msg, &local) != 0;
    Logf("republicmp  ROAD_SYNC: command direction %d %s", msg.direction, ok ? "applied" : "NOT applied - connection not found");
    return ok;
}

void RoadSync_Tick()
{
    FlushOutboundQueue();
    SendPlanCaptures();
    SendGroupToolCaptures();

    // Factory connection directions: only while connected and in the world
    // (the join screen and a fresh load would otherwise look like changes).
    if (g_connected && !SyncBlock_IsActive() && IsWorldLoaded()) {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - g_lastDirScanMs >= DIR_SCAN_INTERVAL_MS) {
            g_lastDirScanMs = nowMs;
            ScanFactoryDirections();
        }
    } else {
        g_knownDirections.clear();
    }

    std::vector<RoadSyncAckMsg> acks;
    EnterCriticalSection(&g_queueLock);
    acks.swap(g_incomingAcks);
    if (!g_incoming.empty()) {
        g_pending.insert(g_pending.end(), g_incoming.begin(), g_incoming.end());
        g_incoming.clear();
    }
    LeaveCriticalSection(&g_queueLock);

    for (const RoadSyncAckMsg& ack : acks) {
        bool found = false;
        for (size_t i = 0; i < g_pendingSends.size(); i++) {
            if (g_pendingSends[i] == ack.seq) {
                g_pendingSends.erase(g_pendingSends.begin() + i);
                found = true;
                break;
            }
        }
        Logf("republicmp  ROAD_SYNC: ack seq=%d %s %s%s", ack.seq, CategoryName(ack.category),
             ack.success ? "CONFIRMED built" : "FAILED on the other side",
             found ? "" : " (no matching pending send)");
    }

    if (g_pending.empty() && g_waitingDemolish.empty()) return;

    if (!IsWorldLoaded()) {
        if (!g_warnedWorldNotLoaded) {
            Logf("republicmp  ROAD_SYNC: %zu remote item(s) held until the world is loaded", g_pending.size());
            g_warnedWorldNotLoaded = true;
        }
        return;
    }
    g_warnedWorldNotLoaded = false;

    // Never replay into a manager while the local player is dragging, or
    // while the tool wrappers are blocked by UI - items stay queued.
    __int64 thisPtr = GetWorldThisPtr();
    ULONGLONG nowMs = GetTickCount64();
    // A window or menu open on this PC no longer holds the other player's
    // roads (replay_menus.h); the join screen still does.
    if (SyncBlock_IsActive()) return;
    ReplayUiScope replayUi;
    for (size_t i = 0; i < g_waitingDemolish.size();) {
        RoadSyncItem& item = g_waitingDemolish[i];
        if (nowMs < item.notBeforeMs || LocalBuilds_IsToolDragActive() || ToolWrapperGuardBusy(thisPtr)) {
            ++i;
            continue;
        }
        bool over = nowMs - item.waitSinceMs >= DEMOLISH_OWNER_WAIT_MS;
        if (ApplyDemolishAndAck(item.dem, true, over) == DEMOLISH_WAIT_OWNER) {
            item.notBeforeMs = nowMs + DEMOLISH_OWNER_POLL_MS;
            ++i;
        } else {
            g_waitingDemolish.erase(g_waitingDemolish.begin() + i);
        }
    }

    std::vector<RoadSyncItem> toApply;
    toApply.swap(g_pending);
    for (size_t i = 0; i < toApply.size(); i++) {
        if (LocalBuilds_IsToolDragActive() || ToolWrapperGuardBusy(thisPtr) || !TerrainFenceAllows(toApply[i]) ||
            nowMs < toApply[i].notBeforeMs) {
            g_pending.insert(g_pending.begin(), toApply.begin() + i, toApply.end());
            break;
        }
        if (toApply[i].tag == ROAD_SYNC_WIRE_PLAN_START)
            ApplyPlanStartAndAck(toApply[i].plan);
        else if (toApply[i].tag == ROAD_SYNC_WIRE_GROUP_TOOL)
            ApplyGroupToolAndAck(toApply[i].group);
        else if (toApply[i].tag == ROAD_SYNC_WIRE_DIRECTION)
            ApplyDirectionAndAck(toApply[i].dir);
        else if (toApply[i].tag == ROAD_SYNC_WIRE_UPGRADE)
            ApplyUpgradeAndAck(toApply[i].up);
        else if (toApply[i].tag == ROAD_SYNC_WIRE_DEMOLISH) {
            RoadSyncItem& item = toApply[i];
            DemolishOutcome r = ApplyDemolishAndAck(item.dem, item.attempts + 1 >= DEMOLISH_MAX_ATTEMPTS, false);
            if (r == DEMOLISH_WAIT_OWNER) {
                item.waitSinceMs = nowMs;
                item.notBeforeMs = nowMs + DEMOLISH_OWNER_POLL_MS;
                g_waitingDemolish.push_back(item);
            } else if (r == DEMOLISH_RETRY) {
                // Again after the game's next frames; later items wait behind it.
                item.attempts++;
                item.notBeforeMs = nowMs + DEMOLISH_RETRY_MS;
                g_pending.insert(g_pending.begin(), toApply.begin() + i, toApply.end());
                break;
            }
        } else
            ApplySegmentAndAck(toApply[i].seg);
    }
}
