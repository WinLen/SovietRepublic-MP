// SPDX-License-Identifier: GPL-3.0-only
// See purchase.h. The capture, the checks and the replay follow SoylentGreed's
// purchase replay (RepublicLAN, read against the disassembly of 1.1.1.9):
// window kinds 1/2 into a depot or from the used-vehicle market, 5/6 licences
// and 7 a factory's blueprint. What is not carried yet (category 9, trailer
// depots, realistic purchases of categories 6/8/10 into a depot that is not a
// customs house, kinds 9..13) is refused on the clicking PC.
#include "purchase.h"
#include "commands.h"
#include "world_refs.h"
#include "../core/safe_mem.h"
#include "../session/two_states.h"
#include "../ui/lang.h"
#include <math.h>
#include <string.h>
#include <vector>

void Purchase_Bind(const TsmHost* host)
{
    TsmBind(host);
}

#define WORLD_RVA 0x9D4F10
#define CAMPAIGN_RVA 0x9E8088          // != -1: a campaign
#define PRESS_RETURN_RVA 0x810949      // after the relayed call at 810943
#define RANDOM_INT_IMPORT 0x86CA70     // C3DRandom_Int
#define BUILDINGS_OFFSET 0x11B08       // world building vector
#define VEHICLES_OFFSET 0x12810        // world vehicle vector
#define TYPES_OFFSET 0x12840           // vehicle types, 0x9f08 each
#define TYPE_STRIDE 0x9F08
#define TYPE_MAX 4096
#define REALISTIC_OFFSET 0x14452
#define FREE_OFFSET 0x578
#define RUBLES_OFFSET 0x588
#define DOLLARS_OFFSET 0x580
#define TOTAL_DOLLARS_OFFSET 0x114B0 // month totals, floats
#define TOTAL_RUBLES_OFFSET 0x114B4
#define MARKET_OFFSET 0x50             // used-vehicle market: ten vectors of 0x28-byte records
#define USED_TARGET 14
#define USED_RECORD 0x28
#define VARIANT_COUNT 0x20
#define MAX_TRAILERS 16
#define WINDOW_BYTES 0x600

// The routines the replay calls; narrow arguments travel in full slots, as the branch passes them.
typedef unsigned char (*SlotFn)(BYTE* world, BYTE* depot, BYTE* type, unsigned __int64 zero, int* slot);              // 3e3f40
typedef unsigned char (*CampaignFn)(BYTE* world, BYTE* depot, BYTE* typeName);                                         // 627e80
typedef void (*RefillFn)(BYTE* world, BYTE* window);                                                                   // 76a170
typedef BYTE* (*CreateFn)(BYTE* world, BYTE* type, BYTE* building, unsigned __int64 one, unsigned __int64 index,
                          unsigned __int64 b28, unsigned __int64 b30, unsigned __int64 slot, unsigned __int64 b40,
                          unsigned __int64 b48, unsigned __int64 b50);                                                   // 3e4a00
typedef void (*CoupleFn)(BYTE* world, BYTE* vehicle, BYTE* trailer, unsigned __int64 minusOne, unsigned __int64 b20,
                         unsigned __int64 b28, unsigned __int64 q30);                                                    // 3fd5d0
typedef void (*AddToSlotFn)(BYTE* list, BYTE* vehicle);                                                                 // 33f0
typedef void (*SlotDoneFn)(BYTE* world, BYTE* depot, BYTE* slotRecord);                                                // 2ab810
typedef void (*PushFn)(BYTE* vector, BYTE* const* value);                                                               // b0e90
typedef void (*LoadTypeFn)(BYTE* world, BYTE* type);                                                                    // 3ea7d0
typedef void (*LoadMaterialFn)(BYTE* unused, BYTE* type, unsigned __int64 variant);                                     // 3ea250
typedef void (*GrowFn)(BYTE* vector);                                                                                   // cc880
typedef int (*RandomIntFn)(int range);                                                                                  // C3DRandom_Int
typedef void (*UsedRemoveFn)(BYTE* market, BYTE* type);                                                                 // 6624d0
typedef void (*UsedApplyFn)(BYTE* record, BYTE* vehicle);                                                               // 660a70

