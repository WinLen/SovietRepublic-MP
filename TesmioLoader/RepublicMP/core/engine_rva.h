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
#ifndef WORLD_CAMERA_MGR_PTR_OFF
#define WORLD_CAMERA_MGR_PTR_OFF    0x0018
#endif
#ifndef CAM_MGR_STRUCT_BYTES
#define CAM_MGR_STRUCT_BYTES        0x168
#endif
#ifndef CAM_MGR_POS_OFFSET
#define CAM_MGR_POS_OFFSET          0x08
#endif
#ifndef CAM_MGR_ANGLES_OFFSET
#define CAM_MGR_ANGLES_OFFSET       0x14
#endif
#ifndef CAM_MGR_CUR_POS_OFFSET
#define CAM_MGR_CUR_POS_OFFSET      0x20
#endif
#ifndef CAM_MGR_CUR_ANGLES_OFFSET
#define CAM_MGR_CUR_ANGLES_OFFSET   0x2C
#endif
#ifndef CAM_MGR_FOLLOW_ACTIVE_OFF
#define CAM_MGR_FOLLOW_ACTIVE_OFF   0x05B
#endif
#ifndef CAM_MGR_RIDE_VEH_PTR_OFF
#define CAM_MGR_RIDE_VEH_PTR_OFF    0x070
#endif
#ifndef CAM_MGR_FOLLOW_WIN_PTR_OFF
#define CAM_MGR_FOLLOW_WIN_PTR_OFF  0x078
#endif
#ifndef CAM_MGR_TRANS_STEP_OFFSET
#define CAM_MGR_TRANS_STEP_OFFSET   0x14C
#endif
#ifndef CAM_MGR_TRANS_TIMER_OFFSET
#define CAM_MGR_TRANS_TIMER_OFFSET  0x15C
#endif
#ifndef CAM_MGR_TRANS_DUR_OFFSET
#define CAM_MGR_TRANS_DUR_OFFSET    0x160
#endif
#ifndef FOLLOW_WIN_CITIZEN_PTR_OFF
#define FOLLOW_WIN_CITIZEN_PTR_OFF  0x058
#endif
#ifndef FOLLOW_WIN_VEHICLE_PTR_OFF
#define FOLLOW_WIN_VEHICLE_PTR_OFF  0x060
#endif
#ifndef CIT_RIDING_VEHICLE_PTR_OFF
#define CIT_RIDING_VEHICLE_PTR_OFF  0x660
#endif
#ifndef RVA_CAMERA_MGR_UPDATE
#define RVA_CAMERA_MGR_UPDATE       0x14250
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
#ifndef WORLD_SYSTAB_TAB_PTR_OFFSET
#define WORLD_SYSTAB_TAB_PTR_OFFSET 0xC650
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
#ifndef VEH_TYPE_SCRIPT_NAME_OFFSET
#define VEH_TYPE_SCRIPT_NAME_OFFSET 0x200u
#endif
#ifndef VEH_TYPE_CATEGORY_OFFSET
#define VEH_TYPE_CATEGORY_OFFSET    0x294u
#endif
#ifndef VEH_TYPE_HAS_LICENCE_OFFSET
#define VEH_TYPE_HAS_LICENCE_OFFSET 0x9C88u
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
#ifndef WORLD_TRACKER_BLOCK_OFFSET
#define WORLD_TRACKER_BLOCK_OFFSET         0x109A0u
#endif
#ifndef WORLD_TRACKER_BLOCK_SIZE
#define WORLD_TRACKER_BLOCK_SIZE           0x230u
#endif
#ifndef WORLD_TRACKER_HEALTH_CUR_SUM_OFF
#define WORLD_TRACKER_HEALTH_CUR_SUM_OFF   0x109A0u
#endif
#ifndef WORLD_TRACKER_HEALTH_CUR_CNT_OFF
#define WORLD_TRACKER_HEALTH_CUR_CNT_OFF   0x109D0u
#endif
#ifndef WORLD_TRACKER_HEALTH_OLD_SUM_OFF
#define WORLD_TRACKER_HEALTH_OLD_SUM_OFF   0x10A00u
#endif
#ifndef WORLD_TRACKER_HEALTH_OLD_CNT_OFF
#define WORLD_TRACKER_HEALTH_OLD_CNT_OFF   0x10A30u
#endif
#ifndef WORLD_TRACKER_HAPPY_CUR_SUM_OFF
#define WORLD_TRACKER_HAPPY_CUR_SUM_OFF    0x10A60u
#endif
#ifndef WORLD_TRACKER_HAPPY_CUR_CNT_OFF
#define WORLD_TRACKER_HAPPY_CUR_CNT_OFF    0x10ABCu
#endif
#ifndef WORLD_TRACKER_HAPPY_OLD_SUM_OFF
#define WORLD_TRACKER_HAPPY_OLD_SUM_OFF    0x10B18u
#endif
#ifndef WORLD_TRACKER_HAPPY_OLD_CNT_OFF
#define WORLD_TRACKER_HAPPY_OLD_CNT_OFF    0x10B74u
#endif
#ifndef TRACKER_HEALTH_CAUSE_COUNT
#define TRACKER_HEALTH_CAUSE_COUNT         12
#endif
#ifndef TRACKER_HAPPY_CAUSE_COUNT
#define TRACKER_HAPPY_CAUSE_COUNT          23
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
#ifndef RVA_BLD_COURTHOUSE_STEP
#define RVA_BLD_COURTHOUSE_STEP     0x181C80
#endif
#ifndef RVA_BLD_POLICE_STEP
#define RVA_BLD_POLICE_STEP         0x1825D0
#endif
#ifndef RVA_BLD_HOTEL_STEP
#define RVA_BLD_HOTEL_STEP          0x183010
#endif
#ifndef RVA_BLD_ATTRACTION1_STEP
#define RVA_BLD_ATTRACTION1_STEP    0x183580
#endif
#ifndef RVA_BLD_EXPORT_STEP
#define RVA_BLD_EXPORT_STEP         0x183D30
#endif
#ifndef RVA_BLD_ATTRACTION2_STEP
#define RVA_BLD_ATTRACTION2_STEP    0x184930
#endif
#ifndef RVA_BLD_CUSTOMS_TOURIST_STEP
#define RVA_BLD_CUSTOMS_TOURIST_STEP 0x1854E0
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
#ifndef RVA_WATER_HEALTH_STEP
#define RVA_WATER_HEALTH_STEP       0x1B0950
#endif
#ifndef RVA_CUSTOMS_BORDER
#define RVA_CUSTOMS_BORDER          0x1B7D50
#endif
#ifndef RVA_CUSTOMS_VEHICLE
#define RVA_CUSTOMS_VEHICLE         0x1B9A40
#endif
#ifndef RVA_LIVING_TICK
#define RVA_LIVING_TICK             0x1BC210
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
#ifndef RVA_HEATING_STEP
#define RVA_HEATING_STEP            0x4881A0
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
#ifndef RVA_CITIZEN_VEHICLE_ALIGHT
#define RVA_CITIZEN_VEHICLE_ALIGHT  0x831700
#endif
#ifndef RVA_CITIZEN_FOOT_ARRIVE
#define RVA_CITIZEN_FOOT_ARRIVE     0x82D820
#endif
#ifndef RVA_CITIZEN_COMMIT_CRIME
#define RVA_CITIZEN_COMMIT_CRIME    0x2558D0
#endif
#ifndef RVA_CRIME_EXPIRE_CASE
#define RVA_CRIME_EXPIRE_CASE       0x2571E0
#endif
#ifndef RVA_CITIZEN_CHURCH_STEP
#define RVA_CITIZEN_CHURCH_STEP     0x834520
#endif
#ifndef RVA_CITIZEN_BIRTH
#define RVA_CITIZEN_BIRTH           0x834A20
#endif
#ifndef RVA_CITIZEN_STARVE_STEP
#define RVA_CITIZEN_STARVE_STEP     0x834DE0
#endif
#ifndef RVA_CITIZEN_REMOVE
#define RVA_CITIZEN_REMOVE          0x835D70
#endif
#ifndef RVA_CITIZEN_EVAL_NEEDS
#define RVA_CITIZEN_EVAL_NEEDS      0x83A6D0
#endif
#ifndef RVA_CITIZEN_CAN_MOVE_TO_BLD
#define RVA_CITIZEN_CAN_MOVE_TO_BLD 0x83C590
#endif
#ifndef RVA_CITIZEN_MOVE_TO_BLD
#define RVA_CITIZEN_MOVE_TO_BLD     0x83CA50
#endif

