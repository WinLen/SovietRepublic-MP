// SPDX-License-Identifier: GPL-3.0-only
// Canonical RVA addresses and structure offsets for SOVIET64.exe (PE 6A3EB6AD).
// Centralizes engine memory constants and enforces compile-time invariants from AGENTS.md.
#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Global Engine Objects (RVAs from g_exeBase)
// ---------------------------------------------------------------------------

#ifndef WORLD_RVA
#define WORLD_RVA                   0x9D4F10
#endif
#ifndef WORLD_THIS_RVA
#define WORLD_THIS_RVA              WORLD_RVA
#endif
#ifndef UNIVERSAL_THIS_RVA
#define UNIVERSAL_THIS_RVA          WORLD_RVA
#endif

#ifndef ENGINE_RVA
#define ENGINE_RVA                  0x9941F0
#endif
#ifndef ENGINE_TERRAIN_OFFSET
#define ENGINE_TERRAIN_OFFSET       0xED8
#endif

#ifndef CAMERA_RVA
#define CAMERA_RVA                  0x9E9C90
#endif
#ifndef CAMERA_POS_OFFSET
#define CAMERA_POS_OFFSET           0x80
#endif
#ifndef CAMERA_LOOKAT_OFFSET
#define CAMERA_LOOKAT_OFFSET        0x98
#endif
#ifndef CAMERA_VIEWPROJ_OFFSET
#define CAMERA_VIEWPROJ_OFFSET      0x1F0
#endif

#ifndef LOAD_TRIGGER_DAT_RVA
#define LOAD_TRIGGER_DAT_RVA        0x993C66
#endif
#ifndef WINDOW_FOCUS_BYTE_RVA
#define WINDOW_FOCUS_BYTE_RVA       0x99208F
#endif
#ifndef TERRAIN_FLAG_DAT_RVA
#define TERRAIN_FLAG_DAT_RVA        0x995298
#endif
#ifndef SAVE_NAME_DAT_RVA
#define SAVE_NAME_DAT_RVA           0x9962B0
#endif

// ---------------------------------------------------------------------------
// WORLD Object Offsets (relative to g_exeBase + WORLD_RVA)
// ---------------------------------------------------------------------------

#ifndef WORLD_CHEAT_MODE_OFFSET
#define WORLD_CHEAT_MODE_OFFSET     0x0009
#endif
#ifndef WORLD_DEV_MODE_OFFSET
#define WORLD_DEV_MODE_OFFSET       0x000A
#endif
#ifndef WORLD_UNLIMITED_MONEY_OFFSET
#define WORLD_UNLIMITED_MONEY_OFFSET 0x578
#endif
#ifndef WORLD_USD_OFFSET
#define WORLD_USD_OFFSET            0x580
#endif
#ifndef WORLD_RUB_OFFSET
#define WORLD_RUB_OFFSET            0x588
#endif
#ifndef WORLD_DAY_OFFSET
#define WORLD_DAY_OFFSET            0x590
#endif
#ifndef WORLD_YEAR_OFFSET
#define WORLD_YEAR_OFFSET           0x594
#endif
#ifndef WORLD_PROGRESS_OFFSET
#define WORLD_PROGRESS_OFFSET       0x59C
#endif

#ifndef SAVE_NAME_FIELD_OFFSET
#define SAVE_NAME_FIELD_OFFSET      0x9F8
#endif
#ifndef SAVE_NAME_FIELD_MAX_LEN
#define SAVE_NAME_FIELD_MAX_LEN     120
#endif

// Invariant #12: world + 0xB08 is a 1-byte citizen option; 0xB0A+ is written by FormatMoney (0x336420).
#ifndef WORLD_CITIZEN_OPT_BYTE_OFF
#define WORLD_CITIZEN_OPT_BYTE_OFF  0xB08
#endif
#ifndef WORLD_FORMAT_MONEY_BUF_OFF
#define WORLD_FORMAT_MONEY_BUF_OFF  0xB0A
#endif

