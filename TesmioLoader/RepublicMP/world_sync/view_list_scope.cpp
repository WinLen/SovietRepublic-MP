// SPDX-License-Identifier: GPL-3.0-only
// See view_list_scope.h.
#include "view_list_scope.h"
#include "networks/road_sync.h" // RoadSync_GetCategoryInfo (the network managers)
#include <vector>
#include <algorithm>

#define WORLD_THIS_RVA 0x9D4F10
#define RENDERER_RVA 0x995268
#define RENDERER_LOCK_OFFSET 0x80
#define GAME_ALLOCATE_RVA 0x19e0 // FUN_1400019e0(count, stride)
#define GAME_RELEASE_RVA 0x1a50  // FUN_140001a50(pointer, count, stride)

#define NET_WHOLE_OFFSET 0x2b0  // committed edges: begin, end
#define NET_VIEW_OFFSET 0xaa0   // camera-visible edges: begin, end, capacity
#define NET_SECOND_OFFSET 0xab8 // second view list: begin, end, capacity
#define EDGE_POINTS_OFFSET 0x08 // points: begin, end (24-byte records, XYZ first)
#define EDGE_NODE0_OFFSET 0x20
#define EDGE_NODE1_OFFSET 0x28
#define NODE_POS_OFFSET 0x4
#define POINT_STRIDE 0x18

#define LIST_SLACK 256           // room for edges the command adds
#define LOCK_WAIT_MS 200

typedef void* (*AllocateFn)(size_t count, size_t stride);
typedef void (*ReleaseFn)(void* p, size_t count, size_t stride);

struct ListSwap {
    __int64 header;           // network + NET_VIEW_OFFSET / NET_SECOND_OFFSET
    __int64 begin, end, cap;  // this PC's own list
    __int64 buf;              // the scope's buffer
    size_t bufCount;
};
struct NetScope {
    __int64 network;
    ListSwap lists[2];
    int swapped; // lists swapped so far
    std::vector<__int64> before; // the committed edges at Begin, sorted
};

static NetScope g_nets[ROAD_SYNC_CATEGORY_COUNT];
static int g_netCount = 0;
static int g_depth = 0;
static CRITICAL_SECTION* g_rendererLock = nullptr;
static std::vector<__int64> g_whole; // scratch: a committed vector, sorted