static_assert(WORLD_SYSTAB_SUBPAGE_OFFSET == WORLD_SYSTAB_TAB_PTR_OFFSET + 0x0Cu &&
              RVA_BLD_COURTHOUSE_STEP < RVA_BLD_POLICE_STEP &&
              RVA_BLD_POLICE_STEP < RVA_BLD_HOTEL_STEP &&
              RVA_BLD_HOTEL_STEP < RVA_BLD_ATTRACTION1_STEP &&
              RVA_BLD_ATTRACTION1_STEP < RVA_BLD_ATTRACTION2_STEP &&
              RVA_BLD_ATTRACTION2_STEP < RVA_BLD_CUSTOMS_TOURIST_STEP &&
              RVA_BLD_CUSTOMS_TOURIST_STEP < RVA_BLD_TOURISM_STEP &&
              RVA_CITIZEN_COMMIT_CRIME < RVA_CRIME_EXPIRE_CASE &&
              RVA_CITIZEN_FOOT_ARRIVE < RVA_CITIZEN_VEHICLE_ALIGHT &&
              RVA_CITIZEN_EVAL_NEEDS < RVA_CITIZEN_CAN_MOVE_TO_BLD &&
              RVA_CITIZEN_CAN_MOVE_TO_BLD < RVA_CITIZEN_MOVE_TO_BLD,
              "Tourism & Crime building/citizen step RVAs and WORLD_SYSTAB_TAB_PTR_OFFSET (+0xC650) must match 1.1.1.9 layout");

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

#ifndef BLD_TYPE_OFFSET
#define BLD_TYPE_OFFSET             0x318
#endif
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
#ifndef BLD_EFFECTIVE_CAP_FLOAT_OFFSET
#define BLD_EFFECTIVE_CAP_FLOAT_OFFSET 0xDDC
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
#ifndef VEH_PARENT_PTR_OFFSET
#define VEH_PARENT_PTR_OFFSET       0x398
#endif
#ifndef VEH_CHILDREN_BEGIN_OFFSET
#define VEH_CHILDREN_BEGIN_OFFSET   0x3A0
#endif
#ifndef VEH_CHILDREN_END_OFFSET
#define VEH_CHILDREN_END_OFFSET     0x3A8
#endif
#ifndef VEH_LINK_PTR_OFFSET
#define VEH_LINK_PTR_OFFSET         0x4A0
#endif
#ifndef VEH_CUR_BLD_OFFSET
#define VEH_CUR_BLD_OFFSET          0x4F0
#endif
#ifndef VEH_STOPPED_BLD_OFFSET
#define VEH_STOPPED_BLD_OFFSET      0x530
#endif
#ifndef VEH_CUSTOM_LINE_FLAG_OFF
#define VEH_CUSTOM_LINE_FLAG_OFF    0x5FD
#endif
#ifndef VEH_TARGET_STOP_BLD_OFF
#define VEH_TARGET_STOP_BLD_OFF     0x610
#endif
#ifndef VEH_STOPS_LIST_OFFSET
#define VEH_STOPS_LIST_OFFSET       0x638
#endif
#ifndef VEH_STOP_RECORD_STRIDE
#define VEH_STOP_RECORD_STRIDE      0x198
#endif
#ifndef VEH_STOP_HALF_STRIDE
#define VEH_STOP_HALF_STRIDE        0x0C8
#endif
#ifndef VEH_STOP_BLD_PTR_VEC_OFF
#define VEH_STOP_BLD_PTR_VEC_OFF    0x680
#endif
#ifndef VEH_TYPE_PTR_OFFSET
#define VEH_TYPE_PTR_OFFSET         0x1708
#endif
#ifndef VEH_CABLEWAY_START_FLAG_OFF
#define VEH_CABLEWAY_START_FLAG_OFF 0x198C
#endif
#ifndef VEH_HELI_FLIGHT_STATE_OFF
#define VEH_HELI_FLIGHT_STATE_OFF   0x1A04
#endif
#ifndef VEH_TYPE_PASSENGER_CAP_OFF
#define VEH_TYPE_PASSENGER_CAP_OFF  0x148
#endif
#ifndef VEH_TYPE_CATEGORY_OFFSET
#define VEH_TYPE_CATEGORY_OFFSET    0x294
#endif
#ifndef WORLD_WESTERN_BORDER_BLD_OFF
#define WORLD_WESTERN_BORDER_BLD_OFF 0x11AF8
#endif
#ifndef WORLD_SOVIET_BORDER_BLD_OFF
#define WORLD_SOVIET_BORDER_BLD_OFF  0x11B00
#endif
#ifndef RVA_LINES_NOTIFY_VEH
#define RVA_LINES_NOTIFY_VEH        0x499790
#endif
#ifndef RVA_VEH_INVERT_STOPS
#define RVA_VEH_INVERT_STOPS        0x6644A0
#endif
#ifndef RVA_VEH_ADD_STOP
#define RVA_VEH_ADD_STOP            0x67D5D0
#endif
#ifndef RVA_VEH_DEL_STOP
#define RVA_VEH_DEL_STOP            0x67D910
#endif
#ifndef RVA_VEH_REBUILD_CARGO_SUM
#define RVA_VEH_REBUILD_CARGO_SUM   0x6BF1D0
#endif
#ifndef RVA_VEH_ADD_STOP_CARGO_RES
#define RVA_VEH_ADD_STOP_CARGO_RES  0x6BF710
#endif
#ifndef RVA_VEH_RELEASE
#define RVA_VEH_RELEASE             0x6C52C0
#endif
#ifndef RVA_HELI_REROUTE
#define RVA_HELI_REROUTE            0x6DE260
#endif
#ifndef VEH_WORKER_SIM_FLAG_OFFSET
#define VEH_WORKER_SIM_FLAG_OFFSET  0xD41
#endif
#ifndef VEH_STEER_CURRENT_OFFSET
#define VEH_STEER_CURRENT_OFFSET    0x115C
#endif
#ifndef VEH_STEER_TARGET_OFFSET
#define VEH_STEER_TARGET_OFFSET     0x1160
#endif
#ifndef VEH_RENDER_VISIBLE_OFFSET
#define VEH_RENDER_VISIBLE_OFFSET   0x13D8
#endif
#ifndef VEH_WHEELS_BEGIN_OFFSET
#define VEH_WHEELS_BEGIN_OFFSET     0x1438
#endif
#ifndef VEH_WHEELS_END_OFFSET
#define VEH_WHEELS_END_OFFSET       0x1440
#endif
#ifndef VEH_WHEEL_STRUCT_STRIDE
#define VEH_WHEEL_STRUCT_STRIDE     24
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