#ifndef CURSOR_X_OFFSET
#define CURSOR_X_OFFSET             0xF6C
#endif
#ifndef CURSOR_Y_OFFSET
#define CURSOR_Y_OFFSET             0xF70
#endif
#ifndef CURSOR_Z_OFFSET
#define CURSOR_Z_OFFSET             0xF74
#endif
#ifndef WORLD_HIDE_HUD_OFF
#define WORLD_HIDE_HUD_OFF          0xFBA
#endif
#ifndef TOOL_GUARD1_OFFSET
#define TOOL_GUARD1_OFFSET          0xFBC
#endif
#ifndef TOOL_GUARD2_OFFSET
#define TOOL_GUARD2_OFFSET          0xFBE
#endif
#ifndef TOOL_GUARD3_OFFSET
#define TOOL_GUARD3_OFFSET          0xFC3
#endif
#ifndef WORLD_RESOURCES_OFFSET
#define WORLD_RESOURCES_OFFSET      0xC2B0
#endif
#ifndef WORLD_WORK_RESOURCE_PTR_OFF
#define WORLD_WORK_RESOURCE_PTR_OFF 0xC2C8
#endif
#ifndef WORLD_ECON_TAB_OFFSET
#define WORLD_ECON_TAB_OFFSET       0xC65C
#endif
#ifndef WORLD_SYSTAB_SUBPAGE_OFFSET
#define WORLD_SYSTAB_SUBPAGE_OFFSET 0xC65C
#endif
#ifndef WORLD_RESOURCE_STRIDE
#define WORLD_RESOURCE_STRIDE       0x340
#endif
#ifndef WORLD_VEHICLE_TYPES_OFFSET
#define WORLD_VEHICLE_TYPES_OFFSET  0x12840
#endif
#ifndef WORLD_VEHICLE_TYPE_STRIDE
#define WORLD_VEHICLE_TYPE_STRIDE   0x9F08
#endif

#ifndef TOOL_DRAG_ACTIVE_OFFSET
#define TOOL_DRAG_ACTIVE_OFFSET     0xD2AC
#endif
#ifndef WORLD_WORKSHOP_MGR_OFF
#define WORLD_WORKSHOP_MGR_OFF      0xD3A8
#endif
#ifndef WORLD_MINIMAP_PTR_OFF
#define WORLD_MINIMAP_PTR_OFF       0xD400
#endif
#ifndef WORLD_SELECTED_TOOL_PTR_OFF
#define WORLD_SELECTED_TOOL_PTR_OFF 0xD428
#endif
#ifndef WORLD_FORCE_TERRAIN_SAVE_OFF
#define WORLD_FORCE_TERRAIN_SAVE_OFF 0xFC90
#endif

// Bank / Loan & Economy Statistics subsystem offsets inside WORLD (SOVIET64.exe PE 6A3EB6AD)
#ifndef WORLD_HUD_POP_BLOCK_OFFSET
#define WORLD_HUD_POP_BLOCK_OFFSET  0xC6C8
#endif
#ifndef WORLD_HUD_POP_TIMER_OFFSET
#define WORLD_HUD_POP_TIMER_OFFSET  0xC6DC
#endif
#ifndef WORLD_HUD_POP_COUNT_OFFSET
#define WORLD_HUD_POP_COUNT_OFFSET  0xC720
#endif
#ifndef WORLD_HUD_POP_BLOCK_SIZE
#define WORLD_HUD_POP_BLOCK_SIZE    0xC4
#endif
#ifndef WORLD_ECON_GRAPH_DIRTY_OFF
#define WORLD_ECON_GRAPH_DIRTY_OFF  0x106D8
#endif
#ifndef WORLD_POP_GRAPH_DIRTY_OFF
#define WORLD_POP_GRAPH_DIRTY_OFF   0x108C0
#endif
#ifndef WORLD_LOANS_OFFSET
#define WORLD_LOANS_OFFSET          0x10BD0
#endif
#ifndef WORLD_LOAN_SELECTED_OFFSET
#define WORLD_LOAN_SELECTED_OFFSET  0x10BE8
#endif
#ifndef WORLD_LOAN_REPAY_AMOUNT_OFF
#define WORLD_LOAN_REPAY_AMOUNT_OFF 0x10C08
#endif
#ifndef WORLD_POP_STAT_TIMER_OFF
#define WORLD_POP_STAT_TIMER_OFF    0x10F24
#endif
#ifndef WORLD_POP_STAT_CURRENT_OFF
#define WORLD_POP_STAT_CURRENT_OFF  0x10F28
#endif
#ifndef WORLD_STAT_CURRENT_OFFSET
#define WORLD_STAT_CURRENT_OFFSET   0x11328
#endif
#ifndef WORLD_LOAN_STATS_OFFSET
#define WORLD_LOAN_STATS_OFFSET     0x11718
#endif
#ifndef WORLD_STAT_HISTORY_OFFSET
#define WORLD_STAT_HISTORY_OFFSET   0x11728
#endif

