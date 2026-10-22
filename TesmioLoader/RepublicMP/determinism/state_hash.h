// SPDX-License-Identifier: GPL-3.0-only
// State hash: a compact fingerprint of the simulation, per subsystem.
//
// Used two ways:
//  - sync check (session/sync_check): host and player compare their summaries
//    every game hour - the counts and sums show how far the two independent
//    simulations have drifted apart;
//  - determinism probe (determinism/determinism): the exact hashes of two runs from
//    the same save are compared step by step.
//
// Everything is read off the world object (thisPtr = exe+0x9D4F10); the
// containers are std::vector<T*> (begin, end):
//
//   world+0x11B08  buildings (exe+0x9E6A18). Building: +0x00 int index,
//                  +0x604 float construction progress, +0x970 vector of
//                  0xE0-byte storage slots (slot +0x00 vector of 16-byte
//                  entries {Resource* +0, float amount +8, float +0xC}),
//                  +0xBC0 vector<Worker*> assigned citizens, +0xDE4 float
//                  produced in the current window.
//   world+0x12810  vehicles (exe+0x9E7720). Vehicle: +0x04 int index,
//                  +0x3B8 cargo vector (same 16-byte entries),
//                  +0x698 int current schedule stop index,
//                  +0xDE0 position (main C3D_NODE +0xD60, position +0x80).
//   world+0x126A8  citizens (exe+0x9E75B8). Worker: +0x20 home Building*,
//                  +0xD4 float age, +0x620 position (C3D_NODE +0x5A0).
//
// Found through the save/load routines (FUN_14042cc70 / FUN_140430fc0) and
// the per-object writers FUN_1401edc40 (building), FUN_1406c6770 (vehicle),
// FUN_14083d9d0 (worker). A resource is identified by its name (the
// resource object starts with it); pointers are never hashed - they differ
// between machines - only indices, names and values.
#pragma once

#include "tesmio_plugin.h"
#include <vector>

struct StateSummary {
    int day, year;
    float dayProgress;
    double rub, usd;
    int buildings, vehicles, citizens;
    double storageTotal;    // all building storage amounts
    double cargoTotal;      // all vehicle cargo amounts
    double productionTotal; // all buildings' current production windows
    double ageTotal;        // all citizens' ages
    unsigned long long hashClock;       // day, year, progress, money
    unsigned long long hashBuildings;
    unsigned long long hashVehicles;    // logical vehicle state (id, schedule stop, cargo)
    unsigned long long hashVehiclesPos; // in-transit 3D positions (soft-synced in multiplayer)
    unsigned long long hashCitizens;    // logical citizen state (home building, age)
};

void StateHash_Bind(const TsmHost* host);

// Reads the current state. False if the world is not loaded or a read faulted.
// Game thread only (reads live engine objects).
bool StateHash_Compute(StateSummary* out);

// Combined hash of all subsystems.
unsigned long long StateHash_All(const StateSummary& s);

// Per-object detail for the probe: what the subsystem hashes cover, one
// record per list entry, so two runs can name the first objects that differ.
#define RECORD_SLOTS 8
struct BuildingRecord {
    int index;              // +0x00
    int category;           // type +0x360 (-1: no type)
    float pos[3];           // +0x3a0
    float progress;         // +0x604
    float produced;         // +0xde4
    int workers;            // +0xbc0 count
    unsigned long long storage; // hash of the storage slots
    int slots;              // storage slots (+0x970), up to RECORD_SLOTS described below
    int slotEntries[RECORD_SLOTS];
    float slotAmount[RECORD_SLOTS];
};
struct VehicleRecord {
    int index;              // +0x04
    float pos[3];           // +0xde0
    unsigned long long cargo; // hash of the cargo entries
};
struct CitizenRecord {
    int home;               // home building's index (-1: none)
    float age;              // +0xd4
    float pos[3];           // +0x620
};

// Game thread only. False if the world is not loaded or a read faulted.
bool StateHash_BuildingRecords(std::vector<BuildingRecord>& out);
bool StateHash_CitizenRecords(std::vector<CitizenRecord>& out);
bool StateHash_VehicleRecords(std::vector<VehicleRecord>& out);