// Residential building invite citizens (0x6F8CC0: 0x6FEE1E / 0x6FF31A / 0x6FF52D / 0x6FFA4E -> 0x823470)
#ifndef RVA_SPAWN_CITIZEN
#define RVA_SPAWN_CITIZEN                   0x823470
#endif
#ifndef RVA_IAT_RAND
#define RVA_IAT_RAND                        0x86E0A0
#endif
#ifndef RVA_IAT_RAND_FLOAT
#define RVA_IAT_RAND_FLOAT                  0x86CA78
#endif
#ifndef RVA_INVITE_EXPERT_EDU_MIN_FLOAT
#define RVA_INVITE_EXPERT_EDU_MIN_FLOAT     0x90A2A8
#endif
#ifndef RVA_INVITE_EXPERT_EDU_MAX_FLOAT
#define RVA_INVITE_EXPERT_EDU_MAX_FLOAT     0x90A310
#endif
#ifndef RVA_INVITE_EXPERT_MULT_FLOAT
#define RVA_INVITE_EXPERT_MULT_FLOAT        0x90A364
#endif
#ifndef WORLD_PRICE_IMMIGRANT_USD_OFF
#define WORLD_PRICE_IMMIGRANT_USD_OFF       0x0E7C
#endif
#ifndef WORLD_PRICE_IMMIGRANT_RUB_OFF
#define WORLD_PRICE_IMMIGRANT_RUB_OFF       0x0E80
#endif
#ifndef WORLD_IMMIGRANT_BASE_SPEND_USD_OFF
#define WORLD_IMMIGRANT_BASE_SPEND_USD_OFF  0x11560
#endif
#ifndef WORLD_IMMIGRANT_BASE_SPEND_RUB_OFF
#define WORLD_IMMIGRANT_BASE_SPEND_RUB_OFF  0x11564
#endif
#ifndef WORLD_IMMIGRANT_RATE_MULT_RUB_OFF
#define WORLD_IMMIGRANT_RATE_MULT_RUB_OFF   0x13694
#endif
#ifndef WORLD_IMMIGRANT_RATE_MULT_USD_OFF
#define WORLD_IMMIGRANT_RATE_MULT_USD_OFF   0x1369C
#endif
#ifndef BLD_AREA_PTR_OFF
#define BLD_AREA_PTR_OFF                    0x108
#endif
#ifndef AREA_CITY_STAT_PTR_OFF
#define AREA_CITY_STAT_PTR_OFF              0x1B8
#endif
#ifndef BLD_CITIZENS_BEGIN_OFF
#define BLD_CITIZENS_BEGIN_OFF              0xBC0
#endif
#ifndef BLD_CITIZENS_END_OFF
#define BLD_CITIZENS_END_OFF                0xBC8
#endif
#ifndef BLD_TYPE_RES_SPEC_PTR_OFF
#define BLD_TYPE_RES_SPEC_PTR_OFF           0x3A0
#endif
#ifndef RES_SPEC_MAX_FLATS_FLOAT_OFF
#define RES_SPEC_MAX_FLATS_FLOAT_OFF        0x08C
#endif
#ifndef CIT_EDUCATION_FLOAT_OFF
#define CIT_EDUCATION_FLOAT_OFF             0x0A8
#endif
#ifndef CIT_CHILD_PTR_OFF
#define CIT_CHILD_PTR_OFF                   0x0B0
#endif
#ifndef CIT_PARENT_PTR_OFF
#define CIT_PARENT_PTR_OFF                  0x0C8
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
static_assert(VEH_CHILDREN_END_OFFSET == VEH_CHILDREN_BEGIN_OFFSET + 8,
              "VEH_CHILDREN_END_OFFSET must be 8 bytes after VEH_CHILDREN_BEGIN_OFFSET");
static_assert(VEH_STEER_TARGET_OFFSET == VEH_STEER_CURRENT_OFFSET + 4 &&
              VEH_WHEELS_END_OFFSET == VEH_WHEELS_BEGIN_OFFSET + 8 &&
              VEH_WHEEL_STRUCT_STRIDE == 24,
              "Vehicle steering (+0x115C/+0x1160) and wheel vector (+0x1438, stride 24) must match 0x6712B0");
static_assert(CAM_MGR_FOLLOW_WIN_PTR_OFF == CAM_MGR_RIDE_VEH_PTR_OFF + 8 &&
              FOLLOW_WIN_VEHICLE_PTR_OFF == FOLLOW_WIN_CITIZEN_PTR_OFF + 8,
              "Camera manager follow/ride offsets must match 0x10D30 / 0x14250");
static_assert(CAM_MGR_POS_OFFSET == 0x08 && CAM_MGR_ANGLES_OFFSET == 0x14 &&
              CAM_MGR_CUR_POS_OFFSET == 0x20 && CAM_MGR_CUR_ANGLES_OFFSET == 0x2C &&
              CAM_MGR_TRANS_STEP_OFFSET == 0x14C && CAM_MGR_TRANS_TIMER_OFFSET == 0x15C &&
              CAM_MGR_TRANS_DUR_OFFSET == 0x160,
              "Camera manager position and transition offsets must match 0x14250 layout");
static_assert(RVA_SPAWN_CITIZEN == 0x823470 &&
              BLD_CITIZENS_END_OFF == BLD_CITIZENS_BEGIN_OFF + 8 &&
              WORLD_STAT_CURRENT_OFFSET + STAT_IMMIGRANT_SPEND_OFF == 0x11568 &&
              WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_COUNTS_OFF + 12 == 0x1157C,
              "Invite citizens (0x6F8CC0 -> 0x823470) offsets must match 1.1.1.9 layout");