#ifndef STAT_RECORD_SIZE
#define STAT_RECORD_SIZE            0x400
#endif
#ifndef STAT_RES_VEC_COUNT
#define STAT_RES_VEC_COUNT          16
#endif
#ifndef STAT_RES_VEC_BASE_OFF
#define STAT_RES_VEC_BASE_OFF       0x008
#endif
#ifndef STAT_RES_VEC_STRIDE
#define STAT_RES_VEC_STRIDE         0x018
#endif
#ifndef STAT_VEHICLES_OFF
#define STAT_VEHICLES_OFF           0x188
#endif
#ifndef STAT_IMMIGRANT_SPEND_OFF
#define STAT_IMMIGRANT_SPEND_OFF    0x240
#endif
#ifndef STAT_CITIZEN_COUNTS_OFF
#define STAT_CITIZEN_COUNTS_OFF     0x248
#endif
#ifndef STAT_CITIZEN_SATISFACTION_OFF
#define STAT_CITIZEN_SATISFACTION_OFF 0x25C
#endif
#ifndef STAT_CITIZEN_AVERAGES_OFF
#define STAT_CITIZEN_AVERAGES_OFF   0x2AC
#endif
#ifndef STAT_CITIZEN_DEMOGRAPHICS_OFF
#define STAT_CITIZEN_DEMOGRAPHICS_OFF 0x2B8
#endif
#ifndef STAT_CITIZEN_ADULTS_OFF
#define STAT_CITIZEN_ADULTS_OFF     0x2C4
#endif
#ifndef STAT_CITIZEN_BRACKETS_OFF
#define STAT_CITIZEN_BRACKETS_OFF   0x2C8
#endif
#ifndef STAT_EXTRA_COUNTS_OFF
#define STAT_EXTRA_COUNTS_OFF       0x2EC
#endif
#ifndef STAT_TOURISM_OFF
#define STAT_TOURISM_OFF            0x340
#endif
#ifndef STAT_LOANS_OFF
#define STAT_LOANS_OFF              0x3F0
#endif

#ifndef WORLD_RES_PTR_SLOTS_BEGIN_OFF
#define WORLD_RES_PTR_SLOTS_BEGIN_OFF 0xC2C8
#endif
#ifndef WORLD_RES_PTR_SLOTS_END_OFF
#define WORLD_RES_PTR_SLOTS_END_OFF   0xC430
#endif
#ifndef WORLD_ELENERGY_RES_PTR_OFF
#define WORLD_ELENERGY_RES_PTR_OFF    0xC3B8
#endif

