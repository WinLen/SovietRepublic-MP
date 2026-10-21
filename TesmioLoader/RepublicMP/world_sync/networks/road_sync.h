// SPDX-License-Identifier: GPL-3.0-only
// Road sync: network replication of every drag-built infrastructure tool
// (roads, pipes, rails, footpaths, fences, power lines, conveyors, ...).
//
// How the game builds these
// -------------------------
// Every drag tool is a per-frame wrapper (FUN_1403d00f0 for roads and one
// sibling per tool, dispatched by FUN_14030d1a0 on the active tool name) that
// drives three shared engine primitives on the tool's own "manager" object,
// found at a fixed offset off the placement god-object (thisPtr):
//   FUN_1404fc040  drag start  - picks/creates the start node (manager+0x428)
//   FUN_1404fcaf0  drag tick   - routes to the cursor, fills the route buffer
//   FUN_140518a80  commit      - turns the route into a real segment
//
// A finished segment is: start node -> interior points -> end node. The
// route buffer (manager+0x410/+0x418, 24-byte records: xyz + 3 engine floats)
// holds ONLY the interior points; the ends are the node objects
// manager+0x428 / manager+0x430 (or an existing node the end snapped to,
// manager+0x740 when manager+0x749 == 1).
//
// Sending side
// ------------
// world_sync/networks/local_builds.cpp hooks FUN_140518a80's entry and, for a tool manager,
// copies the interior records, both node positions and any building
// connection port at either end (RoadSyncSegment). Commits that happen inside
// a building placement (a building laying its own access road) are skipped -
// the building replay recreates them on the other side.
//
// Receiving side
// --------------
// The segment is replayed through the same primitives: the tool wrapper
// starts a drag on the start node, fc040 is re-run in fuzzy (+-1m) mode so the
// start attaches to the existing node there, fcaf0 is called ONCE with the
// cursor on the end node and the sender's interior records as its param_8
// (bulk route - the engine skips its own routing), and the wrapper releases,
// which commits. The result is bit-identical to the sender's segment.
//
// Upgrades (re-typing an existing segment, e.g. gravel -> panels) take a
// different commit branch (manager+0x390 != 0) and travel as
// RoadSyncUpgradeMsg; see ApplyUpgrade in road_sync.cpp.
//
// Transport: one dedicated TCP connection (ROAD_SYNC_PORT). Every payload is
// preceded by an int32 tag (ROAD_SYNC_WIRE_*). Segments/upgrades are queued
// while disconnected and sent in capture order once a connection is up; each
// one is acknowledged with the receiving engine's verdict.
//
// Both machines must run the same build - the message layout is not versioned.
#pragma once

#include "tesmio_plugin.h"
#include "../../core/host_config.h"
#include "../buildings/build_mode.h"
#include <stddef.h> // offsetof

#define ROAD_SYNC_PORT 5558
#define ROAD_SYNC_TEST_HOST_IP REPUBLICMP_HOST_IP

// Upper bound on interior points per segment (the longest real drags seen
// were ~125 points; 742 was seen on a long road).
#define ROAD_SYNC_MAX_POINTS 2048

// Tool categories. The value travels on the wire, so the order must match on
// both machines. Manager offsets / wrappers: see g_categories in road_sync.cpp.
enum RoadSyncCategory {
    ROAD_SYNC_CATEGORY_ROAD = 0,         // road_*, bridges, tunnels, one-way
    ROAD_SYNC_CATEGORY_PIPE = 1,         // pipe_underground, pipe_steam
    ROAD_SYNC_CATEGORY_WATERPIPE = 2,    // waterpipe_*, sewage
    ROAD_SYNC_CATEGORY_HEATPIPE = 3,     // heating_pipe
    ROAD_SYNC_CATEGORY_RAIL = 4,         // rail_*
    ROAD_SYNC_CATEGORY_PEDESTRIAN = 5,   // pedestrianway_*, pedestrian_tunel
    ROAD_SYNC_CATEGORY_FENCE = 6,        // fence_*
    ROAD_SYNC_CATEGORY_POWER_HIGH = 7,   // eletric_high*, eletric_uhigh*
    ROAD_SYNC_CATEGORY_POWER_LOW = 8,    // eletric_low*, eletric_ulow*
    ROAD_SYNC_CATEGORY_CONVEYOR = 9,     // conveyor
    ROAD_SYNC_CATEGORY_CABLEWAY = 10,    // create_cableway
    ROAD_SYNC_CATEGORY_AIRROAD = 11,     // create_airroad*
    ROAD_SYNC_CATEGORY_FACTORYCONN = 12, // factoryconnection (+ direction, see RoadSyncDirectionMsg)
    ROAD_SYNC_CATEGORY_SECOND_ROAD = 13, // second_road_* (decoration paths and pavements, by SoylentGreed)
    ROAD_SYNC_CATEGORY_COUNT
};

