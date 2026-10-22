// SPDX-License-Identifier: GPL-3.0-only
// See citizen_destinations.h.
#include "citizen_destinations.h"
#include "../buildings/build_sync.h"
#include "../../lockstep/commands.h"
#include "../../session/two_states.h"
#include <string.h>
#include <unordered_map>
#include <vector>
#include <math.h>
#include <stdint.h>

namespace {
constexpr int kOffsets[] = {0xF18, 0xF30, 0xF48, 0xF60};
const char* kNames[] = {"workers", "students", "passengers", "tourists"};
constexpr int kStride = 7;
constexpr int kMax = SETTINGS_MAX_SLOT_ENTRIES * 2 / kStride; // 9 endpoints
constexpr uint32_t kFormat = 0x44535431;
constexpr uintptr_t kWorld = 0x9D4F10;
constexpr uintptr_t kBuildingsBegin = 0x9E6A18, kBuildingsEnd = 0x9E6A20;
constexpr uintptr_t kAdd = 0x1D4330, kRemove = 0x1D4440;

struct Entry { float share; unsigned char flags[4]; BYTE* target; };
struct Vector { Entry* begin; Entry* end; Entry* capacity; };
static_assert(sizeof(Entry) == 16 && sizeof(Vector) == 24);
struct State {
    unsigned long long hash[4] = {};
    bool known[4] = {};
    bool muted[4] = {};
    int changes[4] = {};
    ULONGLONG window[4] = {};
    uint32_t type = 0;
    bool seen = false;
    bool oversized[4] = {};
};
std::unordered_map<BYTE*, State> g_states;

struct LsDestBuilding {
    BYTE* b = nullptr;
    uint32_t type = 0;
    float pos[3] = {};
    signed char count[4] = { -1, -1, -1, -1 };
    size_t entryAt = 0;
};
struct LsSentState {
    unsigned long long hash[4] = {};
};
std::vector<LsDestBuilding> g_lsBuildings;
std::vector<Entry> g_lsEntries;
std::unordered_map<BYTE*, LsSentState> g_lsLastSent;

uint32_t Bits(float value) { uint32_t result; memcpy(&result, &value, 4); return result; }
void StoreBits(float* dest, uint32_t value) { memcpy(dest, &value, 4); }

uint32_t TypeHash(BYTE* building) {
    BYTE* type = *(BYTE**)(building + 0x318);
    if (!type) return 0;
    uint32_t hash = 2166136261u;
    int i = 0;
    for (; i < 128 && type[i]; ++i) { hash ^= type[i]; hash *= 16777619u; }
    return i > 0 && i < 128 ? hash : 0;
}

uint32_t LocalType(BYTE* building) {
    __try { return TypeHash(building); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool ValidVector(Vector* vec) {
    uintptr_t a = (uintptr_t)vec->begin, b = (uintptr_t)vec->end, c = (uintptr_t)vec->capacity;
    if (!a && !b && !c) return true;
    return a && a <= b && b <= c && (b-a)%16 == 0 && (c-a)%16 == 0 &&
           (b-a)/16 <= kMax && (c-a) <= 1024*1024;
}

int LocalCount(BYTE* source, int group) {
    __try {
        Vector* vec = (Vector*)(source+kOffsets[group]);
        uintptr_t a=(uintptr_t)vec->begin, b=(uintptr_t)vec->end, c=(uintptr_t)vec->capacity;
        if (!a && !b && !c) return 0;
        if (!a || a>b || b>c || (b-a)%16 || (c-a)%16 || c-a>1024*1024) return -1;
        return (int)((b-a)/16);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool Listed(BYTE* target) {
    BYTE** begin = *(BYTE***)(g_exeBase + kBuildingsBegin);
    BYTE** end = *(BYTE***)(g_exeBase + kBuildingsEnd);
    if (end < begin || end-begin > 200000) return false;
    for (BYTE** it = begin; it < end; ++it) if (*it == target) return true;
    return false;
}

bool ReadWire(BYTE* source, int group, SettingWire* out) {
    __try {
        if (group < 0 || group >= 4) return false;
        Vector* vec = (Vector*)(source + kOffsets[group]);
        if (!ValidVector(vec)) return false; // never truncate a larger list
        memset(out, 0, sizeof(*out));
        uint32_t type = TypeHash(source);
        if (!type) return false;
        out->field = CITIZEN_DESTINATIONS_FIELD;
        out->category = *(int*)(*(BYTE**)(source + 0x318) + 0x360);
        memcpy(out->pos, source + 0x350, 12);
        for (int i = 0; i < 3; ++i) if (!isfinite(out->pos[i])) return false;
        out->slot = group;
        out->value = ((unsigned long long)kFormat << 32) | type;
        out->count = vec->begin ? (int)(vec->end-vec->begin) : 0;
        float sum = 0;
        for (int i = 0; i < out->count; ++i) {
            const Entry* e = &vec->begin[i];
            float* data = &out->data[i*kStride];
            if (!isfinite(e->share) || e->share < 0 || e->share > 1.001f) return false;
            if (e->flags[0] > 1 || e->flags[1] > 1 || e->flags[2] > 1) return false;
            data[5] = e->share;
            sum += e->share;
            StoreBits(&data[6], e->flags[0] | (e->flags[1]<<8) | (e->flags[2]<<16));
            if (!e->target) { StoreBits(&data[3], UINT32_MAX); continue; }
            if (!Listed(e->target)) return false;
            BYTE* typePtr = *(BYTE**)(e->target + 0x318);
            uint32_t targetType = TypeHash(e->target);
            if (!typePtr || !targetType) return false;
            memcpy(data, e->target + 0x350, 12);
            for (int j = 0; j < 3; ++j) if (!isfinite(data[j])) return false;
            StoreBits(&data[3], *(uint32_t*)(typePtr + 0x360));
            StoreBits(&data[4], targetType);
        }
        // Some intermediate slider states need another scan before sending.
        return !out->count || fabsf(sum-1.0f) <= 0.002f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

unsigned long long Hash(const SettingWire& wire) {
    const unsigned char* bytes = (const unsigned char*)&wire;
    unsigned long long hash = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(wire); ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
    return hash;
}

// Temporary test-session correspondence, deliberately rejects ambiguous matches.
BYTE* Resolve(const float* pos, int category, uint32_t type, bool* ambiguous) {
    *ambiguous = false;
    BYTE** begin = *(BYTE***)(g_exeBase + kBuildingsBegin);
    BYTE** end = *(BYTE***)(g_exeBase + kBuildingsEnd);
    if (end < begin || end-begin > 200000) return nullptr;
    BYTE* found = nullptr;
    for (BYTE** it = begin; it < end; ++it) {
        BYTE* b = *it;
        if (!b || !*(BYTE**)(b + 0x318)) continue;
        if (*(int*)(*(BYTE**)(b+0x318)+0x360) != category || TypeHash(b) != type) continue;
        float* p = (float*)(b+0x350);
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) ||
            fabsf(p[0]-pos[0]) > 0.5f || fabsf(p[1]-pos[1]) > 1.0f || fabsf(p[2]-pos[2]) > 0.5f) continue;
        if (found) { *ambiguous = true; return nullptr; }
        found = b;
    }
    return found;
}

struct PendingReason {
    const char* reason;
    int targetIndex;
};

// Only reads references. No native mutators and no partial application.
PendingReason DiagnosePending(const SettingWire& wire) {
    if (wire.slot<0 || wire.slot>=4 || wire.count<0 || wire.count>kMax ||
        (uint32_t)(wire.value>>32)!=kFormat) return {"invalid payload", -1};
    for (int j=0; j<3; ++j) if (!isfinite(wire.pos[j])) return {"invalid source position", -1};
    bool ambiguous=false;
    BYTE* source=Resolve(wire.pos,wire.category,(uint32_t)wire.value,&ambiguous);
    if (ambiguous) return {"ambiguous source", -1};
    if (!source) return {"missing source", -1};
    for (int i=0; i<wire.count; ++i) {
        const float* data=&wire.data[i*kStride];
        if (Bits(data[3])==UINT32_MAX) continue;
        for (int j=0; j<3; ++j) if (!isfinite(data[j])) return {"invalid target position", i};
        BYTE* target=Resolve(data,(int)Bits(data[3]),Bits(data[4]),&ambiguous);
        if (ambiguous) return {"ambiguous target", i};
        if (!target) return {"missing target", i};
    }
    return {"all endpoints present; see application log", -1};
}

bool NativeHelpersReady() {
    const unsigned char addPrefix[] = {0x48,0x89,0x4C,0x24,0x08,0x53,0x48,0x83,0xEC,0x40};
    const unsigned char removePrefix[] = {0x40,0x53,0x48,0x83,0xEC,0x30,0x49,0x8B,0x00,0x49};
    return memcmp(g_exeBase+kAdd, addPrefix, sizeof(addPrefix)) == 0 &&
           memcmp(g_exeBase+kRemove, removePrefix, sizeof(removePrefix)) == 0;
}

void RestoreVector(BYTE* source, int group, const Entry* oldEntries, int oldCount) {
    __try {
        if (group < 0 || group >= 4 || oldCount < 0 || oldCount > kMax) return;
        Vector* vec = (Vector*)(source + kOffsets[group]);
        if (!ValidVector(vec) || !NativeHelpersReady()) return;
        int count = vec->begin ? (int)(vec->end - vec->begin) : 0;
        bool sameTargets = count == oldCount;
        for (int i = 0; sameTargets && i < count; ++i) sameTargets = vec->begin[i].target == oldEntries[i].target;
        if (!sameTargets) {
            typedef void (*RemoveFn)(BYTE*, int, Vector*);
            typedef void (*AddFn)(BYTE*, BYTE*, Vector*);
            for (int i = count - 1; i >= 0; --i) {
                ((RemoveFn)(g_exeBase + kRemove))(g_exeBase + kWorld, i, vec);
                if (!ValidVector(vec) || (vec->begin && vec->end - vec->begin != i)) return;
            }
            for (int i = 0; i < oldCount; ++i) {
                if (oldEntries[i].target && !Listed(oldEntries[i].target)) return;
                ((AddFn)(g_exeBase + kAdd))(g_exeBase + kWorld, oldEntries[i].target, vec);
                if (!ValidVector(vec) || !vec->begin || vec->end - vec->begin != i + 1) return;
            }
        }
        if (ValidVector(vec) && ((oldCount == 0) || (vec->begin && (int)(vec->end - vec->begin) == oldCount))) {
            for (int i = 0; i < oldCount; ++i) {
                vec->begin[i].share = oldEntries[i].share;
                memcpy(vec->begin[i].flags, oldEntries[i].flags, 4);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 0=wait for endpoint, 1=consumed. No retry after a possibly partial native write.
int ApplyNative(const SettingWire& wire, BYTE** changedSource) {
    *changedSource = nullptr;
    __try {
        if (wire.slot < 0 || wire.slot >= 4 || wire.count < 0 || wire.count > kMax ||
            (uint32_t)(wire.value >> 32) != kFormat) return 1;
        for (int i = 0; i < 3; ++i) if (!isfinite(wire.pos[i])) return 1;
        bool ambiguous = false;
        BYTE* source = Resolve(wire.pos, wire.category, (uint32_t)wire.value, &ambiguous);
        if (ambiguous) { Logf("republicmp  DESTINATIONS: ambiguous source - rejected"); return 1; }
        if (!source) return 0;
        BYTE* targets[kMax] = {};
        float sum = 0;
        for (int i = 0; i < wire.count; ++i) {
            const float* data = &wire.data[i*kStride];
            uint32_t flags = Bits(data[6]);
            if ((flags & 0xFF000000u) || (flags&255)>1 || ((flags>>8)&255)>1 || ((flags>>16)&255)>1 ||
                !isfinite(data[5]) || data[5] < 0 || data[5] > 1.001f) return 1;
            sum += data[5];
            for (int j = 0; j < 3; ++j) if (!isfinite(data[j])) return 1;
            if (Bits(data[3]) == UINT32_MAX) {
                if (i != wire.count-1 || Bits(data[4]) != 0) return 1;
            } else {
                targets[i] = Resolve(data, (int)Bits(data[3]), Bits(data[4]), &ambiguous);
                if (ambiguous) { Logf("republicmp  DESTINATIONS: ambiguous target - rejected"); return 1; }
                if (!targets[i]) return 0;
            }
            for (int j = 0; j < i; ++j) if (targets[j] == targets[i]) return 1;
        }
        if (wire.count && fabsf(sum-1.0f) > 0.002f) return 1;
        Vector* vec = (Vector*)(source+kOffsets[wire.slot]);
        if (!ValidVector(vec)) { Logf("republicmp  DESTINATIONS: local vector rejected"); return 1; }
        // Native helpers traced from this exact game's destination panel.
        if (!NativeHelpersReady()) {
            Logf("republicmp  DESTINATIONS: unsupported native helper - rejected"); return 1;
        }
        int count = vec->begin ? (int)(vec->end-vec->begin) : 0;
        bool sameTargets = count == wire.count;
        for (int i = 0; sameTargets && i < count; ++i) sameTargets = vec->begin[i].target == targets[i];
        *changedSource = source;
        if (!sameTargets) {
            typedef void (*RemoveFn)(BYTE*, int, Vector*);
            typedef void (*AddFn)(BYTE*, BYTE*, Vector*);
            for (int i = count-1; i >= 0; --i) {
                ((RemoveFn)(g_exeBase+kRemove))(g_exeBase+kWorld, i, vec);
                if (!ValidVector(vec) || (vec->begin && vec->end-vec->begin != i)) {
                    Logf("republicmp  DESTINATIONS: native removal failed; stop applying"); return 1;
                }
            }
            for (int i = 0; i < wire.count; ++i) {
                ((AddFn)(g_exeBase+kAdd))(g_exeBase+kWorld, targets[i], vec);
                if (!ValidVector(vec) || !vec->begin || vec->end-vec->begin != i+1 || vec->begin[i].target != targets[i]) {
                    Logf("republicmp  DESTINATIONS: native addition failed; stop applying"); return 1;
                }
            }
        }
        for (int i = 0; i < wire.count; ++i) {
            vec->begin[i].share = wire.data[i*kStride+5];
            uint32_t flags = Bits(wire.data[i*kStride+6]);
            for (int j = 0; j < 3; ++j) vec->begin[i].flags[j] = (unsigned char)(flags>>(j*8));
        }
        SettingWire actual;
        bool matches = ReadWire(source, wire.slot, &actual) && actual.count == wire.count;
        // Endpoint coordinates may differ within Resolve's explicit tolerance;
        // verify the resolved local references and payload, not packet padding.
        for (int i = 0; matches && i < wire.count; ++i) {
            matches = vec->begin[i].target == targets[i] &&
                      Bits(actual.data[i*kStride+5]) == Bits(wire.data[i*kStride+5]) &&
                      Bits(actual.data[i*kStride+6]) == Bits(wire.data[i*kStride+6]);
        }
        Logf("republicmp  DESTINATIONS: %s %d endpoints at (%.1f,%.1f) %s", kNames[wire.slot],
             wire.count, wire.pos[0], wire.pos[2], matches ? "applied and read back" : "FAILED readback");
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DESTINATIONS: exception; possibly partial application - not retried"); return 1;
    }
}
}

void CitizenDestinations_Bind(const TsmHost* host) { TsmBind(host); }
void CitizenDestinations_Reset() {
    g_states.clear();
    g_lsBuildings.clear();
    g_lsEntries.clear();
    g_lsLastSent.clear();
}

void CitizenDestinations_LockstepBaseline(BYTE** buildings, int count) {
    g_lsBuildings.resize(count);
    g_lsEntries.clear();
    for (int i = 0; i < count; ++i) {
        LsDestBuilding& s = g_lsBuildings[i];
        s = {};
        BYTE* b = buildings[i];
        if (!b) continue;
        __try {
            uint32_t type = TypeHash(b);
            if (!type) continue;
            s.b = b;
            s.type = type;
            memcpy(s.pos, b + 0x350, 12);
            s.entryAt = g_lsEntries.size();
            for (int g = 0; g < 4; ++g) {
                Vector* vec = (Vector*)(b + kOffsets[g]);
                if (!ValidVector(vec)) {
                    s.count[g] = -1;
                    continue;
                }
                int n = vec->begin ? (int)(vec->end - vec->begin) : 0;
                s.count[g] = (signed char)n;
                if (n > 0) g_lsEntries.insert(g_lsEntries.end(), vec->begin, vec->begin + n);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s.b = nullptr;
        }
    }
}

void CitizenDestinations_LockstepDetect() {
    for (const LsDestBuilding& old : g_lsBuildings) {
        if (!old.b) continue;
        BYTE* b = old.b;
        __try {
            if (TypeHash(b) != old.type || memcmp(b + 0x350, old.pos, 12) != 0) continue;
            size_t at = old.entryAt;
            for (int g = 0; g < 4; ++g) {
                int oldCount = old.count[g];
                if (oldCount < 0) continue;
                const Entry* oldEntries = oldCount > 0 ? &g_lsEntries[at] : nullptr;
                at += oldCount;
                Vector* vec = (Vector*)(b + kOffsets[g]);
                if (!ValidVector(vec)) continue;
                int nowCount = vec->begin ? (int)(vec->end - vec->begin) : 0;
                if (nowCount == oldCount && (nowCount == 0 || memcmp(vec->begin, oldEntries, nowCount * sizeof(Entry)) == 0))
                    continue;
                if (!TwoStates_CheckBuildingControlAllowed(b)) {
                    RestoreVector(b, g, oldEntries, oldCount);
                    continue;
                }
                SettingWire wire;
                if (ReadWire(b, g, &wire)) {
                    unsigned long long h = Hash(wire);
                    LsSentState& sent = g_lsLastSent[b];
                    if (sent.hash[g] != h) {
                        sent.hash[g] = h;
                        Commands_Submit(LS_CMD_SETTING, &wire, sizeof(wire));
                    }
                    RestoreVector(b, g, oldEntries, oldCount);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void CitizenDestinations_Scan(BYTE** buildings, int count, bool sendChanges, bool clicked) {
    for (auto& kv : g_states) kv.second.seen = false;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < count; ++i) {
        BYTE* b = buildings[i];
        for (int group = 0; group < 4; ++group) {
            SettingWire wire;
            if (!ReadWire(b, group, &wire)) {
                int localCount=LocalCount(b,group);
                if (localCount>kMax) {
                    uint32_t type=LocalType(b);
                    if (!type) continue;
                    State& state=g_states[b];
                    if (state.type!=type) { state={}; state.type=type; }
                    state.seen=true;
                    if (!state.oversized[group]) {
                        Logf("republicmp  DESTINATIONS: %s list has %d endpoints; limit=%d, not sent",
                             kNames[group],localCount,kMax);
                        state.oversized[group]=true;
                    }
                }
                continue;
            }
            State& state = g_states[b];
            if (state.type != (uint32_t)wire.value) { state = {}; state.type = (uint32_t)wire.value; }
            state.seen = true;
            state.oversized[group] = false;
            unsigned long long hash = Hash(wire);
            if (state.known[group] && hash != state.hash[group] && sendChanges && clicked && !state.muted[group]) {
                if (now-state.window[group] > SETTINGS_FLOOD_WINDOW_MS) { state.window[group] = now; state.changes[group] = 0; }
                if (++state.changes[group] > SETTINGS_FLOOD_LIMIT) {
                    state.muted[group] = true;
                    Logf("republicmp  DESTINATIONS: %s changes too often; muted", kNames[group]);
                } else {
                    BuildSync_SendSetting(wire);
                    Logf("republicmp  DESTINATIONS: sending %s %d endpoints at (%.1f,%.1f)",
                         kNames[group], wire.count, wire.pos[0], wire.pos[2]);
                }
            }
            state.hash[group] = hash;
            state.known[group] = true;
        }
    }
    for (auto it = g_states.begin(); it != g_states.end();) {
        if (!it->second.seen) it = g_states.erase(it); else ++it;
    }
}

// The renderer lock ([exe+0x995268]+0x80): the main thread holds it through
// Early, where citizens read these lists, and the game's window edits them
// under it too. A list is only rebuilt while holding it.
#define RENDERER_RVA 0x995268
#define RENDERER_LOCK_OFFSET 0x80
#define RENDERER_LOCK_WAIT_MS 50

static CRITICAL_SECTION* TakeRendererLock() {
    CRITICAL_SECTION* cs = nullptr;
    __try {
        BYTE* renderer = *(BYTE**)(g_exeBase + RENDERER_RVA);
        if (renderer) cs = (CRITICAL_SECTION*)(renderer + RENDERER_LOCK_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) { cs = nullptr; }
    if (!cs) return nullptr;
    ULONGLONG until = GetTickCount64() + RENDERER_LOCK_WAIT_MS;
    do {
        if (TryEnterCriticalSection(cs)) return cs;
        SwitchToThread();
    } while (GetTickCount64() < until);
    return nullptr;
}

bool CitizenDestinations_Apply(const SettingWire& wire) {
    BYTE* changed = nullptr;
    CRITICAL_SECTION* lock = TakeRendererLock();
    if (!lock) return false; // tried again next frame
    int result = ApplyNative(wire, &changed);
    LeaveCriticalSection(lock);
    if (changed && wire.slot >= 0 && wire.slot < 4) {
        SettingWire actual;
        if (ReadWire(changed, wire.slot, &actual)) {
            State& state = g_states[changed];
            if (state.type != (uint32_t)actual.value) { state = {}; state.type = (uint32_t)actual.value; }
            state.hash[wire.slot] = Hash(actual); // remote application must not echo
            state.known[wire.slot] = true;
        }
    }
    return result != 0;
}

bool CitizenDestinations_ApplyLockstep(const SettingWire& wire) {
    BYTE* changed = nullptr;
    int result = ApplyNative(wire, &changed);
    if (changed && wire.slot >= 0 && wire.slot < 4) {
        g_lsLastSent[changed].hash[wire.slot] = 0;
        SettingWire actual;
        if (ReadWire(changed, wire.slot, &actual)) {
            State& state = g_states[changed];
            if (state.type != (uint32_t)actual.value) { state = {}; state.type = (uint32_t)actual.value; }
            state.hash[wire.slot] = Hash(actual);
            state.known[wire.slot] = true;
        }
    }
    return result != 0;
}

void CitizenDestinations_LogPending(const SettingWire& wire, const char* event) {
    __try {
        PendingReason why=DiagnosePending(wire);
        const char* group=(wire.slot>=0 && wire.slot<4) ? kNames[wire.slot] : "invalid group";
        Logf("republicmp  DESTINATIONS: %s %s list %d source=(%.2f,%.2f,%.2f) category=%d typeHash=%08X: %s",
             event,group,wire.count,wire.pos[0],wire.pos[1],wire.pos[2],wire.category,
             (uint32_t)wire.value,why.reason);
        if (why.targetIndex>=0) {
            const float* data=&wire.data[why.targetIndex*kStride];
            Logf("republicmp  DESTINATIONS: target[%d]=(%.2f,%.2f,%.2f) category=%d typeHash=%08X",
                 why.targetIndex,data[0],data[1],data[2],(int)Bits(data[3]),Bits(data[4]));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("republicmp  DESTINATIONS: %s diagnostic read failed",event);
    }
}