static const TsmRoutineSig kRoutines[] = {
    { 0x3E3F40, { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec }, 16 },
    { 0x627E80, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 }, 16 },
    { 0x76A170, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xee, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5c, 0x24, 0x20, 0x57, 0x48 }, 16 },
    { 0x3E4A00, { 0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec, 0x48 }, 16 },
    { 0x3FD5D0, { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48 }, 16 },
    { 0x0033F0, { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xf1 }, 16 },
    { 0x2AB810, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xef, 0x05, 0x00, 0x00, 0x4c, 0x8b, 0xdc, 0x4d, 0x89, 0x43, 0x18 }, 16 },
    { 0x0B0E90, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x41, 0x08, 0x48, 0x8b }, 16 },
    { 0x3EA7D0, { 0x4c, 0x8b, 0xdc, 0x55, 0x57, 0x49, 0x8d, 0xab, 0x68, 0xff, 0xff, 0xff, 0x48, 0x81, 0xec, 0x88 }, 16 },
    { 0x3EA250, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48 }, 16 },
    { 0x0CC880, { 0x48, 0x83, 0xec, 0x28, 0x4c, 0x8b, 0x49, 0x10, 0x4c, 0x8b, 0xc1, 0x48, 0x8b, 0x51, 0x08, 0x49 }, 16 },
    { 0x6624D0, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0x26, 0x01, 0x00, 0x00, 0x57, 0x48, 0x83, 0xec, 0x20, 0x66, 0x0f }, 16 },
    { 0x660A70, { 0x48, 0x85, 0xd2, 0x0f, 0x84, 0xde, 0x00, 0x00, 0x00, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b }, 16 },
};

static int g_routinesOk = -1; // -1 not checked yet

static bool RoutinesOk()
{
    return Ref_CheckRoutineTable(&g_routinesOk, kRoutines, sizeof(kRoutines) / sizeof(kRoutines[0]), "PURCHASE");
}

// ---------------------------------------------------------------------------
// Identities the same on every PC
// ---------------------------------------------------------------------------
//
// In lockstep the building list is the same on every PC: a building travels as
// its place in the list and its id (+0).

struct BuildingRef {
    int place; // -1: none
    int id;
};

static BuildingRef RefOf(BYTE* world, BYTE* building)
{
    BuildingRef r = { -1, 0 };
    if (!building) return r;
    BYTE** begin = *(BYTE***)(world + BUILDINGS_OFFSET);
    BYTE** end = *(BYTE***)(world + BUILDINGS_OFFSET + 8);
    for (BYTE** it = begin; it && it < end; ++it) {
        if (*it == building) {
            r.place = (int)(it - begin);
            r.id = *(int*)building;
            break;
        }
    }
    return r;
}

static BYTE* BuildingAt(BYTE* world, const BuildingRef& r)
{
    if (r.place < 0) return nullptr;
    BYTE** begin = *(BYTE***)(world + BUILDINGS_OFFSET);
    BYTE** end = *(BYTE***)(world + BUILDINGS_OFFSET + 8);
    if (!begin || begin + r.place >= end) return nullptr;
    BYTE* b = begin[r.place];
    return b && *(int*)b == r.id ? b : nullptr;
}

static BYTE* TypeAt(BYTE* world, unsigned index)
{
    BYTE* begin = *(BYTE**)(world + TYPES_OFFSET);
    BYTE* end = *(BYTE**)(world + TYPES_OFFSET + 8);
    if (!begin || end < begin || (unsigned __int64)(end - begin) / TYPE_STRIDE <= index) return nullptr;
    return begin + (unsigned __int64)index * TYPE_STRIDE;
}

// FNV of the type's name (+0x200) up to its end: the buffer after the name
// holds whatever each PC's heap had there.
static unsigned TypeCheck(BYTE* type)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < 64 && type[0x200 + i]; i++) {
        h ^= type[0x200 + i];
        h *= 16777619u;
    }
    return h | 1u;
}

// The type at the index, or the one with the same name elsewhere in this PC's list.
static BYTE* TypeFor(BYTE* world, unsigned index, unsigned check)
{
    BYTE* type = TypeAt(world, index);
    if (type && TypeCheck(type) == check) return type;
    BYTE* begin = *(BYTE**)(world + TYPES_OFFSET);
    BYTE* end = *(BYTE**)(world + TYPES_OFFSET + 8);
    for (BYTE* t = begin; t && t + TYPE_STRIDE <= end; t += TYPE_STRIDE)
        if (TypeCheck(t) == check) return t;
    return nullptr;
}

// The used-vehicle market's list for a type: categories 1, 4, 6 and 8/10, the
// second five when the type's +85bc is above zero; others have none.
static unsigned UsedListOffset(unsigned cat, bool second)
{
    if (cat == 1) return second ? 0x60 : 0x00;
    if (cat == 4) return second ? 0x78 : 0x18;
    if (cat == 6) return second ? 0x90 : 0x30;
    if (cat == 8 || cat == 10) return second ? 0xA8 : 0x48;
    return 0xFFFFFFFFu;
}

// The first record of the type in this PC's market (as the window finds it).
static bool UsedFind(BYTE* world, BYTE* type, double* age, double* age2)
{
    *age = *age2 = 0;
    BYTE* market = *(BYTE**)(world + MARKET_OFFSET);
    if (!market) return false;
    unsigned off = UsedListOffset(*(unsigned*)(type + 0x294), *(int*)(type + 0x85BC) > 0);
    if (off == 0xFFFFFFFFu) return false;
    BYTE* it = *(BYTE**)(market + off);
    BYTE* end = *(BYTE**)(market + off + 8);
    for (; it && it < end; it += USED_RECORD) {
        if (*(BYTE**)it != type) continue;
        *age = *(double*)(it + 8);
        *age2 = *(double*)(it + 16);
        return true;
    }
    return false;
}

