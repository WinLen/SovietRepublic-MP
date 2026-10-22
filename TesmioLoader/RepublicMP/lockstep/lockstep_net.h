// SPDX-License-Identifier: GPL-3.0-only
// Lockstep channel: one ordered TCP connection that carries the shared
// simulation between the host and the player.
//
// Both PCs load the same save with the determinism layer on (session/
// save_sync), so their simulations start equal; from there they stay equal
// if both run the same ticks with the same commands between them:
//
//  - the host admits the ticks: when it runs tick T it sends GRANT(T, speed),
//    and the player runs tick T only once that grant has arrived. The host
//    stays at most LOCKSTEP_WINDOW ticks ahead of what the player reports
//    (ACK), so the slower PC sets the pace;
//  - the player's actions (lockstep/commands) go to the host (CMD); the host
//    orders them with its own, makes each after its current tick and sends it
//    on (CMDS, with that tick). TCP keeps the order: a command made after
//    tick N always arrives before GRANT(N + 1), so the player makes it at the
//    same point;
//  - pause and speed are the host's: the player's keys and buttons are
//    requests (PAUSE, SPEED), its state comes back for the buttons (STATE).
//    A selected construction tool does not stop the ticks (it acts in a
//    private tool scope);
//  - every LOCKSTEP_HASH_TICKS ticks the player sends its state summary
//    (HASH, determinism/state_hash) and the host compares it with its own;
//    the first difference is reported.
//
// Received messages are queued by a receive thread and handled on the game
// thread (LockstepNet_Poll, at the start of a frame). Sends come from the
// game thread.
#pragma once

#include "tesmio_plugin.h"
#include "../determinism/state_hash.h"

#define LOCKSTEP_PORT 5560
#define LOCKSTEP_WINDOW 24      // ticks the host may run ahead of the player (~800 ms buffer)
#define LOCKSTEP_HASH_TICKS 60  // ticks between state comparisons

void LockstepNet_Bind(const TsmHost* host);

// Session thread. Blocks until connected, cancelled or timed out.
bool LockstepNet_Host(DWORD timeoutMs, volatile bool* cancel);
bool LockstepNet_Join(const char* hostIp, DWORD timeoutMs, volatile bool* cancel);
void LockstepNet_Close();
bool LockstepNet_IsConnected();
bool LockstepNet_IsHost();
// The first state difference found (host; the player gets it from the host), or "".
const char* LockstepNet_Desync();
// Host: the last tick whose state summaries matched (0 = none compared yet).
unsigned long long LockstepNet_LastMatchedTick();

// Game thread, start of a frame: handles everything received.
void LockstepNet_Poll();

// Host (game thread).
bool LockstepNet_MayRun(unsigned long long tick);
void LockstepNet_Grant(unsigned long long tick, int speed);
int LockstepNet_TakePauseToggles();
int LockstepNet_TakeSpeedRequest(); // 0 = none
// Any thread: tells the player the world is sent again (a re-sync).
void LockstepNet_SendResync();
// Any thread: a keepalive, and the time since the other game sent anything.
void LockstepNet_Ping();
unsigned long long LockstepNet_MsSinceReceived();
// The round trip of the last answered ping (false until one came back).
bool LockstepNet_RoundTrip(unsigned long long* ms);
// How the ticks flow: host - the player's ticks behind the last grant, and the
// seconds the host waited for the player (the window full); player - grants
// received but not run yet. Any thread.
struct LockstepFlow {
    unsigned long long behind;
    int stalls;                      // seconds with a wait since the session started
    unsigned long long msSinceStall; // ~0 when it never waited
};
LockstepFlow LockstepNet_Flow();
void LockstepNet_SendState(bool paused, int speed);

// Player (game thread).
bool LockstepNet_TakeGrant(unsigned long long tick, int* speed);
int LockstepNet_GrantBacklog(unsigned long long nextTick);
void LockstepNet_Ack(unsigned long long completed);
// Any thread: the host announced a re-sync (taken once).
bool LockstepNet_TakeResync();
void LockstepNet_SendPauseToggle();
void LockstepNet_SendSpeed(int speed);
// The host's pause and speed for the buttons; false until it sent them.
bool LockstepNet_HostState(bool* paused, int* speed);

// Both: commands (host: made after `afterTick`; player: to the host).
void LockstepNet_SendCommand(int kind, unsigned long long afterTick, const void* data, size_t size);
// Both, after tick `tick`: the state summary for the comparison.
void LockstepNet_StateAfter(unsigned long long tick, const StateSummary& s);

// Non-simulation UI cursor & map ping sync between Host and Player.
struct PartnerCursorInfo {
    bool valid;
    float x, y, z;
    float camX, camZ;
    ULONGLONG lastUpdateMs;
    bool hasPing;
    float pingX, pingY, pingZ;
    ULONGLONG pingReceivedMs;
    unsigned pingSeq;
};
void LockstepNet_SendCursor(float x, float y, float z, float camX, float camZ, bool isPing);
PartnerCursorInfo LockstepNet_GetPartnerCursor();

// Sends the client's compressed log bundle to the Host (on DESYNC, DISCONNECT, or CRASH).
void LockstepNet_SendClientLogs(const char* reason);