static_assert(WORLD_SOVIET_BORDER_BLD_OFF == WORLD_WESTERN_BORDER_BLD_OFF + 8 &&
              VEH_STOP_RECORD_STRIDE == 0x198 &&
              VEH_STOP_HALF_STRIDE * 2 + 8 == VEH_STOP_RECORD_STRIDE,
              "Vehicle window border stop & stop record offsets must match 0x7D6B00 layout");

// ---------------------------------------------------------------------------
// Remote Command Sound, Warning Banner & Notification Popup Isolation Constants
// ---------------------------------------------------------------------------

#ifndef IMPORT_PLAY_SOUND_RVA
#define IMPORT_PLAY_SOUND_RVA           0x86D2A8
#endif
#ifndef IMPORT_PLAY_SOUND_3D_RVA
#define IMPORT_PLAY_SOUND_3D_RVA        0x86CBD8u
#endif
#ifndef SOUND_MUTE_BYTE_RVA
#define SOUND_MUTE_BYTE_RVA             0x9D4F24
#endif
#ifndef CAMERA_EYE_RVA
#define CAMERA_EYE_RVA                  0x9E9D10u
#endif
#ifndef WORLD_OPEN_WINDOWS_BEGIN_OFFSET
#define WORLD_OPEN_WINDOWS_BEGIN_OFFSET 0xCAC0u
#endif
#ifndef WORLD_OPEN_WINDOWS_END_OFFSET
#define WORLD_OPEN_WINDOWS_END_OFFSET   0xCAC8u
#endif
#ifndef WORLD_OPEN_WINDOWS_BEGIN_RVA
#define WORLD_OPEN_WINDOWS_BEGIN_RVA    (WORLD_RVA + WORLD_OPEN_WINDOWS_BEGIN_OFFSET)
#endif
#ifndef WORLD_OPEN_WINDOWS_END_RVA
#define WORLD_OPEN_WINDOWS_END_RVA      (WORLD_RVA + WORLD_OPEN_WINDOWS_END_OFFSET)
#endif
#ifndef WORLD_NOTIF_POPUPS_BEGIN_OFFSET
#define WORLD_NOTIF_POPUPS_BEGIN_OFFSET 0xD208u
#endif
#ifndef WORLD_NOTIF_POPUPS_END_OFFSET
#define WORLD_NOTIF_POPUPS_END_OFFSET   0xD210u
#endif
#ifndef WORLD_BRUSH_SOUNDS_BEGIN_OFFSET
#define WORLD_BRUSH_SOUNDS_BEGIN_OFFSET 0xD488u
#endif
#ifndef WORLD_BRUSH_SOUNDS_END_OFFSET
#define WORLD_BRUSH_SOUNDS_END_OFFSET   0xD490u
#endif
#ifndef BRUSH_SOUND_REC_SIZE
#define BRUSH_SOUND_REC_SIZE            0x110u
#endif
#ifndef BRUSH_SOUND_VOL_OFFSET
#define BRUSH_SOUND_VOL_OFFSET          0x108u
#endif
#ifndef BRUSH_SOUND_ACTIVE_OFFSET
#define BRUSH_SOUND_ACTIVE_OFFSET       0x10Cu
#endif
#ifndef WORLD_WARNING_TEXT_OFFSET
#define WORLD_WARNING_TEXT_OFFSET       0xD5A0u
#endif
#ifndef WORLD_WARNING_TEXT_BYTES
#define WORLD_WARNING_TEXT_BYTES        0x2000u
#endif
#ifndef WORLD_WARNING_TIMER_OFFSET
#define WORLD_WARNING_TIMER_OFFSET      0xF5A0u
#endif
#ifndef WORLD_PLACEMENT_ERROR_OFFSET
#define WORLD_PLACEMENT_ERROR_OFFSET    0x11A10u
#endif
#ifndef NOTIF_MSG_POS_X_OFFSET
#define NOTIF_MSG_POS_X_OFFSET          0x4200u
#endif
#ifndef NOTIF_MSG_POS_Z_OFFSET
#define NOTIF_MSG_POS_Z_OFFSET          0x4208u
#endif
#ifndef NOTIFICATION_REBUILD_POPUPS_RVA
#define NOTIFICATION_REBUILD_POPUPS_RVA 0x4C4A40u
#endif
#ifndef WARNING_POPUP_CREATE_RVA
#define WARNING_POPUP_CREATE_RVA        0x6EE2A0u
#endif
#ifndef REMOTE_3D_SOUND_MAX_DIST
#define REMOTE_3D_SOUND_MAX_DIST        250.0f
#endif

static_assert(CAMERA_EYE_RVA == CAMERA_RVA + CAMERA_POS_OFFSET,
              "CAMERA_EYE_RVA (0x9E9D10) must equal CAMERA_RVA (0x9E9C90) + CAMERA_POS_OFFSET (0x80)");
static_assert(WORLD_OPEN_WINDOWS_BEGIN_RVA == WORLD_RVA + WORLD_OPEN_WINDOWS_BEGIN_OFFSET &&
              WORLD_OPEN_WINDOWS_END_RVA == WORLD_OPEN_WINDOWS_BEGIN_RVA + 8,
              "WORLD_OPEN_WINDOWS_BEGIN/END_RVA (0x9E19D0/0x9E19D8) must match WORLD_RVA + 0xCAC0/0xCAC8");
static_assert(WORLD_NOTIF_POPUPS_END_OFFSET == WORLD_NOTIF_POPUPS_BEGIN_OFFSET + 8 &&
              WORLD_BRUSH_SOUNDS_END_OFFSET == WORLD_BRUSH_SOUNDS_BEGIN_OFFSET + 8 &&
              BRUSH_SOUND_ACTIVE_OFFSET == BRUSH_SOUND_VOL_OFFSET + 4 &&
              BRUSH_SOUND_REC_SIZE == 0x110u,
              "Notification popups (+0xD208..+0xD210) and brush loop sound records (+0xD488..+0xD490, 0x110 stride) must match 1.1.1.9 layout");
static_assert(WORLD_WARNING_TIMER_OFFSET == WORLD_WARNING_TEXT_OFFSET + WORLD_WARNING_TEXT_BYTES &&
              NOTIF_MSG_POS_Z_OFFSET == NOTIF_MSG_POS_X_OFFSET + 8,
              "Warning banner timer (+0xF5A0) must follow 0x2000-byte warning text (+0xD5A0), and notification pos Z (+0x4208) must be 8 bytes after pos X (+0x4200)");

// ---------------------------------------------------------------------------
// Building Resource Purchase (Manual One-Time Buy & Auto-Purchase) Constants
// ---------------------------------------------------------------------------