struct RoadSyncCategoryInfo {
    const char* name;
    int managerOffset; // *(thisPtr + managerOffset) = the tool's manager object
    bool enabled;      // disabled categories are neither captured nor applied
};

// nullptr for an out-of-range category.
const RoadSyncCategoryInfo* RoadSync_GetCategoryInfo(int category);

// Category whose manager object is `manager`, or -1 (also for disabled ones).
int RoadSync_CategoryForManager(__int64 thisPtr, __int64 manager);

// Number of segments in a category's network, or -1 if unreadable
// (diagnostics: which networks a building placement adds to).
int RoadSync_SegmentCount(__int64 thisPtr, int category);

// One route record exactly as the engine stores it in manager+0x410
// (24 bytes). `extra` is engine-internal per-point data (orientation and
// running length); it is transmitted verbatim, never interpreted.
struct RoadSyncPoint {
    float pos[3];
    float extra[3];
};

// A building connection port at one end of a segment. Pipes, conveyors and
// power lines can end on a building instead of a node; the engine reads that
// from the "hovered port" globals DAT_1409e45b0 (building) / DAT_1409e45b8
// (port index). Buildings are identified by position (obj+0x350); the port
// index is per building type and therefore identical on both machines.
struct RoadSyncBuildingPort {
    int valid;       // 0 = this end is not on a building port
    int portIndex;
    float bldPos[3];
};

// Where a footpath end joins a road (the footpath tool's hover state, see
// RoadSync_ReadFootpathLinks): a road edge it splits at one of its points,
// and/or a road node it meets. The commit links the two networks' nodes
// there - the pedestrian crossing. Edges and nodes are identified by
// position (an edge by its end nodes and point count).
struct RoadSyncRoadLink {
    int hasEdge;
    int pointIndex;     // the edge's point the road is split at
    float edgeA[3];     // the edge's node 0
    float edgeB[3];     // the edge's node 1
    int edgePoints;     // the edge's point count
    int hasNode;
    float node[3];
};

// Everything needed to rebuild one committed segment.
struct RoadSyncSegment {
    int category;       // RoadSyncCategory
    int typeId;         // manager+0x528
    int variant;        // manager+0x52c
    float curveValue;   // manager+0x48, the tool's curve setting
    float startNode[3]; // start node position
    float endNode[3];   // end node position
    RoadSyncBuildingPort startPort;
    RoadSyncBuildingPort endPort;
    RoadSyncRoadLink startLink; // footpaths only
    RoadSyncRoadLink endLink;
    int pointCount;     // interior points, 0..ROAD_SYNC_MAX_POINTS (power-line spans can have 0-1)
    RoadSyncPoint points[ROAD_SYNC_MAX_POINTS];
};

// Wire message for a segment. Only the first pointCount points are meaningful.
struct RoadSyncMsg {
    int seq; // sender-assigned, echoed by the ack
    int terrainFence; // TerrainSync_GetFenceSeq() at send - applied only after that terrain batch
    BuildMode mode;   // sender's rubles/dollars/realistic choice (build_mode.h), replayed with it
    RoadSyncSegment seg;
};

// Wire form of a segment (also inside RoadSyncMsg and after an access-road
// BuildSyncMsg): everything up to `points`, then only the pointCount points
// actually used - points[] is the last member of both structs. A 5-point
// path is ~200 bytes instead of the whole 2048-point array (~48 KB). The
// receiver reads the header, checks pointCount, then reads exactly that
// many points; a bad pointCount means the stream is out of step and the
// connection is dropped.
#define ROAD_SYNC_SEGMENT_HEADER_BYTES offsetof(RoadSyncSegment, points)
#define ROAD_SYNC_MSG_HEADER_BYTES (offsetof(RoadSyncMsg, seg) + ROAD_SYNC_SEGMENT_HEADER_BYTES)

