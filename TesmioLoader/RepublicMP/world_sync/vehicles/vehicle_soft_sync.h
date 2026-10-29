// SPDX-License-Identifier: GPL-3.0-only
// Soft authoritative vehicle synchronization (Host -> Client).
//
// Instead of treating in-transit vehicle coordinate micro-differences as fatal
// DESYNCs, the simulation keeps routes, stops and cargo synchronized via
// Lockstep commands while the Host periodically (and on-demand whenever
// StateHash detects vehicle drift) sends compact authoritative vehicle state
// batches (LS_CMD_VEHICLE_SOFT_SYNC) to self-heal fuel, wear, speed, cargo
// amounts and in-transit positions on the Client.
#pragma once

#include "tesmio_plugin.h"

void VehicleSoftSync_Bind(const TsmHost* host);
void VehicleSoftSync_Reset();

// Host: schedules an immediate soft-sync batch on the next tick boundary
// (called when StateHash comparison notices vehicle logical or position drift).
void VehicleSoftSync_RequestImmediate();

// Host, main thread after a simulation tick completes.
void VehicleSoftSync_HostTick(unsigned long long tick);

// Host, main thread inside Commands_Boundary right before sending/applying a soft-sync batch.
void VehicleSoftSync_RefreshCommandTreasury(void* data, size_t size);

// Both PCs, main thread inside Commands_Boundary.
bool VehicleSoftSync_ApplyCommand(const void* data, size_t size);

// Resolves the building a vehicle (or its lead locomotive if a coupled wagon) is currently parked/stopped at.
BYTE* VehicleSoftSync_ResolveEffectiveBld(const BYTE* v);

// Test helper: builds a wire packet into outBuf (returns true if non-empty).
bool VehicleSoftSync_BuildPacketForTest(void* outBuf, size_t maxBytes, size_t* outSize, bool immediate);

struct VehicleSoftSyncDiagStats {
    unsigned long long batchesSent;
    unsigned long long batchesApplied;
    unsigned long long immediateSweeps;
    unsigned long long vehiclesSynced;
    unsigned long long posSnapped;
    unsigned long long posBlended;
    unsigned long long cargoUpdated;
    unsigned long long bldStorageUpdated;
    float lastMaxDriftM;
    float allTimeMaxDriftM;
};
void VehicleSoftSync_GetDiagStats(VehicleSoftSyncDiagStats* out);