#ifndef WORLD_UI_RES_SLIDERS_OFFSET
#define WORLD_UI_RES_SLIDERS_OFFSET     0x11F28u
#endif
#ifndef BLD_STORAGE_VEC_CLEAR_RVA
#define BLD_STORAGE_VEC_CLEAR_RVA       0x0B11F0u
#endif
#ifndef BLD_INIT_RES_SLIDERS_RVA
#define BLD_INIT_RES_SLIDERS_RVA        0x174B30u
#endif
#ifndef BLD_APPLY_AUTO_BUY_RVA
#define BLD_APPLY_AUTO_BUY_RVA          0x174D40u
#endif
#ifndef BLD_MANUAL_BUY_RES_RVA
#define BLD_MANUAL_BUY_RES_RVA          0x174E40u
#endif
#ifndef SITE_BLD_MANUAL_BUY_RES_RVA
#define SITE_BLD_MANUAL_BUY_RES_RVA     0x711A4Bu
#endif
#ifndef BLD_AUTO_BUY_MODE_OFFSET
#define BLD_AUTO_BUY_MODE_OFFSET        0x9E8u
#endif
#ifndef BLD_EXPORT_MODE_OFFSET
#define BLD_EXPORT_MODE_OFFSET          0x9ECu
#endif
#ifndef RES_PRICE_USD_OFFSET
#define RES_PRICE_USD_OFFSET            0x58u
#endif
#ifndef RES_PRICE_RUB_OFFSET
#define RES_PRICE_RUB_OFFSET            0x5Cu
#endif

static_assert(BLD_EXPORT_MODE_OFFSET == BLD_AUTO_BUY_MODE_OFFSET + 4 &&
              RES_PRICE_RUB_OFFSET == RES_PRICE_USD_OFFSET + 4 &&
              BLD_MANUAL_BUY_RES_RVA == BLD_APPLY_AUTO_BUY_RVA + 0x100u,
              "Building resource purchase offsets (+0x9E8/+0x9EC, +0x58/+0x5C, 0x174D40/0x174E40) must match 1.1.1.9 layout");

// ---------------------------------------------------------------------------
// C3D Material IAT RVAs (SOVIET64.exe Import Address Table for C3DDLL64.dll)
// ---------------------------------------------------------------------------

#ifndef RVA_IAT_SAVE_ORIGINAL_VALUES
#define RVA_IAT_SAVE_ORIGINAL_VALUES        0x86D578
#endif
#ifndef RVA_IAT_CHANGE_VALUES_TO
#define RVA_IAT_CHANGE_VALUES_TO            0x86D568
#endif
#ifndef RVA_IAT_RESTORE_ORIGINAL_VALUES
#define RVA_IAT_RESTORE_ORIGINAL_VALUES     0x86D570
#endif

static_assert(RVA_IAT_SAVE_ORIGINAL_VALUES == 0x86D578, "RVA_IAT_SAVE_ORIGINAL_VALUES mismatch");
static_assert(RVA_IAT_CHANGE_VALUES_TO == 0x86D568, "RVA_IAT_CHANGE_VALUES_TO mismatch");
static_assert(RVA_IAT_RESTORE_ORIGINAL_VALUES == 0x86D570, "RVA_IAT_RESTORE_ORIGINAL_VALUES mismatch");

// ---------------------------------------------------------------------------
// University & Research Tree Constants (SOVIET64.exe PE 6A3EB6AD)
// ---------------------------------------------------------------------------

#ifndef WORLD_RESEARCH_UNLOCKS_OFFSET
#define WORLD_RESEARCH_UNLOCKS_OFFSET       0x1175Cu
#endif
#ifndef WORLD_RESEARCH_UNLOCKS_BYTES
#define WORLD_RESEARCH_UNLOCKS_BYTES        0x16u
#endif
#ifndef WORLD_RESEARCH_VEC_BEGIN_OFFSET
#define WORLD_RESEARCH_VEC_BEGIN_OFFSET     0x11778u
#endif
#ifndef WORLD_RESEARCH_VEC_END_OFFSET
#define WORLD_RESEARCH_VEC_END_OFFSET       0x11780u
#endif
#ifndef RESEARCH_ITEM_STRIDE
#define RESEARCH_ITEM_STRIDE                0xE8u
#endif
#ifndef RESEARCH_ITEM_NAME_OFFSET
#define RESEARCH_ITEM_NAME_OFFSET           0x00u
#endif
#ifndef RESEARCH_ITEM_SUBTYPE_OFFSET
#define RESEARCH_ITEM_SUBTYPE_OFFSET        0x40u
#endif
#ifndef RESEARCH_ITEM_INITIAL_OFFSET
#define RESEARCH_ITEM_INITIAL_OFFSET        0x50u
#endif
#ifndef RESEARCH_ITEM_REQ_WORK_OFFSET
#define RESEARCH_ITEM_REQ_WORK_OFFSET       0x54u
#endif
#ifndef RESEARCH_ITEM_AVAILABLE_OFFSET
#define RESEARCH_ITEM_AVAILABLE_OFFSET      0xCCu
#endif
#ifndef RESEARCH_ITEM_PROGRESS_OFFSET
#define RESEARCH_ITEM_PROGRESS_OFFSET       0xD0u
#endif
#ifndef RESEARCH_ITEM_BUILDING_PTR_OFFSET
#define RESEARCH_ITEM_BUILDING_PTR_OFFSET   0xD8u
#endif
#ifndef RESEARCH_ITEM_BLD_PTR_OFFSET
#define RESEARCH_ITEM_BLD_PTR_OFFSET        RESEARCH_ITEM_BUILDING_PTR_OFFSET
#endif
#ifndef RESEARCH_ITEM_TIMER_E0_OFFSET
#define RESEARCH_ITEM_TIMER_E0_OFFSET       0xE0u
#endif
#ifndef BLD_UNIV_STAFF_RATIO_OFFSET
#define BLD_UNIV_STAFF_RATIO_OFFSET         0x11A8u
#endif
#ifndef RVA_BLD_UNIV_STEP
#define RVA_BLD_UNIV_STEP                   0x17DCF0u
#endif
#ifndef RVA_RESEARCH_DAILY_UPDATE
#define RVA_RESEARCH_DAILY_UPDATE           0x2F6BD0u
#endif
#ifndef RVA_RESEARCH_RECOMPUTE
#define RVA_RESEARCH_RECOMPUTE              0x2F6C90u
#endif
#ifndef RVA_RESEARCH_FIND_BY_BLD
#define RVA_RESEARCH_FIND_BY_BLD            0x2F7B10u
#endif
#ifndef RVA_UNIV_RESEARCH_PANEL
#define RVA_UNIV_RESEARCH_PANEL             0x2F7ED0u
#endif

static_assert(WORLD_RESEARCH_VEC_END_OFFSET == WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8 &&
              RESEARCH_ITEM_STRIDE == 0xE8u &&
              RESEARCH_ITEM_PROGRESS_OFFSET == RESEARCH_ITEM_AVAILABLE_OFFSET + 4 &&
              RESEARCH_ITEM_BUILDING_PTR_OFFSET == RESEARCH_ITEM_PROGRESS_OFFSET + 8 &&
              RESEARCH_ITEM_TIMER_E0_OFFSET == RESEARCH_ITEM_BUILDING_PTR_OFFSET + 8,
              "ResearchItem layout (+0x11778, stride 0xE8, +0xCC/+0xD0/+0xD8/+0xE0) must match 0x2F6C90 / 0x2F7ED0");

