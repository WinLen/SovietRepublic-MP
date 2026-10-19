// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the route tools as commands (by SoylentGreed).
//
// vehicle_addstop (2b2760) and line_addstop (2b4a50) add a stop to the
// schedule of the window's object (world +d298: a vehicle, or a line of the
// lines manager [world +12800]) at the window's stop index ([world +d2a0]
// +264). Both call the shared pick 2b5120 (at 2b2800 and 2b4b03), which on a
// click with an allowed pick writes the station building or the signal node
// to its out arguments and answers 1; the tool then adds the stop. In a
// lockstep session that answer becomes a command and the pick answers 0 on
// the clicking PC; at the boundary every PC runs the tool itself with the
// object, the window index and the pick forced to the carried result.
//
// line_addstop_copy / line_addstop_copy2 (2b4c60) put a picked vehicle on the
// line +d298. In a session the tool runs with the click hidden; its mark
// (40cde0 at 2b505c) tells the picked vehicle and whether it is allowed, and
// an allowed click becomes a command: every PC makes the tool's click steps
// (498990 test, 1aa540 home test, 497b10 line, 3bb330 start with Ctrl).
//
// vehicle_godepo / vehicle_godepo_batch (2b2dd0) send the vehicle +d298 (or a
// depot's own vehicles) to a picked depot. In a session the tool runs with the
// click hidden; its mark (40bd60 at 2b3790) gives the depot and, from the
// tool's frame, the vehicles it listed: one command, and every PC makes the
// tool's sending steps with the game's own routines and tests.
//
// The line window's "New stop" edits the window's working copy (+270), which
// only the clicking PC holds: that stays local.
#pragma once

#include "tesmio_plugin.h"

void RouteTools_Bind(const TsmHost* host);

// False when a game routine the commands call differs (the tools are then put away in lockstep).
bool RouteTools_Ready();

// The relays (sim_patches): the stop pick at 2b2800 (kind 0) and 2b4b03 (kind 1).
unsigned __int64 RouteTools_StopPick(int kind, unsigned __int64 world, unsigned __int64 a2, unsigned __int64 a3,
                                     unsigned __int64 a4, unsigned __int64 a5, unsigned __int64 a6,
                                     unsigned __int64* building, unsigned __int64* node);
// The line copy tool's call (the tool stub of 31452c) and its mark (2b505c).
unsigned __int64 RouteTools_LineCopyTool(unsigned __int64 world);
unsigned __int64 RouteTools_LineMark(unsigned __int64 world, unsigned __int64 vehicle, unsigned __int64 a3,
                                     unsigned __int64 refused);

// The depot tool's call (the tool stubs of 31426f and 314291) and its mark
// (2b3790; the relay passes its caller's rsp and return address).
unsigned __int64 RouteTools_DepotTool(unsigned __int64 world, unsigned __int64 batch);
unsigned __int64 RouteTools_DepotMark(unsigned __int64 world, unsigned __int64 depot, unsigned __int64 a3,
                                      unsigned __int64 a4, unsigned __int64 a5, unsigned __int64 callerRsp,
                                      unsigned __int64 returnTo);

// Additional carried tools (square road/water, go_there, bojka, join_hub, worker_relocate, vehicle tools).
unsigned __int64 RouteTools_SecondRoadTool(unsigned __int64 world, unsigned __int64 type, unsigned __int64 square);
unsigned __int64 RouteTools_GoThereTool(unsigned __int64 world);
unsigned __int64 RouteTools_BojkaTool(unsigned __int64 world, unsigned __int64 mode);
unsigned __int64 RouteTools_JoinHubStopTool(unsigned __int64 world);
unsigned __int64 RouteTools_JoinHubTool(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 RouteTools_WorkerRelocateTool(unsigned __int64 world);
unsigned __int64 RouteTools_VehicleCopyTool(unsigned __int64 world);
unsigned __int64 RouteTools_VehicleReplaceTool(unsigned __int64 world);
unsigned __int64 RouteTools_VehicleReplaceAutoTool(unsigned __int64 world);
unsigned __int64 RouteTools_VehicleToVehicleTool(unsigned __int64 world);
unsigned __int64 RouteTools_VehicleSendConstrTool(unsigned __int64 world);

// The command (lockstep/commands).
bool RouteTools_ApplyCommand(const void* data, size_t size);
