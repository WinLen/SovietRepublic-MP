// SPDX-License-Identifier: GPL-3.0-only
// See placement_collision.h.
#include "placement_collision.h"
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

// mov rax,rsp / movss [rax+20h],xmm3 / mov [rax+10h],rdx / mov [rax+8],rcx -
// 16 bytes (the loader's jump needs 14), none RIP-relative.
static const unsigned char kQueryExpect[16] = {
    0x48, 0x8B, 0xC4, 0xF3, 0x0F, 0x11, 0x58, 0x20, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08
};

#define NET_SHORTLIST_BEGIN_OFFSET 0xb00
#define NET_SHORTLIST_END_OFFSET 0xb08

// Return addresses (RVA) of the building tool's 19 calls: CALL + 5.
static const unsigned int kToolReturnRvas[19] = {
    0x2c97ed, 0x2c98c7, 0x2c9997, 0x2c9a67, 0x2c9b37, 0x2c9c07, 0x2c9cd7, 0x2c9da7, 0x2c9e77, 0x2ca013,
    0x2ca185, 0x2ca40f, 0x2ca4da, 0x2ca5a5, 0x2ca670, 0x2ca73b, 0x2ca806, 0x2ca8d1, 0x2ca99c
};

// network, position, two floats in xmm2/xmm3, sixteen stack slots forwarded
// bit for bit (argument 10 is the sixth of them).
typedef bool (*QueryFn)(__int64 network, __int64 position, float a3, float a4,
                        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9, __int64 a10,
                        __int64 a11, __int64 a12, __int64 a13, __int64 a14, __int64 a15, __int64 a16,
                        __int64 a17, __int64 a18, __int64 a19, __int64 a20);
static QueryFn g_origQuery = nullptr;

void PlacementCollision_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static bool IsToolCall(void* ret)
{
    unsigned long long rva = (unsigned long long)((BYTE*)ret - g_exeBase);
    for (int i = 0; i < 19; i++)
        if (rva == kToolReturnRvas[i]) return true;
    return false;
}

// Empties the shortlist for the call; returns false (nothing changed) if its
// header is unreadable or malformed.
static bool SuppressShortlist(__int64 network, __int64* begin, __int64* savedEnd)
{
    __try {
        __int64 b = *(__int64*)(network + NET_SHORTLIST_BEGIN_OFFSET);
        __int64 e = *(__int64*)(network + NET_SHORTLIST_END_OFFSET);
        if (e < b || ((e - b) & 7)) return false;
        *begin = b;
        *savedEnd = e;
        *(__int64*)(network + NET_SHORTLIST_END_OFFSET) = b;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Puts the end back - unless the game replaced the shortlist meanwhile.
static void RestoreShortlist(__int64 network, __int64 begin, __int64 savedEnd)
{
    __try {
        if (*(__int64*)(network + NET_SHORTLIST_BEGIN_OFFSET) == begin &&
            *(__int64*)(network + NET_SHORTLIST_END_OFFSET) == begin)
            *(__int64*)(network + NET_SHORTLIST_END_OFFSET) = savedEnd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool HookedQuery(__int64 network, __int64 position, float a3, float a4,
                        __int64 a5, __int64 a6, __int64 a7, __int64 a8, __int64 a9, __int64 a10,
                        __int64 a11, __int64 a12, __int64 a13, __int64 a14, __int64 a15, __int64 a16,
                        __int64 a17, __int64 a18, __int64 a19, __int64 a20)
{
    __int64 begin = 0, savedEnd = 0;
    if (!network || !IsToolCall(_ReturnAddress()) || !SuppressShortlist(network, &begin, &savedEnd))
        return g_origQuery(network, position, a3, a4, a5, a6, a7, a8, a9, a10,
                           a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);

    bool result = false;
    __try {
        result = g_origQuery(network, position, a3, a4, a5, a6, a7, a8, a9, 0, // the whole network, not the view
                             a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
    } __finally {
        RestoreShortlist(network, begin, savedEnd);
    }
    return result;
}

bool InstallPlacementCollisionHook()
{
    return InstallInlineHook(g_exeBase + HOOK_COLLISION_QUERY_RVA, (void*)HookedQuery,
                             (void**)&g_origQuery, kQueryExpect, sizeof(kQueryExpect),
                             "republicmp placement collision");
}
