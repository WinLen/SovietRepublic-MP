// SPDX-License-Identifier: GPL-3.0-only
// See probe_watch.h.
#include "probe_watch.h"
#include <tlhelp32.h>
#include <string.h>

#define WORLD_RVA 0x9D4F10
#define CITIZEN_LIST_OFFSET 0x126A8
#define CIT_POS_OFFSET 0x620

// What to watch: the citizen nearest to a spot (the one standing there now),
// four of its fields (the needs), for the whole run. When the citizens that
// split first keep standing at one place, at different steps, the watch
// follows whoever is there: when the nearest citizen changes, the registers
// move to the new one. Positions are equal in both runs until the split, so
// both runs watch the same citizens. Off by default; set the spot to the
// place to examine.
#define WATCH_ON 0
// Instead of a citizen: the republic's balances (world +0x588 rubles, +0x580
// dollars), for the whole run - who changes the money, and where the runs
// part. Off by default.
#define WATCH_MONEY 0
#define SPOT_X 0.0f
#define SPOT_Z 0.0f
#define SPOT_RADIUS 12.0f

static const unsigned kOffsets[4] = { 0x18, 0xD8, 0xDC, 0xE0 };

#define MAX_HITS 65536
#define CALLERS 4
#define CONTEXT_LINES 14

struct Hit {
    unsigned step;
    unsigned citizen;            // its place in the citizen list
    unsigned char target;        // index into kOffsets
    unsigned char mainThread;
    unsigned value;
    unsigned rip;                // exe offset of the instruction after the write, 0 = outside the exe
    unsigned callers[CALLERS];   // exe offsets of return addresses, 0 = outside
};

static Hit g_hits[2][MAX_HITS];
static volatile LONG g_hitCount[2];
static volatile LONG g_run = -1;       // the run being recorded, -1 = none
static volatile LONG g_step = 0;       // the step running now
static DWORD g_mainThread = 0;
static BYTE* g_addresses[4];
static BYTE* g_citizen = nullptr;      // the one watched now
static volatile LONG g_citizenPlace = -1;
static LONG g_arms[2];
static PVOID g_handler = nullptr;

void ProbeWatch_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static unsigned ExeOffset(DWORD64 a)
{
    BYTE* p = (BYTE*)a;
    if (p < g_exeBase || p >= g_exeBase + 0x4000000) return 0;
    return (unsigned)(p - g_exeBase);
}

static void Unwind(CONTEXT c, unsigned* out)
{
    for (int i = 0; i < CALLERS; i++) out[i] = 0;
    for (int i = 0; i < CALLERS; i++) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!f) { // a leaf: the return address is at rsp
            if (!c.Rsp) return;
            c.Rip = *(DWORD64*)c.Rsp;
            c.Rsp += 8;
        } else {
            PVOID data = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
        }
        if (!c.Rip) return;
        out[i] = ExeOffset(c.Rip);
    }
}

// Set while this thread is in the handler (a nested hit is let through).
static __declspec(thread) int t_inHandler = 0;