#ifndef RVA_BLD_SIM_LOOP_HEAD
#define RVA_BLD_SIM_LOOP_HEAD       0x13CCFE
#endif
#ifndef RVA_BLD_SIM_LOOP_TAIL
#define RVA_BLD_SIM_LOOP_TAIL       0x13E494
#endif
#ifndef RVA_BLD_CONSTRUCT_WORKERS
#define RVA_BLD_CONSTRUCT_WORKERS   0x1403F0
#endif
#ifndef RVA_CONSTRUCT_GROUNDWORK
#define RVA_CONSTRUCT_GROUNDWORK    0x143710
#endif
#ifndef RVA_BLD_WORKER_STEP
#define RVA_BLD_WORKER_STEP         0x14C5B0
#endif
#ifndef RVA_BLD_IMPORT_STEP
#define RVA_BLD_IMPORT_STEP         0x171E10
#endif
#ifndef RVA_BLD_PRODUCTIVITY_STEP
#define RVA_BLD_PRODUCTIVITY_STEP   0x175A00
#endif
#ifndef RVA_BLD_EXPORT_STEP
#define RVA_BLD_EXPORT_STEP         0x183D30
#endif
#ifndef RVA_BLD_TOURISM_STEP
#define RVA_BLD_TOURISM_STEP        0x1856F0
#endif
#ifndef RVA_BLD_STEP_PRODUCTION
#define RVA_BLD_STEP_PRODUCTION     0x189030
#endif
#ifndef RVA_CIT_HOME_STEP
#define RVA_CIT_HOME_STEP           0x18FF20
#endif
#ifndef RVA_BLD_CONSUME_RESOURCE
#define RVA_BLD_CONSUME_RESOURCE    0x1990C0
#endif
#ifndef RVA_CUSTOMS_BORDER
#define RVA_CUSTOMS_BORDER          0x1B7D50
#endif
#ifndef RVA_CUSTOMS_VEHICLE
#define RVA_CUSTOMS_VEHICLE         0x1B9A40
#endif
#ifndef RVA_BLD_STEP_PRE_POST
#define RVA_BLD_STEP_PRE_POST       0x1BD530
#endif
#ifndef RVA_BLD_STEP_UTILITY
#define RVA_BLD_STEP_UTILITY        0x1D1E80
#endif
#ifndef RVA_BLD_UTILITY_BORDER_STEP
#define RVA_BLD_UTILITY_BORDER_STEP 0x1D2DD0
#endif
#ifndef RVA_LEDGER_ADD
#define RVA_LEDGER_ADD              0x2FDB00
#endif
#ifndef RVA_STAT_RECORD_CLEAR
#define RVA_STAT_RECORD_CLEAR       0x2FC290
#endif
#ifndef CHEAT_KEYS_STEP_RVA
#define CHEAT_KEYS_STEP_RVA         0x309D80
#endif
#ifndef CHEAT_KEYS_CALL_RVA
#define CHEAT_KEYS_CALL_RVA         0x30D959
#endif
#ifndef RVA_CALC_STORED_RESOURCES
#define RVA_CALC_STORED_RESOURCES   0x33E620
#endif
#ifndef RVA_VEHICLE_BORDER_EXPORT
#define RVA_VEHICLE_BORDER_EXPORT   0x3DF9B9
#endif
#ifndef RVA_VEC_LOAN_PUSH_BACK
#define RVA_VEC_LOAN_PUSH_BACK      0x448AA0
#endif
#ifndef RVA_TAKE_LOAN
#define RVA_TAKE_LOAN               0x4B9140
#endif
#ifndef RVA_GET_MAX_AVAILABLE_BORROW
#define RVA_GET_MAX_AVAILABLE_BORROW 0x4B9380
#endif
#ifndef RVA_STEP_ONE_LOAN_DAY
#define RVA_STEP_ONE_LOAN_DAY       0x4B9490
#endif
#ifndef RVA_STEP_ALL_LOANS_DAILY
#define RVA_STEP_ALL_LOANS_DAILY    0x4B9660
#endif
#ifndef RVA_CALC_INTEREST_RATE
#define RVA_CALC_INTEREST_RATE      0x4B9A80
#endif
#ifndef RVA_VEHICLE_CARGO_TRADE
#define RVA_VEHICLE_CARGO_TRADE     0x68B330
#endif
#ifndef RVA_VEHICLE_REFUEL
#define RVA_VEHICLE_REFUEL          0x6B3B50
#endif
#ifndef RVA_FOREIGN_LABOR_STEP
#define RVA_FOREIGN_LABOR_STEP      0x830820
#endif
#ifndef RVA_FOREIGN_WORKER_BOARD
#define RVA_FOREIGN_WORKER_BOARD    0x831120
#endif
#ifndef RVA_CITIZEN_BIRTH
#define RVA_CITIZEN_BIRTH           0x834A20
#endif
#ifndef RVA_CITIZEN_REMOVE
#define RVA_CITIZEN_REMOVE          0x835D70
#endif

enum SovietLoanCurrency {
    LOAN_CURRENCY_USD = 1,
    LOAN_CURRENCY_RUB = 2,
};