// ---------------------------------------------------------------------------
// Construction & Demolition Office Assignment RVAs (SOVIET64.exe PE 6A3EB6AD)
// ---------------------------------------------------------------------------

#ifndef WORLD_ACTIVE_WINDOW_BLD_OFF
#define WORLD_ACTIVE_WINDOW_BLD_OFF         0xD298u
#endif
#ifndef RVA_CALLER_OFFICE_ROAD_SCAN
#define RVA_CALLER_OFFICE_ROAD_SCAN         0x14119Bu
#endif
#ifndef RVA_ASSIGN_CONSTRUCTION_OFFICE
#define RVA_ASSIGN_CONSTRUCTION_OFFICE      0x161C90u
#endif
#ifndef RVA_UNASSIGN_CONSTRUCTION_OFFICE
#define RVA_UNASSIGN_CONSTRUCTION_OFFICE    0x162360u
#endif
#ifndef RVA_START_PLANNED_CONSTRUCTION
#define RVA_START_PLANNED_CONSTRUCTION      0x1A8D70u
#endif
#ifndef SITE_COPY_OFFICE_ASSIGN_NET_RVA
#define SITE_COPY_OFFICE_ASSIGN_NET_RVA     0x1D70EEu
#endif
#ifndef SITE_COPY_OFFICE_ASSIGN_BLD_RVA
#define SITE_COPY_OFFICE_ASSIGN_BLD_RVA     0x1D83C2u
#endif
#ifndef SITE_CONSTRUCT_TOOL_START_PLAN_RVA
#define SITE_CONSTRUCT_TOOL_START_PLAN_RVA  0x1D9FFDu
#endif
#ifndef SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA
#define SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA  0x1DA02Au
#endif
#ifndef SITE_CONSTRUCT_TOOL_ASSIGN_BLD_RVA
#define SITE_CONSTRUCT_TOOL_ASSIGN_BLD_RVA  0x1DA48Du
#endif

static_assert(RVA_UNASSIGN_CONSTRUCTION_OFFICE == RVA_ASSIGN_CONSTRUCTION_OFFICE + 0x6D0u &&
              RVA_CALLER_OFFICE_ROAD_SCAN < RVA_ASSIGN_CONSTRUCTION_OFFICE &&
              SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA == SITE_CONSTRUCT_TOOL_START_PLAN_RVA + 0x2Du,
              "Construction Office assignment RVAs (0x161C90, 0x162360, 0x14119B, 0x1D9FFD/0x1DA02A) must match 1.1.1.9 layout");

static_assert(VEH_TYPE_SCRIPT_NAME_OFFSET == 0x200u &&
              VEH_TYPE_CATEGORY_OFFSET == 0x294u &&
              VEH_TYPE_HAS_LICENCE_OFFSET == 0x9C88u &&
              VEH_TYPE_HAS_LICENCE_OFFSET < WORLD_VEHICLE_TYPE_STRIDE,
              "VEHICLE_TYPE license & script offsets (+0x200, +0x294, +0x9C88, stride 0x9F08) must match 0x3DDA80 / 0x76D81E");


// ---------------------------------------------------------------------------
// City Creation/Deletion, On-the-Fly Rename & Car Dealer Filter Offsets
// ---------------------------------------------------------------------------

#ifndef CITY_CREATE_RVA
#define CITY_CREATE_RVA                     0x32F450u
#endif
#ifndef CITY_DELETE_RVA
#define CITY_DELETE_RVA                     0x32F5C0u
#endif
#ifndef CITY_REASSIGN_BLDS_RVA
#define CITY_REASSIGN_BLDS_RVA              0x32FF40u
#endif
#ifndef REBUILD_STOP_LINE_NAMES_RVA
#define REBUILD_STOP_LINE_NAMES_RVA         0x4963D0u
#endif
#ifndef WORLD_RENAME_REFRESH_SRC_OFF
#define WORLD_RENAME_REFRESH_SRC_OFF        0xC6B0u
#endif
#ifndef WORLD_RENAME_REFRESH_DST_OFF
#define WORLD_RENAME_REFRESH_DST_OFF        0xC6B8u
#endif
#ifndef BLD_CITIZEN_FILTER_AT
#define BLD_CITIZEN_FILTER_AT               0xCD8u
#endif
#ifndef BLD_CITIZEN_FILTER_END
#define BLD_CITIZEN_FILTER_END              0xCE8u
#endif
#ifndef LINE_CUSTOM_NAME_FLAG_OFF
#define LINE_CUSTOM_NAME_FLAG_OFF           0x01u
#endif
#ifndef LINE_NAME_OFFSET
#define LINE_NAME_OFFSET                    0x04u
#endif
#ifndef SITE_RENAME_AFTER_CALL_RVA
#define SITE_RENAME_AFTER_CALL_RVA          0x8018D5u
#endif
#ifndef SITE_RENAME_SKIP_MUTATION_RVA
#define SITE_RENAME_SKIP_MUTATION_RVA       0x8019CEu
#endif
#ifndef VEH_CUSTOM_NAME_FLAG_OFF
#define VEH_CUSTOM_NAME_FLAG_OFF            0x108u
#endif
#ifndef VEH_NAME_OFFSET
#define VEH_NAME_OFFSET                     0x10Au
#endif
#ifndef VEH_NAME_END_OFFSET
#define VEH_NAME_END_OFFSET                 ((0x10Au + 0x80u + 3u) & ~3u)
#endif

static_assert(CITY_DELETE_RVA == CITY_CREATE_RVA + 0x170u &&
              CITY_REASSIGN_BLDS_RVA == CITY_CREATE_RVA + 0xAF0u &&
              BLD_CITIZEN_FILTER_END == BLD_CITIZEN_FILTER_AT + 0x10u &&
              VEH_NAME_END_OFFSET == 0x18Cu,
              "City creation/rename RVAs and building/vehicle name/filter offsets must match 1.1.1.9 layout");
static_assert(WORLD_TRACKER_BLOCK_OFFSET + WORLD_TRACKER_BLOCK_SIZE == WORLD_LOANS_OFFSET &&
              WORLD_TRACKER_HEALTH_CUR_CNT_OFF == WORLD_TRACKER_HEALTH_CUR_SUM_OFF + 12 * sizeof(float) &&
              WORLD_TRACKER_HEALTH_OLD_SUM_OFF == WORLD_TRACKER_HEALTH_CUR_CNT_OFF + 12 * sizeof(int32_t) &&
              WORLD_TRACKER_HEALTH_OLD_CNT_OFF == WORLD_TRACKER_HEALTH_OLD_SUM_OFF + 12 * sizeof(float) &&
              WORLD_TRACKER_HAPPY_CUR_SUM_OFF == WORLD_TRACKER_HEALTH_OLD_CNT_OFF + 12 * sizeof(int32_t) &&
              WORLD_TRACKER_HAPPY_CUR_CNT_OFF == WORLD_TRACKER_HAPPY_CUR_SUM_OFF + 23 * sizeof(float) &&
              WORLD_TRACKER_HAPPY_OLD_SUM_OFF == WORLD_TRACKER_HAPPY_CUR_CNT_OFF + 23 * sizeof(int32_t) &&
              WORLD_TRACKER_HAPPY_OLD_CNT_OFF == WORLD_TRACKER_HAPPY_OLD_SUM_OFF + 23 * sizeof(float) &&
              WORLD_TRACKER_HAPPY_OLD_CNT_OFF + 23 * sizeof(int32_t) == WORLD_LOANS_OFFSET,
              "Health & Happiness Tracker block (+0x109A0..+0x10BCF, 0x230 bytes) must match 0x2FC010 / 0x8C3C0 / 0x8D490");