inline bool RoadSync_PointCountValid(int pointCount)
{
    return pointCount >= 0 && pointCount <= ROAD_SYNC_MAX_POINTS;
}

inline size_t RoadSync_PointBytes(int pointCount)
{
    return RoadSync_PointCountValid(pointCount) ? (size_t)pointCount * sizeof(RoadSyncPoint) : 0;
}

// Upgrade of existing segments (FUN_140518a80's manager+0x390 branch). A
// single-segment drag re-types segment +0x380 over the point range
// (+0x388, +0x38c); a pathfound multi-segment drag re-types whole segments.
// Each piece identifies its segment by node positions.
#define ROAD_SYNC_MAX_UPGRADE_PIECES 32
struct RoadSyncUpgradePiece {
    float segStart[3];  // segment's start node position
    float segEnd[3];    // segment's end node position
    int segPointCount;  // interior point count (identity check)
    int rangeStart;     // -1 = from the start node
    int rangeEnd;       // == segPointCount = to the end node
    int oldType;        // manager+0x394
    int oldVariant;     // manager+0x398
    int flag39c;        // manager+0x39c, one-way direction (0 = none)
};
struct RoadSyncUpgradeMsg {
    int seq;
    int terrainFence; // as RoadSyncMsg::terrainFence
    BuildMode mode;   // as RoadSyncMsg::mode
    int category;
    int newType;    // manager+0x528
    int newVariant; // manager+0x52c
    int flag5c4;    // manager+0x5c4
    int pieceCount;
    RoadSyncUpgradePiece pieces[ROAD_SYNC_MAX_UPGRADE_PIECES];
};

// Demolition with the terrain_demolish tool. The tool, every frame, calls
// fcaf0(manager, &cursor, 0x65, 0, 0...) - the engine finds the piece of
// path under the cursor and arms the commit (manager+0x390) - and, while the
// button is held, commits with FUN_140518a80(manager, 0, 0, 0). The receiver
// repeats that with the sender's cursor, so its own engine finds the segment
// and arms the commit. The piece the sender's engine cut (manager+0x388 ..
// +0x38c on segment +0x380) also travels, as the positions of its two ends;
// if the receiver's engine picked a different range, it is replaced with the
// sender's before the commit (route buffer and end nodes included, as the
// upgrade replay does).
//
// Commits on a hovered node (the cursor on a road end, e.g. right after a cut
// or while the button is held) leave no readable range; for those the sender
// compares the segments before and after the commit and sends each removed
// piece as its own message (cursor inside the piece, range = its ends). The
// receiver arms short pieces, which its cursor pick skips, through the node
// of the piece, as the tool does.
struct RoadSyncDemolishMsg {
    int seq;
    int terrainFence;
    int category;
    float cursor[3];  // thisPtr+0xf6c at the sender's commit
    int hasRange;     // 0 = only the cursor is known
    float rangeA[3];  // start of the cut piece (point rs, or node 0 when rs = -1)
    float rangeB[3];  // end of the cut piece (point re, or node 1 when re = count)
    // Multi-segment demolish (manager+0x3d0 set): whole segments, each
    // demolished on its own at the receiver.
    int pieceCount;
    RoadSyncUpgradePiece pieces[8];
};

// Acknowledgement for a segment or upgrade. success = the receiving engine
// accepted it (+0x315 valid and no error category) or it already existed.
struct RoadSyncAckMsg {
    int seq;
    int category;
    int success;
    int attempts;
};

// Direction of a factory connection. The factoryconnection_oneway tool
// (FUN_1403d6e70 with mode 0x32) commits nothing: a click on an existing
// connection cycles segment+0x1bc (0 = both ways, 1 / 2 = one way, relative
// to the segment's node 0 / node 1) and refreshes the buildings at both ends
// (FUN_140191950). The sender scans its connections for changed directions;
// the receiver finds its segment like an upgrade piece, writes the direction
// (mirrored if its segment runs the other way) and runs the same refresh.
// Sent on this connection, so it never overtakes the segment it belongs to.
struct RoadSyncDirectionMsg {
    int seq;
    int category;
    RoadSyncUpgradePiece piece; // segStart / segEnd / segPointCount only
    int direction;              // segment+0x1bc
};