#pragma pack(push, 1)
struct SovietLoan {
    float   yearInterestRate;        // +0x00 ($YearInterestRate)
    float   yearInterestRatePenalty; // +0x04 ($YearInterestRatePenalty = 2 * yearInterestRate)
    int32_t loanType;                // +0x08 ($LoanType: 1 = USD, 2 = RUB)
    int32_t loanSubType;             // +0x0C ($LoanSubType: 0 = West SSR / State 1, 1 = East SSR / State 2)
    int32_t daysRemaining;           // +0x10 ($CurrentDurationDays)
    float   remainingPrincipal;      // +0x14 ($CurrentAmount)
    float   unpaidPenaltyBalance;    // +0x18 ($CurrentAmountForPenalty)
    float   initialPrincipal;        // +0x1C ($Stat_InitialAmount)
    int32_t initialDays;             // +0x20 ($Stat_ContractDurationDays)
    float   totalPaidSoFar;          // +0x24 ($Stat_PaidAmount)
};

struct SovietStatResEntry {
    uintptr_t resourcePtr; // +0x00 (pointer to resource descriptor in world + 0xC2B0)
    float     amountTons;  // +0x08 (physical tons / MWh)
    float     moneyValue;  // +0x0C (value in RUB or USD)
};
#pragma pack(pop)

#ifndef WORLD_BUILDINGS_OFFSET
#define WORLD_BUILDINGS_OFFSET      0x11B08
#endif
#ifndef WORLD_CONSTRUCTIONS_OFFSET
#define WORLD_CONSTRUCTIONS_OFFSET  0x11C28
#endif
#ifndef WORLD_CITIZENS_OFFSET
#define WORLD_CITIZENS_OFFSET       0x126A8
#endif
#ifndef WORLD_LINES_OFFSET
#define WORLD_LINES_OFFSET          0x12800
#endif
#ifndef WORLD_VEHICLES_OFFSET
#define WORLD_VEHICLES_OFFSET       0x12810
#endif

#ifndef SAVE_TRIGGER_FLAG_OFFSET
#define SAVE_TRIGGER_FLAG_OFFSET    0x14D35
#endif
#ifndef SAVE_ZIP_AFTER_FLAG_OFFSET
#define SAVE_ZIP_AFTER_FLAG_OFFSET  0x14D36
#endif

// Direct RVAs into WORLD containers
#ifndef VEC_BUILDINGS_BEGIN_RVA
#define VEC_BUILDINGS_BEGIN_RVA     0x9E6A18
#endif
#ifndef VEC_BUILDINGS_END_RVA
#define VEC_BUILDINGS_END_RVA       0x9E6A20
#endif
#ifndef VEC_CONSTRUCTIONS_BEGIN_RVA
#define VEC_CONSTRUCTIONS_BEGIN_RVA 0x9E6B38
#endif
#ifndef VEC_CONSTRUCTIONS_END_RVA
#define VEC_CONSTRUCTIONS_END_RVA   0x9E6B40
#endif
#ifndef VEC_VEHICLES_BEGIN_RVA
#define VEC_VEHICLES_BEGIN_RVA      (WORLD_RVA + WORLD_VEHICLES_OFFSET)
#endif
#ifndef VEC_VEHICLES_END_RVA
#define VEC_VEHICLES_END_RVA        (WORLD_RVA + WORLD_VEHICLES_OFFSET + 8)
#endif

// Invariant #6: SOVIET_WORKSHOP_MANAGER* is at 0x9E22B8 (WORLD_RVA + 0xD3A8), NEVER 0x9E7750.
#ifndef WORKSHOP_MGR_PTR_RVA
#define WORKSHOP_MGR_PTR_RVA        0x9E22B8
#endif
#ifndef RVA_WORKSHOP_REFRESH
#define RVA_WORKSHOP_REFRESH        0x8407C0
#endif

// ---------------------------------------------------------------------------
// Object & Manager Offsets
// ---------------------------------------------------------------------------

