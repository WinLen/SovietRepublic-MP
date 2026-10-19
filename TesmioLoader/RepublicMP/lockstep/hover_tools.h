// SPDX-License-Identifier: GPL-3.0-only
// Lockstep: the road hover tools as commands (by SoylentGreed).
//
// The Late dispatcher calls these tools every frame one is selected:
//   kind 0  417a70(world)       road_waypoint
//   kind 1  4181d0(world)       road_sign_erase
//   kind 2  417d80(world)       the other road_sign_* tools (the sign kind is the selected tool)
//   kind 3  4184c0(world, dl)   rail_semaphore* (dl 0 or 1)
//   kind 4  418f90(world)       rail_semaphorecancel
//   kind 5  4182d0(world, mode) road_crossroad0/1/2 (the junction kind of the hovered node)
// Each acts only on a click (exe+a54e91) with the menus closed, from the hover
// state the network's pick left (world +f648..+f6e8: the hovered edge and
// node, the snapped points, the cursor +f6c, +f90), the hover flag +fb9 and
// the selected tool. It splits the edge at the cursor and places or removes a
// waypoint, a sign or a signal on the node.
//
// In lockstep a click frame runs the tool with the click hidden (its
// highlight stays) and the hover state - the pointers as references into the
// network's nodes and edges - is a command. Every PC sets that state, the
// flag, the tool, the click and closed menus and runs the tool inside the
// camera-view edge scope, then puts its own state back.
#pragma once

#include "tesmio_plugin.h"

void HoverTools_Bind(const TsmHost* host);

// False when a kind's routine differs (the tool is then left as it was).
bool HoverTools_Ready(int kind);

// The tool stubs of the Late dispatcher call these instead of the routines.
unsigned __int64 HoverTools_Waypoint(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 HoverTools_SignErase(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 HoverTools_Sign(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 HoverTools_RailPlace(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 HoverTools_RailRemove(unsigned __int64 world, unsigned __int64 a2);
unsigned __int64 HoverTools_Junction(unsigned __int64 world, unsigned __int64 mode);

// The command (lockstep/commands).
bool HoverTools_ApplyCommand(const void* data, size_t size);
