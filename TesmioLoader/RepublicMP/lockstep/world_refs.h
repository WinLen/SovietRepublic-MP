// SPDX-License-Identifier: GPL-3.0-only
// Objects of the world as identities the same on every PC (lockstep).
//
// In lockstep the world's lists are the same on every PC: an object travels as
// its place in its list and a check. Vehicles (world +12810) and buildings
// (world +11b08) are checked by their ids (vehicle +4, building +0), lines of
// the lines manager [world +12800] (+0 and +18) and signal nodes (networks
// world +f908 / +fa28) by a hash of their words.
#pragma once

#include <stddef.h>
#include "tesmio_plugin.h"

void WorldRefs_Bind(const TsmHost* host);

typedef unsigned __int64 U64;

#pragma pack(push, 1)
struct RouteRef {
    unsigned list;  // a line: 0 or 1 (the manager's +0 / +18); a node: the network's field
    int place;      // -1: none
    unsigned check; // a building's or vehicle's id, a hash of a line's or node's words
};
#pragma pack(pop)

extern const RouteRef kNoRef;

// Main or render thread; they read the world without a guard (callers use __try).
bool Ref_Vehicle(U64 vehicle, RouteRef& r);
U64 Ref_VehicleAt(const RouteRef& r);
bool Ref_Building(U64 building, RouteRef& r);
U64 Ref_BuildingAt(const RouteRef& r);
bool Ref_Line(U64 line, RouteRef& r);
U64 Ref_LineAt(const RouteRef& r);
bool Ref_Node(U64 node, RouteRef& r);
U64 Ref_NodeAt(const RouteRef& r);

// Shared deterministic hash & vector indexing helpers for lockstep modules.
unsigned Ref_FnvWords(const unsigned* words, int n);
unsigned Ref_NodeCheck(U64 node);
unsigned Ref_IndexInVector(U64 vector, U64 p);
U64 Ref_AtVectorIndex(U64 vector, unsigned index);
void Ref_SetStandInVector(unsigned char* at, const void* begin, size_t bytes);

// Resource (world + 0xC2B0) and VehicleType (world + 0x12840) definition pointer encoding.
bool Ref_DefPtr(U64 p, unsigned char& tag, unsigned& index);
bool Ref_DefPtrAt(unsigned char tag, unsigned index, U64& p);

struct TsmRoutineSig;
bool Ref_CheckRoutineTable(int* cache, const TsmRoutineSig* table, size_t count, const char* moduleTag);