static LONG CALLBACK OnSingleStep(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD64 bits = c->Dr6 & 0xF;
    if (!bits) return EXCEPTION_CONTINUE_SEARCH; // a real single step, not ours
    c->Dr6 = 0;
    // only the game's own accesses
    if (t_inHandler || !ExeOffset(c->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    t_inHandler = 1;
    LONG run = g_run;
    for (int t = 0; t < 4; t++) {
        if (!(bits & (1ull << t)) || run < 0) continue;
        LONG i = InterlockedIncrement(&g_hitCount[run]) - 1;
        if (i >= MAX_HITS) continue;
        Hit& h = g_hits[run][i];
        h.step = (unsigned)g_step;
        h.citizen = (unsigned)g_citizenPlace;
        h.target = (unsigned char)t;
        h.mainThread = GetCurrentThreadId() == g_mainThread;
        h.value = g_addresses[t] ? *(volatile unsigned*)g_addresses[t] : 0;
        h.rip = ExeOffset(c->Rip);
        __try {
            Unwind(*c, h.callers);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    t_inHandler = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Puts `dr7` (and the addresses) into every other thread's debug registers;
// runs on a thread of its own so the main thread can be set as well.
static DWORD64 g_dr7;
static DWORD WINAPI SetRegisters(LPVOID)
{
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD set = 0;
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = (DWORD64)g_addresses[0];
                c.Dr1 = (DWORD64)g_addresses[1];
                c.Dr2 = (DWORD64)g_addresses[2];
                c.Dr3 = (DWORD64)g_addresses[3];
                c.Dr6 = 0;
                c.Dr7 = g_dr7;
                if (SetThreadContext(th, &c)) set++;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return set;
}

static DWORD Apply(DWORD64 dr7)
{
    g_dr7 = dr7;
    HANDLE th = CreateThread(nullptr, 0, SetRegisters, nullptr, 0, nullptr);
    if (!th) return 0;
    WaitForSingleObject(th, INFINITE);
    DWORD set = 0;
    GetExitCodeThread(th, &set);
    CloseHandle(th);
    return set;
}

// The citizen nearest to the spot (within the radius), nullptr if none.
static BYTE* Nearest(LONG* place)
{
    BYTE* world = g_exeBase + WORLD_RVA;
    BYTE* best = nullptr;
    float bestD = SPOT_RADIUS * SPOT_RADIUS;
    __try {
        BYTE** begin = *(BYTE***)(world + CITIZEN_LIST_OFFSET);
        BYTE** end = *(BYTE***)(world + CITIZEN_LIST_OFFSET + 8);
        if (!begin || end < begin || end - begin > 100000) return nullptr;
        for (BYTE** p = begin; p < end; p++) {
            if (!*p) continue;
            const float* pos = (const float*)(*p + CIT_POS_OFFSET);
            float dx = pos[0] - SPOT_X, dz = pos[2] - SPOT_Z;
            float d = dx * dx + dz * dz;
            if (d < bestD) {
                bestD = d;
                best = *p;
                *place = (LONG)(p - begin);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return best;
}

// Moves the registers to `citizen` (nullptr: off).
static void Watch(int run, BYTE* citizen, LONG place)
{
    if (citizen == g_citizen) return;
    g_citizen = citizen;
    InterlockedExchange(&g_citizenPlace, place);
    DWORD64 dr7 = 0;
    for (int t = 0; t < 4; t++) {
        g_addresses[t] = citizen ? citizen + kOffsets[t] : nullptr;
        // local enable, break on writes (01), 4 bytes (11)
        if (citizen) dr7 |= (1ull << (t * 2)) | (0xDull << (16 + t * 4));
    }
    if (WATCH_MONEY && citizen) {
        // rubles and dollars, 8 bytes each (len 10)
        BYTE* world = g_exeBase + WORLD_RVA;
        g_addresses[0] = world + 0x588;
        g_addresses[1] = world + 0x580;
        g_addresses[2] = g_addresses[3] = nullptr;
        dr7 = (1ull << 0) | (0x9ull << 16) | (1ull << 2) | (0x9ull << 20);
    }
    if (!g_handler) g_handler = AddVectoredExceptionHandler(1, OnSingleStep);
    g_mainThread = GetCurrentThreadId();
    InterlockedExchange(&g_run, citizen ? run : -1);
    Apply(dr7);
    if (citizen) g_arms[run]++;
}

void ProbeWatch_Reset()
{
    if (g_citizen) Watch(0, nullptr, -1);
    g_hitCount[0] = g_hitCount[1] = 0;
    g_arms[0] = g_arms[1] = 0;
}

void ProbeWatch_Step(int run, unsigned long long step, bool last)
{
    InterlockedExchange(&g_step, (LONG)step + 1); // writes from now on belong to the next step
    if (!WATCH_ON) return;
    if (step == 0) {
        InterlockedExchange(&g_hitCount[run], 0);
        g_arms[run] = 0;
    }
    if (last) {
        Watch(run, nullptr, -1);
        Logf("republicmp  PROBE WATCH: run %d: %ld write(s), %ld citizen(s) watched at (%.0f, %.0f)", run + 1,
             g_hitCount[run], g_arms[run], SPOT_X, SPOT_Z);
        return;
    }
    LONG place = -1;
    BYTE* c = WATCH_MONEY ? g_exeBase + WORLD_RVA : Nearest(&place);
    Watch(run, c, place);
}

static void LogHit(int run, LONG i)
{
    const Hit& h = g_hits[run][i];
    float f;
    memcpy(&f, &h.value, 4);
    unsigned offset = WATCH_MONEY ? (h.target ? 0x580 : 0x588) : kOffsets[h.target];
    Logf("republicmp  PROBE WATCH:   run %d #%ld step %u citizen #%u +0x%X = %08x (%.9g) at %X <- %X <- %X <- %X <- %X%s",
         run + 1, i, h.step, h.citizen, offset, h.value, f, h.rip, h.callers[0], h.callers[1],
         h.callers[2], h.callers[3], h.mainThread ? "" : " (other thread)");
}

static bool Same(const Hit& a, const Hit& b)
{
    return a.step == b.step && a.citizen == b.citizen && a.target == b.target && a.value == b.value && a.rip == b.rip;
}

void ProbeWatch_Finish()
{
    if (g_citizen) Watch(0, nullptr, -1);
    LONG n0 = g_hitCount[0] < MAX_HITS ? g_hitCount[0] : MAX_HITS;
    LONG n1 = g_hitCount[1] < MAX_HITS ? g_hitCount[1] : MAX_HITS;
    Logf("republicmp  PROBE WATCH: %ld / %ld write(s) recorded (run 1 / run 2)", g_hitCount[0], g_hitCount[1]);
    if (!n0 || !n1) return;
    // the writers seen, once each
    for (int run = 0; run < 2; run++) {
        LONG n = run ? n1 : n0;
        int shown = 0;
        for (LONG i = 0; i < n && shown < 40; i++) {
            bool seen = false;
            for (LONG j = 0; j < i && !seen; j++)
                seen = g_hits[run][j].target == g_hits[run][i].target && g_hits[run][j].rip == g_hits[run][i].rip &&
                       g_hits[run][j].callers[0] == g_hits[run][i].callers[0];
            if (seen) continue;
            if (!shown) Logf("republicmp  PROBE WATCH: run %d writers (first write of each):", run + 1);
            LogHit(run, i);
            shown++;
        }
    }
    // the first place the sequences part
    LONG i = 0, n = n0 < n1 ? n0 : n1;
    while (i < n && Same(g_hits[0][i], g_hits[1][i])) i++;
    if (i == n && n0 == n1) {
        Logf("republicmp  PROBE WATCH: the same writes in both runs");
        return;
    }
    Logf("republicmp  PROBE WATCH: the runs part at write #%ld:", i);
    LONG from = i > 6 ? i - 6 : 0;
    for (int run = 0; run < 2; run++) {
        LONG nr = run ? n1 : n0;
        for (LONG k = from; k < nr && k < from + CONTEXT_LINES; k++) LogHit(run, k);
    }
}