// ---------------------------------------------------------------------------
// Native Minimap & HUD Quad / Font Rendering RVAs (SOVIET64.exe PE 6A3EB6AD)
// ---------------------------------------------------------------------------

#ifndef RVA_HUD_DRAW_BTN_FRAME
#define RVA_HUD_DRAW_BTN_FRAME              0x4461F0u
#endif
#ifndef RVA_MINIMAP_DRAW_CONTENT
#define RVA_MINIMAP_DRAW_CONTENT            0x4BDE80u
#endif
#ifndef RVA_MINIMAP_DRAW_BUTTONS
#define RVA_MINIMAP_DRAW_BUTTONS            0x4BFF40u
#endif
#ifndef WORLD_MINIMAP_PTR_OFFSET
#define WORLD_MINIMAP_PTR_OFFSET            0xD400u
#endif
#ifndef MINIMAP_TOOLTIP_WSTR_RVA
#define MINIMAP_TOOLTIP_WSTR_RVA            0x9E24B0u
#endif
#ifndef HUD_QUAD_STAGE_RVA
#define HUD_QUAD_STAGE_RVA                  0x9BE060u
#endif
#ifndef HUD_QUAD_SIZE_X_RVA
#define HUD_QUAD_SIZE_X_RVA                 0x9BE2E8u
#endif
#ifndef HUD_QUAD_SIZE_Y_RVA
#define HUD_QUAD_SIZE_Y_RVA                 0x9BE2ECu
#endif
#ifndef HUD_QUAD_POS_X_RVA
#define HUD_QUAD_POS_X_RVA                  0x9BE2F0u
#endif
#ifndef HUD_QUAD_POS_Y_RVA
#define HUD_QUAD_POS_Y_RVA                  0x9BE2F4u
#endif
#ifndef HUD_QUAD_Z_RVA
#define HUD_QUAD_Z_RVA                      0x9BE2F8u
#endif
#ifndef HUD_QUAD_ROT_RVA
#define HUD_QUAD_ROT_RVA                    0x9BE308u
#endif
#ifndef HUD_QUAD_COLOR_RVA
#define HUD_QUAD_COLOR_RVA                  0x9BE30Cu
#endif
#ifndef HUD_UI_SCALE_RVA
#define HUD_UI_SCALE_RVA                    0x992088u
#endif
#ifndef HUD_SCREEN_HEIGHT_RVA
#define HUD_SCREEN_HEIGHT_RVA               0x99206Cu
#endif
#ifndef HUD_RENDERER_PTR_RVA
#define HUD_RENDERER_PTR_RVA                0x9EAD08u
#endif
#ifndef HUD_TEX_BIND_VTABLE_OFF
#define HUD_TEX_BIND_VTABLE_OFF             0x70u
#endif
#ifndef HUD_SOLID_TEX_PTR_RVA
#define HUD_SOLID_TEX_PTR_RVA               0x9E0B38u
#endif
#ifndef HUD_BTN_FRAME_TEX_PTR_RVA
#define HUD_BTN_FRAME_TEX_PTR_RVA           0x9DFF38u
#endif
#ifndef HUD_PIN_ICON_TEX_PTR_RVA
#define HUD_PIN_ICON_TEX_PTR_RVA            0x9E03E8u
#endif
#ifndef HUD_MAP_CAMERA_TEX_PTR_RVA
#define HUD_MAP_CAMERA_TEX_PTR_RVA          0x9E07C0u
#endif
#ifndef MINIMAP_CAMERA_ICON_MAP_FRACTION
#define MINIMAP_CAMERA_ICON_MAP_FRACTION    0.15f   // exe+0x909CA0
#endif
#ifndef MINIMAP_CAMERA_ICON_MAX_PX
#define MINIMAP_CAMERA_ICON_MAX_PX          70.0f   // exe+0x90AAB8 (x uiScale)
#endif
#ifndef HUD_FONT_MAIN_PTR_RVA
#define HUD_FONT_MAIN_PTR_RVA               0x994208u
#endif
#ifndef HUD_TEXT_MGR_RVA
#define HUD_TEXT_MGR_RVA                    0x996FB0u
#endif
#ifndef HUD_STYLE_BLOCK_RVA
#define HUD_STYLE_BLOCK_RVA                 0x9DFCC0u
#endif
#ifndef HUD_MIDDLEPOINT_OBJ_RVA
#define HUD_MIDDLEPOINT_OBJ_RVA             0x9EACD0u
#endif
#ifndef HUD_FMT_WSTR_RVA
#define HUD_FMT_WSTR_RVA                    0x8B5528u
#endif
#ifndef HUD_BASIC_PASS_STR_RVA
#define HUD_BASIC_PASS_STR_RVA              0x8D8464u
#endif
#ifndef HUD_MOUSE_STATE_RVA
#define HUD_MOUSE_STATE_RVA                 0xA54B90u
#endif
#ifndef HUD_MOUSE_LCLICK_EDGE_RVA
#define HUD_MOUSE_LCLICK_EDGE_RVA           0xA54E91u
#endif
#ifndef RVA_IAT_CREATE_MANAGED_TEX
#define RVA_IAT_CREATE_MANAGED_TEX          0x86C4C0u
#endif
#ifndef RVA_IAT_HUD_HIT_TEST_QUAD
#define RVA_IAT_HUD_HIT_TEST_QUAD           0x86C850u
#endif
#ifndef RVA_IAT_HUD_DRAW_QUAD
#define RVA_IAT_HUD_DRAW_QUAD               0x86C860u
#endif
#ifndef RVA_IAT_HUD_GET_MOUSE_POS
#define RVA_IAT_HUD_GET_MOUSE_POS           0x86C868u
#endif
#ifndef RVA_IAT_TEXT_PRINT_ALL
#define RVA_IAT_TEXT_PRINT_ALL              0x86C870u
#endif
#ifndef RVA_IAT_TEXT_DRAW_W
#define RVA_IAT_TEXT_DRAW_W                 0x86C880u
#endif
#ifndef RVA_IAT_HUD_END_DRAW
#define RVA_IAT_HUD_END_DRAW                0x86C8F8u
#endif
#ifndef RVA_IAT_HUD_BEGIN_DRAW
#define RVA_IAT_HUD_BEGIN_DRAW              0x86C918u
#endif