// "Start planned construction" (#20): drawn without starting, a road or rail
// is a plan (edge +0x140 = 1) on both PCs; the construction menu, the
// construction window and the network tool panel turn the plans into sites
// with FUN_1405258e0(network, dollars, rubles, count, group) - every planned
// edge of the network (count > 0: only among its last `count` edges; group
// >= 0: only edges of that group, +0x124) - or FUN_140525c30, the same over
// every group but `group`. That call travels as it is and the receiver makes
// it on its own network: the same plans become sites there.
struct RoadSyncPlanStartMsg {
    int seq;
    int terrainFence;
    int category;
    int exceptGroup; // 1 = FUN_140525c30
    int dollars;
    int rubles;
    int count;
    int group;
    int started;     // plans the call started on the sender (for the log)
    BuildMode mode;  // sender's build mode: each new site takes it (FUN_1402d9d40), replayed with it
};

// The group construction tools (group_build_rub = 0, group_build_usd = 1,
// group_build_co = 2, group_suspend_const = 3): FUN_14021c110(world, mode),
// called every frame one of them is selected. The first frame with the button
// up (exe+0xa54e90) and a drag marked (world+0x14033) commits: the box
// (world+0x13f5c..+0x13f70) turned by the camera yaw (exe+0x9e9d20) picks the
// planned pieces of every network (full edge lists) and the buildings, and
// starts (or suspends) their construction - no FUN_1405258e0 involved. That
// frame's box, cursor, yaw and build mode travel; the receiver writes them,
// makes the same call and puts its own back.
// (by SoylentGreed: the commit frame and the state it reads)
struct RoadSyncGroupToolMsg {
    int seq;
    int terrainFence;
    int category;    // ROAD_SYNC_GROUP_TOOL_CATEGORY (for the ack log)
    int mode;
    float box[6];    // world+0x13f5c: the moving corner, then the press point
    float cursor[3]; // world+0xf6c
    float yaw;       // exe+0x9e9d20
    BuildMode buildMode;
    int started;     // planned pieces the call started on the sender (for the log)
};

#define ROAD_SYNC_GROUP_TOOL_CATEGORY (-2)

// int32 tag sent before every payload on the connection.
#define ROAD_SYNC_WIRE_SEGMENT 1 // RoadSyncMsg
#define ROAD_SYNC_WIRE_ACK 2     // RoadSyncAckMsg
#define ROAD_SYNC_WIRE_UPGRADE 3 // RoadSyncUpgradeMsg
#define ROAD_SYNC_WIRE_DEMOLISH 4 // RoadSyncDemolishMsg
#define ROAD_SYNC_WIRE_DIRECTION 5 // RoadSyncDirectionMsg
#define ROAD_SYNC_WIRE_PLAN_START 6 // RoadSyncPlanStartMsg
#define ROAD_SYNC_WIRE_GROUP_TOOL 7 // RoadSyncGroupToolMsg

// Longest a received item waits for its terrain fence before it is applied
// anyway (lost/reset terrain stream).
#define ROAD_SYNC_FENCE_TIMEOUT_MS 3000

// Receiving side retries a segment only when the apply itself threw.
#define ROAD_SYNC_MAX_APPLY_ATTEMPTS 3

void RoadSync_Bind(const TsmHost* host);

// Sends (or queues while disconnected) a locally committed segment / upgrade.
// Game thread only. Returns the assigned seq, 0 if queued, -1 if refused.
int RoadSync_SendSegment(const RoadSyncSegment& seg);
int RoadSync_SendUpgrade(const RoadSyncUpgradeMsg& msg);
int RoadSync_SendDemolish(int category, const float cursor[3], const float* rangeA, const float* rangeB,
                          const RoadSyncUpgradePiece* pieces = nullptr, int pieceCount = 0);

// True if this exact segment (same end nodes and middle point) exists here.
bool RoadSync_SegmentExists(const RoadSyncSegment& seg);

// Footpath commit (inside the commit hook, before the original runs): reads
// where its ends join roads - the start from the footpath manager
// (+0x360 edge, +0x368 point, +0x370 node), the end from the hover globals
// (0x9E4590 / 0x9E4598 / 0x9E45A0). Clears both links for other networks.
void RoadSync_ReadFootpathLinks(__int64 manager, int category, RoadSyncSegment& seg);