static BYTE* ResourceAt(BYTE* world, int idx)
{
    if (idx < 0) return nullptr;
    BYTE* cbeg = *(BYTE**)(world + 0xC2B0);
    BYTE* cend = *(BYTE**)(world + 0xC2B8);
    if (!cbeg || cend <= cbeg || (size_t)idx >= (size_t)(cend - cbeg) / 0x340) return nullptr;
    return cbeg + (size_t)idx * 0x340;
}

// ---------------------------------------------------------------------------
// The command
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct PurchaseMsg {
    unsigned kind;        // window kind 1/2 (rubles/dollars), 5/6 licence, 7 blueprint
    unsigned typeIndex;   // in world +12840
    unsigned typeCheck;   // FNV of the type's name
    unsigned char refill; // realistic: the window asked for its customs houses again
    unsigned char used;   // from the used-vehicle market
    unsigned char pad[2];
    BuildingRef depot;    // depot or factory
    BuildingRef customsRub, customsUsd;
    double rubles, dollars;
    float avail;
    int variant;
    int resIndex;         // category 9 resource index in world +c2b0 (or -1)
    unsigned trailers;
    int trailerVariant[MAX_TRAILERS];
    double usedAge, usedAge2;
};
#pragma pack(pop)

static bool Finite(double v)
{
    return isfinite(v) && v >= 0 && v < 1e15;
}

// A type's variant and its trailers' types' variants (type +7c08, 0x110 each, type at +100).
static bool ReadVariants(BYTE* type, PurchaseMsg& m)
{
    m.variant = *(int*)(type + 0x664C);
    BYTE* v = *(BYTE**)(type + 0x7C08);
    BYTE* e = *(BYTE**)(type + 0x7C10);
    if (m.variant < 0 || m.variant >= VARIANT_COUNT || e < v || (e - v) % 0x110) return false;
    unsigned n = v ? (unsigned)((e - v) / 0x110) : 0;
    if (n > MAX_TRAILERS) return false;
    for (unsigned i = 0; i < n; i++) {
        BYTE* t = *(BYTE**)(v + i * 0x110 + 0x100);
        if (!t) return false;
        int x = *(int*)(t + 0x664C);
        if (x < 0 || x >= VARIANT_COUNT) return false;
        m.trailerVariant[i] = x;
    }
    m.trailers = n;
    return true;
}

static bool WriteVariants(BYTE* type, const PurchaseMsg& m)
{
    BYTE* v = *(BYTE**)(type + 0x7C08);
    BYTE* e = *(BYTE**)(type + 0x7C10);
    if (e < v || (v ? (unsigned)((e - v) / 0x110) : 0) != m.trailers) return false;
    *(int*)(type + 0x664C) = m.variant;
    for (unsigned i = 0; i < m.trailers; i++) *(int*)(*(BYTE**)(v + i * 0x110 + 0x100) + 0x664C) = m.trailerVariant[i];
    return true;
}

// The press: "pass" (not a purchase), "take" (made a command), or a refusal.
enum PressOutcome { PRESS_PASS, PRESS_TAKE, PRESS_REFUSE };