static_assert(RVA_MINIMAP_DRAW_BUTTONS == RVA_MINIMAP_DRAW_CONTENT + 0x20C0u &&
              HUD_SOLID_TEX_PTR_RVA == 0x9E0B38u &&
              HUD_MAP_CAMERA_TEX_PTR_RVA == WORLD_RVA + 0xB8B0u &&
              HUD_QUAD_POS_X_RVA == HUD_QUAD_SIZE_X_RVA + 8u &&
              HUD_QUAD_COLOR_RVA == HUD_QUAD_SIZE_X_RVA + 0x24u &&
              RVA_IAT_HUD_DRAW_QUAD == RVA_IAT_HUD_HIT_TEST_QUAD + 0x10u,
              "Minimap DrawContent/DrawButtons RVAs (0x4BDE80, 0x4BFF40), camera texture and C3D_HUD quad offsets must match 1.1.1.9 layout");

// Building working range & auto-search settings (FUN_1401edc40 / FUN_140581080 / FUN_14074e040)
#ifndef BLD_RECONSTRUCT_THRESHOLDS_OFFSET
#define BLD_RECONSTRUCT_THRESHOLDS_OFFSET   0xE50u  // 8 bytes: float bld_thresh (+0xE50), float mach_thresh (+0xE54)
#endif
#ifndef BLD_SEARCH_RANGE_OFFSET
#define BLD_SEARCH_RANGE_OFFSET             0xFC8u  // 4 bytes: int32_t working range / auto-search distance / repair threshold
#endif
#ifndef BLD_AUTOSEARCH_FLAGS_OFFSET
#define BLD_AUTOSEARCH_FLAGS_OFFSET         0x11BCu // 8 bytes (0x11BC..0x11C3): auto-search / residence / crop / repair flags
#endif
#ifndef BLD_AUTOSEARCH_WIRES_OFFSET
#define BLD_AUTOSEARCH_WIRES_OFFSET         0x11C4u // 1 byte: auto-search power/wires flag
#endif
#ifndef BLD_COLLECT_FLAGS_OFFSET
#define BLD_COLLECT_FLAGS_OFFSET            0x1271u // 2 bytes (0x1271..0x1272): container/vehicle collection flags
#endif

static_assert(BLD_RECONSTRUCT_THRESHOLDS_OFFSET == 0xE50u &&
              BLD_SEARCH_RANGE_OFFSET == 0xFC8u &&
              BLD_AUTOSEARCH_FLAGS_OFFSET == 0x11BCu &&
              BLD_AUTOSEARCH_WIRES_OFFSET == BLD_AUTOSEARCH_FLAGS_OFFSET + 8u &&
              BLD_COLLECT_FLAGS_OFFSET == 0x1271u,
              "Building search range and auto-search flag offsets must match 1.1.1.9 BUILDING layout");

// Building TV/Radio studio, Upbringing/Rehabilitation, staff loyalty, and notification alert offsets
#ifndef WORLD_STUDIOS_BEGIN_OFFSET
#define WORLD_STUDIOS_BEGIN_OFFSET          0xD230u // std::vector<Studio*> begin pointer
#endif
#ifndef WORLD_STUDIOS_END_OFFSET
#define WORLD_STUDIOS_END_OFFSET            0xD238u // std::vector<Studio*> end pointer
#endif
#ifndef STUDIO_BUILDING_OFFSET
#define STUDIO_BUILDING_OFFSET              0x150u  // Studio->building back-pointer
#endif
#ifndef STUDIO_BROADCAST_SLIDERS_OFFSET
#define STUDIO_BROADCAST_SLIDERS_OFFSET     0x138u  // 6 floats (24 bytes): alcohol, sports, culture, education, propaganda, anti-religion
#endif
#ifndef BLD_UPBRINGING_SLIDERS_OFFSET
#define BLD_UPBRINGING_SLIDERS_OFFSET       0x13A0u // 6 floats (24 bytes): orphanage / prison rehabilitation sliders
#endif
#ifndef BLD_MIN_LOYALTY_EMPLOYEES_OFFSET
#define BLD_MIN_LOYALTY_EMPLOYEES_OFFSET    0x13D8u // 4 bytes: float minimum employee / warden / tutor loyalty
#endif
#ifndef BLD_MIN_LOYALTY_CITIZENS_OFFSET
#define BLD_MIN_LOYALTY_CITIZENS_OFFSET     0x13E0u // 4 bytes: float minimum student / citizen / visitor loyalty
#endif
#ifndef BLD_ALERT_GOODS_MISSING_OFFSET
#define BLD_ALERT_GOODS_MISSING_OFFSET      0x126Du // 1 byte: uint8_t notify goods missing
#endif
#ifndef BLD_ALERT_MAIN_TOGGLE_OFFSET
#define BLD_ALERT_MAIN_TOGGLE_OFFSET        0x1270u // 1 byte: uint8_t main alert toggle (production stop, tutors, wardens, police, court)
#endif
#ifndef BLD_ALERT_FINISHED_OFFSET
#define BLD_ALERT_FINISHED_OFFSET           0x0E10u // 1 byte: uint8_t notify finished (construction office / site)
#endif
#ifndef BLD_POLLUTION_ALERT_TOGGLE_OFFSET
#define BLD_POLLUTION_ALERT_TOGGLE_OFFSET   0x0DFCu // 4 bytes: int32_t pollution monitoring alert toggle ("Notify if above")
#endif
#ifndef BLD_POLLUTION_ALERT_THRESH_OFFSET
#define BLD_POLLUTION_ALERT_THRESH_OFFSET   0x0E3Cu // 4 bytes: float pollution monitoring alert threshold slider
#endif

static_assert(WORLD_STUDIOS_BEGIN_OFFSET == 0xD230u &&
              WORLD_STUDIOS_END_OFFSET == 0xD238u &&
              STUDIO_BUILDING_OFFSET == 0x150u &&
              STUDIO_BROADCAST_SLIDERS_OFFSET == 0x138u &&
              BLD_UPBRINGING_SLIDERS_OFFSET == 0x13A0u &&
              BLD_MIN_LOYALTY_EMPLOYEES_OFFSET == 0x13D8u &&
              BLD_MIN_LOYALTY_CITIZENS_OFFSET == 0x13E0u &&
              BLD_ALERT_GOODS_MISSING_OFFSET == 0x126Du &&
              BLD_ALERT_MAIN_TOGGLE_OFFSET == 0x1270u &&
              BLD_ALERT_FINISHED_OFFSET == 0x0E10u &&
              BLD_POLLUTION_ALERT_TOGGLE_OFFSET == 0x0DFCu &&
              BLD_POLLUTION_ALERT_THRESH_OFFSET == 0x0E3Cu,
              "Studio broadcast, upbringing sliders, staff loyalty, and alert checkbox offsets must match 1.1.1.9 layout");