// Clears the hover globals the local mouse leaves (the node, edge and road
// under the cursor, the hovered building port): the road planner reads them
// when it lays a path, a building's access road included. Not restored - the
// game recomputes them on the next frame.
void RoadSync_ClearHover();

// Hooks FUN_1405258e0 / FUN_140525c30 (see RoadSyncPlanStartMsg). Call once
// from TsmPluginStart.
bool RoadSync_InstallPlanStartHooks();

// Hooks FUN_14021c110 (see RoadSyncGroupToolMsg). Call once from
// TsmPluginStart.
bool RoadSync_InstallGroupToolHook();

// Builds a path that a building laid as its own access road (sent on the
// build_sync connection right after the building). Returns true if the
// engine accepted it or an identical one already exists. Game thread only.
bool RoadSync_ApplySegmentForBuild(const RoadSyncSegment& seg);

// Per-frame (game thread, Present hook): logs incoming acks and applies
// received segments/upgrades once the world is loaded and the local player
// is not in the middle of a drag.
void RoadSync_Tick();

// Connection control (UI / save-sync flow).
void RoadSync_StartHost();
void RoadSync_StartClient(const char* hostIp = nullptr); // nullptr = keep last IP
void RoadSync_Reset();

bool RoadSync_IsConnected();
bool RoadSync_IsConnecting();
bool RoadSync_IsHostRole();
const char* RoadSync_GetTargetIp();

// True while this module is replaying a remote segment/upgrade, so the
// commit hook does not echo it back.
bool RoadSync_IsApplyingSegment();

// Lockstep commands (lockstep/commands): the replays above, made on every PC
// at a tick boundary, the PC that drew it included. One try each - a retry
// on a later frame would land at another point of the simulation. Main
// thread. True when the engine made it.
bool RoadSync_ApplyCommandSegment(const RoadSyncMsg& msg);
// Clears a category manager's finished drag/preview state right after a local
// commit is intercepted as a lockstep command, so Late and subsequent frames
// before the command echoes back do not read stale preview state.
void RoadSync_ClearCategoryPreview(int category);
// Ends a drag the local player has under way (every tool manager's preview
// cleared, the drag flag down) before a command uses the tools.
void RoadSync_CancelLocalDrag();
// Start planned construction (the interface's buttons) and the group tools'
// commit frame become commands while commands are captured; made through the
// replays above.
bool RoadSync_ApplyCommandPlanStart(const RoadSyncPlanStartMsg& msg);
bool RoadSync_ApplyCommandGroupTool(const RoadSyncGroupToolMsg& msg);
// Factory connection directions (factoryconnection_oneway cycles one inline
// in the Late body): Baseline records them, Detect puts a changed one back
// and hands it over as a command; ApplyCommandDirection makes it.
void RoadSync_LockstepDirectionsBaseline();
void RoadSync_LockstepDirectionsDetect();
bool RoadSync_ApplyCommandDirection(const RoadSyncDirectionMsg& msg);
bool RoadSync_ApplyCommandUpgrade(const RoadSyncUpgradeMsg& msg);
bool RoadSync_ApplyCommandDemolish(const RoadSyncDemolishMsg& msg);

// Lockstep: a bulldozer commit whose piece the hook cannot read (the cursor
// on a cut's end: the game removes through the hovered node or edge). The
// hovered node and edge travel as positions in the commit's network; every PC
// sets them, runs the tool's update at the cursor and commits what it picks.
struct RoadSyncDemolishHover {
    int nodeKind;    // 0 none, 1 = exe+9e4560, 2 = exe+9e45e0
    float node[3];
    int edgeKind;    // 0 none, 1 = exe+9e4558, 2 = exe+9e45d8
    float edgeA[3];  // the edge's node 0 and node 1
    float edgeB[3];
    int edgePoints;
};
struct LsDemolishHoverMsg {
    RoadSyncDemolishMsg m; // the cursor; no range, no pieces
    RoadSyncDemolishHover hover;
};
// The hovered node / edge as they are now, when they belong to this network.
bool RoadSync_ReadDemolishHover(__int64 manager, RoadSyncDemolishHover* out);
bool RoadSync_ApplyCommandDemolishHover(const LsDemolishHoverMsg& msg);