static PressOutcome Capture(const CONTEXT* c, PurchaseMsg& m, const char** why)
{
    BYTE* world = g_exeBase + WORLD_RVA;
    BYTE* type = (BYTE*)c->R13;
    BYTE* window = (BYTE*)c->R14;
    memset(&m, 0, sizeof(m));
    m.resIndex = -1;
    *why = "";
    __try {
        if ((BYTE*)c->R12 != world || !window) {
            *why = "registers";
            return PRESS_REFUSE;
        }
        unsigned char hover = window[0x20];
        unsigned kind = *(unsigned*)(window + 0x23C);
        unsigned target = *(unsigned*)(window + 0x248);
        BYTE* depot = *(BYTE**)(window + 0x240);
        if (!hover || (c->Rbx & 0xFF)) return PRESS_PASS; // not on the Buy button
        // Catalog browsers (9/10) and selection modes (0xb picks an auto-replacement
        // type, 0xc/0xd add a type to a stop half) are not vehicle/blueprint purchases.
        if (kind == 9 || kind == 10 || kind == 0xB || kind == 0xC || kind == 0xD) return PRESS_PASS;
        if (kind != 1 && kind != 2 && kind != 5 && kind != 6 && kind != 7) {
            *why = "window kind";
            return PRESS_REFUSE;
        }
        if (target != 0 && !(target == USED_TARGET && (kind == 1 || kind == 2))) {
            *why = "target kind";
            return PRESS_REFUSE;
        }
        if (!depot) {
            *why = "no depot";
            return PRESS_REFUSE;
        }
        BYTE* types = *(BYTE**)(world + TYPES_OFFSET);
        BYTE* typesEnd = *(BYTE**)(world + TYPES_OFFSET + 8);
        if (!types || type < types || type >= typesEnd || (type - types) % TYPE_STRIDE) {
            *why = "type";
            return PRESS_REFUSE;
        }
        double dollars, rubles;
        float avail;
        memcpy(&rubles, &c->Xmm12, 8);  // taken from +588
        memcpy(&dollars, &c->Xmm13, 8); // taken from +580
        memcpy(&avail, &c->Xmm10, 4);
        if (!Finite(dollars) || !Finite(rubles) || !isfinite(avail)) {
            *why = "prices";
            return PRESS_REFUSE;
        }
        // The customs-house refill works on the root window (parents through +5c8).
        BYTE* root = window;
        for (int i = 0; i < 16; i++) {
            BYTE* up = *(BYTE**)(root + 0x5C8);
            if (!up) break;
            root = up;
        }
        m.kind = kind;
        m.typeIndex = (unsigned)((type - types) / TYPE_STRIDE);
        m.typeCheck = TypeCheck(type);
        m.refill = window[0x2B8] ? 1 : 0;
        m.depot = RefOf(world, depot);
        m.customsRub = RefOf(world, *(BYTE**)(root + 0x2C0));
        m.customsUsd = RefOf(world, *(BYTE**)(root + 0x2C8));
        m.rubles = rubles;
        m.dollars = dollars;
        m.avail = avail;
        if (*(unsigned*)(type + 0x294) == 9) {
            int row = *(int*)(c->Rbp - 0x78);
            BYTE** rbeg = *(BYTE***)(world + 0x12870);
            BYTE** rend = *(BYTE***)(world + 0x12878);
            if (row >= 0 && rbeg && row < (int)(rend - rbeg)) {
                BYTE* res = rbeg[row];
                BYTE* cbeg = *(BYTE**)(world + 0xC2B0);
                BYTE* cend = *(BYTE**)(world + 0xC2B8);
                if (res && cbeg && res >= cbeg && res < cend && (res - cbeg) % 0x340 == 0)
                    m.resIndex = (int)((res - cbeg) / 0x340);
            }
        }
        if (m.depot.place < 0) {
            *why = "depot not in the building list";
            return PRESS_REFUSE;
        }
        if (!ReadVariants(type, m)) {
            *why = "variants";
            return PRESS_REFUSE;
        }
        if (target == USED_TARGET) {
            m.used = 1;
            UsedFind(world, type, &m.usedAge, &m.usedAge2);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *why = "fault";
        return PRESS_REFUSE;
    }
    return PRESS_TAKE;
}

bool Purchase_OnPress(bool pressed, const CONTEXT* caller)
{
    if (!pressed || !Commands_TakeLocal()) return pressed;
    if (caller->Rip != (DWORD64)(g_exeBase + PRESS_RETURN_RVA)) return pressed;
    PurchaseMsg m;
    const char* why = "";
    PressOutcome o = Capture(caller, m, &why);
    if (o == PRESS_PASS) return pressed;
    if (o == PRESS_TAKE && !RoutinesOk()) {
        o = PRESS_REFUSE;
        why = "game routines differ";
    }
    if (o == PRESS_TAKE) {
        __try {
            BYTE* depot = BuildingAt(g_exeBase + WORLD_RVA, m.depot);
            if (depot) {
                float x = *(float*)(depot + 0x350), z = *(float*)(depot + 0x358);
                if (!TwoStates_CheckBuildingControlAllowed(m.depot.id, x, z, Tr("покупка техники/лицензии в чужом здании", "buying vehicle/licence in a foreign building")))
                    return false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (o == PRESS_REFUSE) {
        Logf("republicmp  PURCHASE: a purchase was refused in lockstep (%s)", why);
        return false;
    }
    Commands_Submit(LS_CMD_PURCHASE, &m, sizeof(m));
    return false;
}

// ---------------------------------------------------------------------------
// The replay (every PC, at the boundary)
// ---------------------------------------------------------------------------

static bool Banned(BYTE* world, unsigned kind, unsigned cat)
{
    auto flag = [&](unsigned o) { return world[o] != 0; };
    if (kind == 2) {
        if (cat - 1 <= 1) return !flag(0x136A7);
        if (cat - 3 <= 2) return !flag(0x136A9);
        if (cat == 6) return !flag(0x136AB);
        if (cat == 7) return !flag(0x136AE);
        if (cat == 8 || cat == 10) return !flag(0x136AD);
        return false;
    }
    if (kind == 1) {
        if (cat - 1 <= 1) return !flag(0x136A6);
        if (cat - 3 <= 2) return !flag(0x136A8);
        if (cat == 6) return !flag(0x136AA);
        if (cat == 7) return !flag(0x136AF);
        if (cat == 8 || cat == 10) return !flag(0x136AC);
    }
    return false;
}

static bool CanPay(BYTE* world, double rubles, double dollars)
{
    if (*(int*)(world + FREE_OFFSET)) return true;
    if (rubles > *(double*)(world + RUBLES_OFFSET) && rubles > 0) return false;
    if (dollars > *(double*)(world + DOLLARS_OFFSET) && dollars > 0) return false;
    return true;
}

static void Pay(BYTE* world, double rubles, double dollars, BYTE* type = nullptr, BYTE* res = nullptr)
{
    if (!*(int*)(world + FREE_OFFSET)) {
        *(double*)(world + RUBLES_OFFSET) -= rubles;
        *(double*)(world + DOLLARS_OFFSET) -= dollars;
    }
    if (type && *(unsigned*)(type + 0x294) == 9 && res) {
        typedef void (*LedgerFn)(BYTE* world, void* row, BYTE* acct, float avail, unsigned char b5, float mult, unsigned char b7);
        LedgerFn ledger = (LedgerFn)(g_exeBase + 0x2FDB00);
        float amount = *(float*)(res + ((__int64)*(int*)(type + 0x8600) << 5) + 0xCC) * *(float*)(type + 0x8604);
        struct {
            BYTE* res;
            float amount;
            unsigned zero;
        } lrow = { res, amount, 0 };
        if (dollars != 0.0) ledger(world, &lrow, world + 0x11330, 0.0f, 0, 1.0f, 0);
        if (rubles != 0.0) ledger(world, &lrow, world + 0x11360, 0.0f, 0, 1.0f, 0);
        return;
    }
    float& tr = *(float*)(world + TOTAL_RUBLES_OFFSET);
    tr = (float)((double)tr + rubles);
    float& td = *(float*)(world + TOTAL_DOLLARS_OFFSET);
    td = (float)((double)td + dollars);
}

// Kinds 5/6 (licence) and 7 (a factory's blueprint).
static const char* ReplayBlueprint(BYTE* world, const PurchaseMsg& m, BYTE* type, BYTE* depot, int campaign)
{
    bool licence = m.kind != 7;
    BYTE* btype = nullptr;
    if (!licence) {
        btype = *(BYTE**)(depot + 0x318);
        if (!btype) return "not carried (factory)";
    }
    if (licence && type[0x9C88]) return "refused (licence held)";
    bool fail = !CanPay(world, m.rubles, m.dollars);
    if (campaign != -1) fail = ((CampaignFn)(g_exeBase + 0x627E80))(world, depot, type + 0x200) != 0;
    if (fail) return "refused (money or campaign)";
    Pay(world, m.rubles, m.dollars, type, ResourceAt(world, m.resIndex));
    if (licence) {
        type[0x9C88] = 1;
        if (campaign != -1 && !strcmp((const char*)(world + 0x13348), "BuyVehicle")) world[0x13651] = 1;
        return nullptr;
    }
    int def = *(int*)(type + 0x664C);
    RandomIntFn random = *(RandomIntFn*)(g_exeBase + RANDOM_INT_IMPORT);
    auto draw = [&]() { return depot[0x1271] ? random(*(int*)(type + 0x6648)) : def; };
    LoadTypeFn loadType = (LoadTypeFn)(g_exeBase + 0x3EA7D0);
    LoadMaterialFn loadMaterial = (LoadMaterialFn)(g_exeBase + 0x3EA250);
    if (*(BYTE**)(depot + 0x11C8) && *(int*)(btype + 0x360) == 0x28) {
        BYTE* vec = depot + 0x11D8;
        BYTE*& end = *(BYTE**)(vec + 8);
        if (end - *(BYTE**)vec >= 0x10 && *(int*)(end - 8) == def && *(BYTE**)(end - 0x10) == type) {
            ++*(int*)(end - 4);
        } else {
            int variant = draw();
            if (end == *(BYTE**)(vec + 0x10)) ((GrowFn)(g_exeBase + 0xCC880))(vec);
            BYTE* e = end;
            if (e) {
                *(BYTE**)e = type;
                *(int*)(e + 8) = variant;
                *(int*)(e + 12) = 1;
            }
            end += 0x10;
            loadType(world, type);
            loadMaterial(world, type, (unsigned)def);
        }
    } else {
        *(int*)(depot + 0xFC0) = 0;
        *(BYTE**)(depot + 0x11C8) = type;
        *(int*)(depot + 0x11D0) = draw();
        loadType(world, type);
        loadMaterial(world, type, (unsigned)*(int*)(type + 0x664C));
    }
    if (campaign != -1 && !strcmp((const char*)(world + 0x13348), "BuildVehicle") &&
        !strcmp((const char*)(type + 0x200), (const char*)(world + 0x132B4)))
        world[0x13651] = 1;
    return nullptr;
}

static BYTE* LastVehicle(BYTE* world)
{
    BYTE** b = *(BYTE***)(world + VEHICLES_OFFSET);
    BYTE** e = *(BYTE***)(world + VEHICLES_OFFSET + 8);
    return e > b ? e[-1] : nullptr;
}

static void ApplyUsedMarketStep(BYTE* market, BYTE* type, const PurchaseMsg& m, BYTE* v)
{
    alignas(8) BYTE rec[USED_RECORD] = {};
    memcpy(rec, &type, 8);
    memcpy(rec + 8, &m.usedAge, 8);
    memcpy(rec + 16, &m.usedAge2, 8);
    ((UsedRemoveFn)(g_exeBase + 0x6624D0))(market, type);
    ((UsedApplyFn)(g_exeBase + 0x660A70))(rec, v);
}

static void SpawnTrailersInSlot(BYTE* world, BYTE* type, BYTE* depot, BYTE* leadVehicle, __int64 trailers, unsigned slot)
{
    CreateFn create = (CreateFn)(g_exeBase + 0x3E4A00);
    CoupleFn couple = (CoupleFn)(g_exeBase + 0x3FD5D0);
    const unsigned __int64 minusOne = 0xFFFFFFFFu;
    for (__int64 i = 1; i <= trailers; i++) {
        BYTE* t = *(BYTE**)(type + 0x7C08) + (i - 1) * 0x110;
        create(world, *(BYTE**)(t + 0x100), depot, 1, (unsigned)i, t[0x108], 0, slot, 0, 1, 0);
        couple(world, leadVehicle, LastVehicle(world), minusOne, 0, 1, 0);
    }
}

static void PlaceRealisticVehicleAtBorder(BYTE* world, const PurchaseMsg& m, BYTE* depot, BYTE* v, unsigned vcat)
{
    PushFn push = (PushFn)(g_exeBase + 0xB0E90);
    struct Vec3 { float x, y, z; };
    struct Vec2 { float x, z; };
    typedef Vec3* (*GetBldPosFn)(BYTE* bld320, Vec3* out);
    typedef Vec3* (*FindBorderFn)(BYTE* world, Vec3* out, const Vec3* depotPos, int kind, unsigned char usd);
    typedef Vec2* (*Vec3To2Fn)(Vec2* out, const Vec3* in);
    typedef void (*PlaceAtBorderFn)(BYTE* v, const Vec2* pos2, float a3, int a4, float y, float a6);
    typedef void (*InitBorderFn)(BYTE* v);
    typedef void (*PushVec3Fn)(BYTE* vec, const Vec3* p);
    typedef void (*SendToDepotFn)(BYTE* v, BYTE* depot);
    typedef void (*StartBorderMoveFn)(BYTE* v, int a2, int a3, int a4);
    Vec3 depotPos = {}, borderPos = {};
    (*(GetBldPosFn*)(g_exeBase + 0x86C7B8))(depot + 0x320, &depotPos);
    ((FindBorderFn)(g_exeBase + 0x42B9B0))(world, &borderPos, &depotPos, 0xA, m.kind == 2 ? 1 : 0);
    if (vcat == 7 || vcat == 8 || vcat == 10) *(int*)(v + 0x1A04) = 2;
    Vec2 pos2 = {};
    (*(Vec3To2Fn*)(g_exeBase + 0x86C010))(&pos2, &borderPos);
    ((PlaceAtBorderFn)(g_exeBase + 0x6AD960))(v, &pos2, 0.0f, 0, borderPos.y, 0.0f);
    ((InitBorderFn)(g_exeBase + 0x6AD290))(v);
    if (vcat == 6) {
        *(int*)(v + 0x1794) = (m.kind == 2) ? -1 : -2;
        *(float*)(v + 0x1A3C) = *(float*)(*(BYTE**)(v + 0x1708) + 0x7A78);
    } else {
        ((PushVec3Fn)(g_exeBase + 0x02F2F0))(v + 0x738, &depotPos);
        if (vcat == 8) {
            BYTE* nullPtr = nullptr;
            push(v + 0x6A0, &nullPtr);
            push(v + 0x6A0, &nullPtr);
        }
    }
    ((SendToDepotFn)(g_exeBase + 0x6B7070))(v, depot);
    ((StartBorderMoveFn)(g_exeBase + 0x6B8EB0))(v, 0, 0, 0);
}

static void RouteRealisticVehicleDelivery(BYTE* world, const PurchaseMsg& m, BYTE* depot, BYTE* customs, int slot, BYTE* v)
{
    PushFn push = (PushFn)(g_exeBase + 0xB0E90);
    AddToSlotFn addToSlot = (AddToSlotFn)(g_exeBase + 0x33F0);
    unsigned vcat = *(unsigned*)(*(BYTE**)(v + 0x1708) + 0x294);
    if (vcat == 1 || vcat - 4 <= 1) {
        push(customs + 0xC70, &v);
        if (vcat == 4 || vcat == 5) {
            BYTE** a = *(BYTE***)(v + 0x3A0);
            BYTE** e = *(BYTE***)(v + 0x3A8);
            for (; a && a < e; ++a) push(customs + 0xC70, a);
        }
        *(int*)(v + 0x1794) = slot + 2;
        *(BYTE**)(v + 0x4F0) = *(BYTE**)(v + 0x4F8) = customs;
        *(BYTE**)(v + 0x600) = depot;
        BYTE* customsSlotList = *(BYTE**)(*(BYTE**)(customs + 0x970) + (__int64)slot * 0xE0 + 0xC0);
        addToSlot(customsSlotList, v);
    } else if (vcat == 6 || vcat == 7 || vcat == 8 || vcat == 10) {
        PlaceRealisticVehicleAtBorder(world, m, depot, v, vcat);
    }
}

static const char* ValidateVehiclePurchase(BYTE* world, const PurchaseMsg& m, BYTE* type, BYTE* depot,
                                           bool realistic, bool customsDepot, unsigned cat, BYTE* win,
                                           BYTE* customs, int campaign, int& slot, BYTE*& market)
{
    SlotFn slotOf = (SlotFn)(g_exeBase + 0x3E3F40);
    if (m.used) {
        market = *(BYTE**)(world + MARKET_OFFSET);
        if (!market) return "not carried (no used market)";
        if (!customsDepot && realistic && (cat == 1 || cat - 4 <= 1) && !customs) return "not carried (used, no customs house)";
    }
    const char* fail = nullptr;
    if (!slotOf(world, depot, type, 0, &slot)) fail = "refused (depot full)";
    if (!m.used && realistic && (cat == 1 || cat - 4 <= 1) && !customsDepot) {
        if (!*(BYTE**)(win + 0x2C0) && !*(BYTE**)(win + 0x2C8)) {
            if (!fail) fail = "refused (no customs house)";
        } else if (m.avail >= *(float*)(type + 0x8624)) {
            if (!fail) fail = "refused (not available)";
        } else if (!fail && (!customs || !slotOf(world, customs, type, 0, &slot) || slot < 0)) {
            fail = "refused (customs house full)";
        }
    }
    if (!CanPay(world, m.rubles, m.dollars) && !fail) fail = "refused (money)";
    if (campaign != -1) fail = ((CampaignFn)(g_exeBase + 0x627E80))(world, depot, type + 0x200) ? "refused (campaign)" : nullptr;
    if (fail) return fail;
    if (Banned(world, m.kind, cat)) return "refused (banned)";
    return nullptr;
}

// Kinds 1/2: a vehicle into a depot (810951..812f46).
static const char* ReplayVehicle(BYTE* world, const PurchaseMsg& m, BYTE* type, BYTE* depot, BYTE* customsRub,
                                 BYTE* customsUsd, int campaign)
{
    CreateFn create = (CreateFn)(g_exeBase + 0x3E4A00);
    CoupleFn couple = (CoupleFn)(g_exeBase + 0x3FD5D0);
    AddToSlotFn addToSlot = (AddToSlotFn)(g_exeBase + 0x33F0);
    SlotDoneFn slotDone = (SlotDoneFn)(g_exeBase + 0x2AB810);
    unsigned cat = *(unsigned*)(type + 0x294);
    BYTE* dtype = *(BYTE**)(depot + 0x318);
    unsigned dkind = dtype ? *(unsigned*)(dtype + 0x360) : 0xFFFFFFFFu;
    bool realistic = world[REALISTIC_OFFSET] != 0;
    bool customsDepot = dtype && dkind == 0x14;
    alignas(16) BYTE win[WINDOW_BYTES] = {};
    *(BYTE**)(win + 0x240) = depot;
    *(unsigned*)(win + 0x23C) = m.kind;
    win[0x2B8] = m.refill;
    *(BYTE**)(win + 0x2C0) = customsRub;
    *(BYTE**)(win + 0x2C8) = customsUsd;
    BYTE* customs = nullptr;
    if (realistic) {
        if (win[0x2B8]) ((RefillFn)(g_exeBase + 0x76A170))(world, win);
        customs = m.kind == 2 ? *(BYTE**)(win + 0x2C0) : *(BYTE**)(win + 0x2C8);
    }
    int slot = -1;
    BYTE* market = nullptr;
    if (const char* err = ValidateVehiclePurchase(world, m, type, depot, realistic, customsDepot, cat, win, customs, campaign, slot, market))
        return err;
    Pay(world, m.rubles, m.dollars, type, ResourceAt(world, m.resIndex));
    __int64 trailers = (*(BYTE**)(type + 0x7C10) - *(BYTE**)(type + 0x7C08)) / 0x110;
    const unsigned __int64 minusOne = 0xFFFFFFFFu;
    auto slotList = [&](BYTE* building) { return *(BYTE**)(*(BYTE**)(building + 0x970) + (__int64)slot * 0xE0 + 0xC0); };
    if ((dtype && dkind == 0x34 && cat == 3) || customsDepot) {
        if (slot < 0) return "refused (no slot)";
        if (customsDepot && trailers == 0 && cat == 9) {
            BYTE* res = ResourceAt(world, m.resIndex);
            if (res) {
                float amount = *(float*)(res + ((__int64)*(int*)(type + 0x8600) << 5) + 0xCC) * *(float*)(type + 0x8604);
                if (amount != 0.0f) {
                    struct { BYTE* res; float amount; unsigned zero; } imp = { res, amount, 0 };
                    typedef void (*ImportContainerFn)(BYTE*, BYTE*, void*, unsigned char, BYTE*);
                    ((ImportContainerFn)(g_exeBase + 0x1CAF10))(world, depot, &imp, 0, type);
                }
            }
            slotDone(world, depot, *(BYTE**)(depot + 0x970) + (__int64)slot * 0xE0);
            return nullptr;
        }
        BYTE* v = create(world, type, depot, 1, trailers ? 0 : minusOne, 0, 0, (unsigned)slot, 0, 1, 0);
        if (!v) v = LastVehicle(world);
        if (!v) return "no vehicle created";
        addToSlot(slotList(depot), v);
        SpawnTrailersInSlot(world, type, depot, v, trailers, (unsigned)slot);
        if (customsDepot && m.used && v) ApplyUsedMarketStep(market, type, m, v);
        slotDone(world, depot, *(BYTE**)(depot + 0x970) + (__int64)slot * 0xE0);
        return nullptr;
    }
    if (realistic && (cat == 1 || cat - 4 <= 1) && slot < 0) return "refused (no slot)";
    BYTE* v = create(world, type, depot, 1, trailers ? 0 : minusOne, 0, 0, minusOne, 0, 1, 0);
    if (trailers) {
        if (realistic) *(BYTE**)(v + 0x4F0) = *(BYTE**)(v + 0x4F8) = customs;
        for (__int64 i = 1; i <= trailers; i++) {
            BYTE* t = *(BYTE**)(type + 0x7C08) + (i - 1) * 0x110;
            BYTE* tv = create(world, *(BYTE**)(t + 0x100), depot, 1, (unsigned)i, t[0x108], 0, minusOne, v[0x67D], 1, 0);
            if (realistic && tv) {
                *(BYTE**)(tv + 0x4F0) = customs;
                *(BYTE**)(tv + 0x4F8) = *(BYTE**)(v + 0x4F0);
            }
            couple(world, v, LastVehicle(world), minusOne, 0, 1, 0);
        }
    }
    if (m.used && v) ApplyUsedMarketStep(market, type, m, v);
    if (realistic && v) RouteRealisticVehicleDelivery(world, m, depot, customs, slot, v);
    return nullptr;
}

static const char* Replay(const PurchaseMsg& m)
{
    BYTE* world = g_exeBase + WORLD_RVA;
    __try {
        BYTE* type = TypeFor(world, m.typeIndex, m.typeCheck);
        if (!type) return "the vehicle type is not in this PC's list";
        BYTE* depot = BuildingAt(world, m.depot);
        if (!depot) return "the depot differs";
        if (!WriteVariants(type, m)) return "the trailers differ";
        int campaign = *(int*)(g_exeBase + CAMPAIGN_RVA);
        if (m.kind >= 5) return ReplayBlueprint(world, m, type, depot, campaign);
        return ReplayVehicle(world, m, type, depot, BuildingAt(world, m.customsRub), BuildingAt(world, m.customsUsd),
                             campaign);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault";
    }
}

bool Purchase_ApplyCommand(const void* data, size_t size)
{
    if (size != sizeof(PurchaseMsg)) return false;
    PurchaseMsg m;
    memcpy(&m, data, sizeof(m));
    if (!RoutinesOk()) {
        Logf("republicmp  PURCHASE: not made - this PC's game routines differ");
        return false;
    }
    const char* why = Replay(m);
    static const char* const kKinds[] = { "?", "vehicle", "vehicle", "?", "?", "licence", "licence", "factory blueprint" };
    Logf("republicmp  PURCHASE: %s%s (window kind %u), type #%u, depot #%d, %.0f rubles %.0f dollars: %s",
         m.kind < 8 ? kKinds[m.kind] : "?", m.used ? " used" : "", m.kind, m.typeIndex, m.depot.place, m.rubles, m.dollars,
         why ? why : "made");
    return why == nullptr;
}