void ViewListScope_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static CRITICAL_SECTION* RendererLock()
{
    __try {
        __int64 renderer = *(__int64*)(g_exeBase + RENDERER_RVA);
        return renderer ? (CRITICAL_SECTION*)(renderer + RENDERER_LOCK_OFFSET) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool TakeLock()
{
    g_rendererLock = RendererLock();
    if (!g_rendererLock) return false;
    ULONGLONG until = GetTickCount64() + LOCK_WAIT_MS;
    do {
        if (TryEnterCriticalSection(g_rendererLock)) return true;
        SwitchToThread();
    } while (GetTickCount64() < until);
    g_rendererLock = nullptr;
    return false;
}

static bool InBox(const float* p, const float box[4])
{
    return p[0] >= box[0] && p[0] <= box[2] && p[2] >= box[1] && p[2] <= box[3];
}

// Edge count of the committed vector, or -1.
static long long WholeCount(__int64 network)
{
    __try {
        __int64 b = *(__int64*)(network + NET_WHOLE_OFFSET);
        __int64 e = *(__int64*)(network + NET_WHOLE_OFFSET + 8);
        return (b && e >= b && !((e - b) & 7)) ? (e - b) / 8 : (b || e ? -1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Writes the edges inside the box into out (room for max); count or -1.
static long long SelectEdges(__int64 network, const float box[4], __int64* out, long long max)
{
    long long n = 0;
    __try {
        __int64* it = *(__int64**)(network + NET_WHOLE_OFFSET);
        __int64* end = *(__int64**)(network + NET_WHOLE_OFFSET + 8);
        for (; it && it < end && n < max; ++it) {
            __int64 edge = *it;
            if (!edge) continue;
            __int64 n0 = *(__int64*)(edge + EDGE_NODE0_OFFSET);
            __int64 n1 = *(__int64*)(edge + EDGE_NODE1_OFFSET);
            bool inside = (n0 && InBox((const float*)(n0 + NODE_POS_OFFSET), box)) ||
                          (n1 && InBox((const float*)(n1 + NODE_POS_OFFSET), box));
            const unsigned char* p = *(const unsigned char**)(edge + EDGE_POINTS_OFFSET);
            const unsigned char* pe = *(const unsigned char**)(edge + EDGE_POINTS_OFFSET + 8);
            for (; !inside && p && p < pe; p += POINT_STRIDE) inside = InBox((const float*)p, box);
            if (inside) out[n++] = edge;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

static bool ReadHeader(__int64 header, __int64* b, __int64* e, __int64* c)
{
    __try {
        *b = *(__int64*)header;
        *e = *(__int64*)(header + 8);
        *c = *(__int64*)(header + 16);
        return (!*b && !*e && !*c) || (*b && *e >= *b && *c >= *e && !((*e - *b) & 7) && !((*c - *b) & 7));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// End first, so a reader between the writes sees an empty list, never a
// begin past its end.
static void WriteHeader(__int64 header, __int64 b, __int64 e, __int64 c)
{
    __try {
        *(volatile __int64*)(header + 8) = *(volatile __int64*)header;
        *(volatile __int64*)header = b;
        *(volatile __int64*)(header + 16) = c;
        *(volatile __int64*)(header + 8) = e;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool SortedHas(const std::vector<__int64>& v, __int64 edge)
{
    return std::binary_search(v.begin(), v.end(), edge);
}

static bool WasInWhole(__int64 edge)
{
    return SortedHas(g_whole, edge);
}

static bool CopyWhole(__int64 network, __int64* out, long long count)
{
    __try {
        memcpy(out, *(void**)(network + NET_WHOLE_OFFSET), (size_t)count * 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Compacts this PC's own list to the edges still committed; new size.
static long long FilterOwn(__int64 begin, __int64 end)
{
    long long kept = 0;
    __try {
        __int64* p = (__int64*)begin;
        long long n = (end - begin) / 8;
        for (long long i = 0; i < n; i++)
            if (WasInWhole(p[i])) p[kept++] = p[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return kept;
}

// The committed edges at Begin, sorted (no SEH here: the vector's iterators unwind).
static bool KeepBefore(NetScope& n, __int64 network, long long whole)
{
    n.before.resize((size_t)whole);
    if (whole && !CopyWhole(network, n.before.data(), whole)) return false;
    std::sort(n.before.begin(), n.before.end());
    return true;
}

static void RestoreAll()
{
    ReleaseFn release = (ReleaseFn)(g_exeBase + GAME_RELEASE_RVA);
    for (int i = g_netCount - 1; i >= 0; i--) {
        NetScope& n = g_nets[i];
        long long count = WholeCount(n.network);
        g_whole.resize(count > 0 ? (size_t)count : 0);
        bool haveWhole = count >= 0 && (count == 0 || CopyWhole(n.network, g_whole.data(), count));
        std::sort(g_whole.begin(), g_whole.end());
        // Edges the command made (a cut's two halves, a new road): the game's
        // own frame would have them in its lists at once, and the tools pick
        // and hover through these lists - without them a piece left by a cut
        // could not be picked until the renderer filled the lists again.
        std::vector<__int64> added;
        if (haveWhole)
            for (__int64 e : g_whole)
                if (!SortedHas(n.before, e)) added.push_back(e);
        for (int k = n.swapped - 1; k >= 0; k--) {
            ListSwap& s = n.lists[k];
            __int64 cb, ce, cc;
            bool current = ReadHeader(s.header, &cb, &ce, &cc);
            // Own list back, minus edges the command removed, plus the ones it made.
            long long kept = haveWhole ? FilterOwn(s.begin, s.end) : 0;
            long long room = s.begin ? (s.cap - s.begin) / 8 : 0;
            long long total = kept + (long long)added.size();
            if (!added.empty() && total > room) {
                // A bigger buffer from the game's allocator; the old one released with it.
                size_t want = (size_t)total + LIST_SLACK;
                __int64 grown = (__int64)((AllocateFn)(g_exeBase + GAME_ALLOCATE_RVA))(want, 8);
                if (grown) {
                    if (kept) memcpy((void*)grown, (const void*)s.begin, (size_t)kept * 8);
                    if (s.begin) release((void*)s.begin, (size_t)room, 8);
                    s.begin = grown;
                    s.cap = grown + (__int64)want * 8;
                    room = (long long)want;
                }
            }
            if (total <= room) {
                if (!added.empty()) memcpy((void*)(s.begin + kept * 8), added.data(), added.size() * 8);
                kept = total;
            }
            WriteHeader(s.header, s.begin, s.begin + kept * 8, s.cap);
            // The buffer the list had in the scope: ours, or one the game
            // grew it into (then ours is already released). An unreadable
            // header leaks it rather than risk a double release.
            if (current && cb && cb != s.begin) release((void*)cb, (size_t)((cc - cb) / 8), 8);
        }
        n.swapped = 0;
        n.before.clear();
    }
    g_netCount = 0;
    g_whole.clear();
}

bool ViewListScope_Begin(float minX, float minZ, float maxX, float maxZ)
{
    if (g_depth > 0) {
        g_depth++;
        return true;
    }
    if (!g_exeBase || !TakeLock()) return false;
    float box[4] = { minX, minZ, maxX, maxZ };
    AllocateFn allocate = (AllocateFn)(g_exeBase + GAME_ALLOCATE_RVA);
    ReleaseFn release = (ReleaseFn)(g_exeBase + GAME_RELEASE_RVA);
    __int64 world = (__int64)(g_exeBase + WORLD_THIS_RVA);
    g_netCount = 0;
    bool ok = true;
    for (int c = 0; c < ROAD_SYNC_CATEGORY_COUNT && ok; c++) {
        const RoadSyncCategoryInfo* info = RoadSync_GetCategoryInfo(c);
        __int64 network = 0;
        __try {
            network = info ? *(__int64*)(world + info->managerOffset) : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            network = 0;
        }
        if (!network) continue;
        long long whole = WholeCount(network);
        if (whole < 0) { ok = false; break; }
        NetScope& n = g_nets[g_netCount++];
        n.network = network;
        n.swapped = 0;
        if (!KeepBefore(n, network, whole)) { ok = false; break; }
        static const __int64 kLists[2] = { NET_VIEW_OFFSET, NET_SECOND_OFFSET };
        long long sel = 0;
        for (int k = 0; k < 2; k++) {
            ListSwap& s = n.lists[k];
            s.header = network + kLists[k];
            if (!ReadHeader(s.header, &s.begin, &s.end, &s.cap)) { ok = false; break; }
            s.bufCount = (size_t)whole + LIST_SLACK;
            s.buf = (__int64)allocate(s.bufCount, 8);
            if (!s.buf) { ok = false; break; }
            if (k == 0) sel = SelectEdges(network, box, (__int64*)s.buf, whole);
            else memcpy((void*)s.buf, (const void*)n.lists[0].buf, (size_t)sel * 8); // same edges in both lists
            if (sel < 0) {
                release((void*)s.buf, s.bufCount, 8);
                ok = false;
                break;
            }
            WriteHeader(s.header, s.buf, s.buf + sel * 8, s.buf + (__int64)s.bufCount * 8);
            n.swapped++;
        }
    }
    if (!ok) {
        RestoreAll();
        LeaveCriticalSection(g_rendererLock);
        g_rendererLock = nullptr;
        return false;
    }
    g_depth = 1;
    return true;
}

void ViewListScope_End()
{
    if (g_depth <= 0) return;
    if (--g_depth > 0) return;
    RestoreAll();
    if (g_rendererLock) LeaveCriticalSection(g_rendererLock);
    g_rendererLock = nullptr;
}