#ifndef OBJ_POS_OFFSET
#define OBJ_POS_OFFSET              0x350
#endif
#ifndef BLD_PROGRESS_OFFSET
#define BLD_PROGRESS_OFFSET         0x604
#endif
#ifndef BLD_PAY_RUB_OFFSET
#define BLD_PAY_RUB_OFFSET          0x608
#endif
#ifndef BLD_PAY_USD_OFFSET
#define BLD_PAY_USD_OFFSET          0x609
#endif
#ifndef BLD_STORAGE_OFFSET
#define BLD_STORAGE_OFFSET          0x970
#endif
#ifndef BLD_STORAGE_SLOT_SIZE
#define BLD_STORAGE_SLOT_SIZE       0xE0
#endif
#ifndef BLD_WORKERS_OFFSET
#define BLD_WORKERS_OFFSET          0xBC0
#endif
#ifndef BLD_PRODUCED_OFFSET
#define BLD_PRODUCED_OFFSET         0xDE4
#endif
#ifndef BUILDING_STRUCT_BYTES
#define BUILDING_STRUCT_BYTES       0x1420
#endif

#ifndef VEH_ID_OFFSET
#define VEH_ID_OFFSET               0x04
#endif
#ifndef VEH_CARGO_OFFSET
#define VEH_CARGO_OFFSET            0x3B8
#endif
#ifndef VEH_HOME_OFFSET
#define VEH_HOME_OFFSET             0x4F8
#endif
#ifndef VEH_DEPOT_OFFSET
#define VEH_DEPOT_OFFSET            0x600
#endif
#ifndef VEH_SCHED_IDX_OFFSET
#define VEH_SCHED_IDX_OFFSET        0x698
#endif
#ifndef VEH_POS_OFFSET
#define VEH_POS_OFFSET              0xDE0
#endif
#ifndef VEH_SIXTH_NODE_ACTIVE_OFFSET
#define VEH_SIXTH_NODE_ACTIVE_OFFSET 0x1314
#endif
#ifndef VEH_SIXTH_NODE_OFFSET
#define VEH_SIXTH_NODE_OFFSET       0x1318
#endif
#ifndef VEH_LINKED_TRAILER_OFFSET
#define VEH_LINKED_TRAILER_OFFSET   0x1A68
#endif
#ifndef C3D_NODE_BYTE_SIZE
#define C3D_NODE_BYTE_SIZE          0xBC
#endif

#ifndef CIT_HOME_BUILDING_OFFSET
#define CIT_HOME_BUILDING_OFFSET    0x20
#endif
#ifndef CIT_PARENT_OFFSET
#define CIT_PARENT_OFFSET           0xC8
#endif
#ifndef CIT_AGE_OFFSET
#define CIT_AGE_OFFSET              0xD4
#endif
#ifndef CIT_POS_OFFSET
#define CIT_POS_OFFSET              0x620
#endif
#ifndef CIT_INFANT_FLAG_OFFSET
#define CIT_INFANT_FLAG_OFFSET      0x71C
#endif
#ifndef CIT_UNBORN_FLAG_OFFSET
#define CIT_UNBORN_FLAG_OFFSET      0x748
#endif

// Invariant #16: Manager preview state starts at +0x408, std::vector<RoadSyncPoint> starts at +0x410.
#ifndef MANAGER_RESET_STATE_OFFSET
#define MANAGER_RESET_STATE_OFFSET  0x408
#endif
#ifndef MGR_ROUTE_BEGIN_OFFSET
#define MGR_ROUTE_BEGIN_OFFSET      0x410
#endif
#ifndef MGR_ROUTE_END_OFFSET
#define MGR_ROUTE_END_OFFSET        0x418
#endif

// Invariant #9: header.bin / headerE.bin binary format constants.
#ifndef HEADER_BIN_DATA_OFFSET
#define HEADER_BIN_DATA_OFFSET      0x260
#endif
#ifndef HEADER_BIN_RECORD_SIZE
#define HEADER_BIN_RECORD_SIZE      0x80
#endif

// Offsets inside SOVIET_WORKSHOP_ITEM (0x21118 bytes)
#ifndef ITEM_IS_SUBSCRIBED_OFF
#define ITEM_IS_SUBSCRIBED_OFF      0x0000
#endif
#ifndef ITEM_TYPE_OFF
#define ITEM_TYPE_OFF               0x0004
#endif
#ifndef ITEM_NAME_OFF
#define ITEM_NAME_OFF               0x006C
#endif
#ifndef ITEM_ID_QWORD_OFF
#define ITEM_ID_QWORD_OFF           0x40F0
#endif
#ifndef ITEM_ID_STR_OFF
#define ITEM_ID_STR_OFF             0x4500
#endif

// ---------------------------------------------------------------------------
// Compile-Time Invariant Assertions (AGENTS.md Section 3 & 4)
// ---------------------------------------------------------------------------

static_assert(VEC_BUILDINGS_BEGIN_RVA == WORLD_RVA + WORLD_BUILDINGS_OFFSET,
              "VEC_BUILDINGS_BEGIN_RVA must equal WORLD_RVA + WORLD_BUILDINGS_OFFSET (0x9E6A18)");
static_assert(VEC_CONSTRUCTIONS_BEGIN_RVA == WORLD_RVA + WORLD_CONSTRUCTIONS_OFFSET,
              "VEC_CONSTRUCTIONS_BEGIN_RVA must equal WORLD_RVA + WORLD_CONSTRUCTIONS_OFFSET (0x9E6B38)");
static_assert(WORKSHOP_MGR_PTR_RVA == WORLD_RVA + WORLD_WORKSHOP_MGR_OFF,
              "WORKSHOP_MGR_PTR_RVA must equal WORLD_RVA + 0xD3A8 (0x9E22B8), never 0x9E7750");
static_assert(MGR_ROUTE_BEGIN_OFFSET == MANAGER_RESET_STATE_OFFSET + 8,
              "MGR_ROUTE_BEGIN_OFFSET (0x410) must be 8 bytes after MANAGER_RESET_STATE_OFFSET (0x408)");
static_assert(HEADER_BIN_DATA_OFFSET == 608 && HEADER_BIN_RECORD_SIZE == 128,
              "header.bin layout must be 0x260 (608) header + 0x80 (128) byte records");
static_assert(sizeof(SovietLoan) == 0x28,
              "SovietLoan layout must match 40-byte engine struct (0x4B9140 / 0x4B9490 / 0x4B9750)");
static_assert(sizeof(SovietStatResEntry) == 0x10,
              "SovietStatResEntry layout must match 16-byte engine struct (0x2FDB00 / 0x2FE3F0)");
static_assert(WORLD_LOAN_STATS_OFFSET == WORLD_STAT_CURRENT_OFFSET + STAT_LOANS_OFF,
              "WORLD_LOAN_STATS_OFFSET (0x11718) must equal WORLD_STAT_CURRENT_OFFSET (0x11328) + STAT_LOANS_OFF (0x3F0)");
static_assert(WORLD_STAT_HISTORY_OFFSET == WORLD_STAT_CURRENT_OFFSET + STAT_RECORD_SIZE,
              "WORLD_STAT_HISTORY_OFFSET (0x11728) must equal WORLD_STAT_CURRENT_OFFSET (0x11328) + STAT_RECORD_SIZE (0x400)");
static_assert(STAT_CITIZEN_SATISFACTION_OFF == STAT_CITIZEN_COUNTS_OFF + 5 * sizeof(int32_t) &&
              STAT_CITIZEN_DEMOGRAPHICS_OFF == STAT_CITIZEN_AVERAGES_OFF + 3 * sizeof(float) &&
              STAT_CITIZEN_BRACKETS_OFF == STAT_CITIZEN_DEMOGRAPHICS_OFF + 4 * sizeof(int32_t) &&
              STAT_EXTRA_COUNTS_OFF == STAT_CITIZEN_BRACKETS_OFF + 9 * sizeof(int32_t),
              "SOVIET_STAT_RECORD citizen satisfaction/averages/demographics/brackets offsets must match 0x2FD2E0");
static_assert(WORLD_CHEAT_MODE_OFFSET == 0x9 && WORLD_DEV_MODE_OFFSET == 0xA &&
              WORLD_SYSTAB_SUBPAGE_OFFSET == WORLD_ECON_TAB_OFFSET,
              "Cheat/dev mode byte offsets (0x9, 0xA) and systab subpage offset (0xC65C) must match 0x309D80 / 0x329390");
static_assert(VEH_SIXTH_NODE_OFFSET == VEH_SIXTH_NODE_ACTIVE_OFFSET + 4 && C3D_NODE_BYTE_SIZE == 0xBC,
              "Vehicle 6th C3D_NODE (+0x1318, 0xBC bytes) must follow active index (+0x1314) per 0x66EDD0");
