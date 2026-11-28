// SPDX-License-Identifier: GPL-3.0-only
// Canonical RVA addresses and structure offsets for SOVIET64.exe (v1.2.0.9, PE 6B05A574).
// Centralizes engine memory constants and enforces compile-time invariants from AGENTS.md.
#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Global Engine Objects (RVAs from g_exeBase)
// ---------------------------------------------------------------------------

#ifndef WORLD_RVA
#define WORLD_RVA                   0xA1EF20
#endif
#ifndef WORLD_THIS_RVA
#define WORLD_THIS_RVA              WORLD_RVA
#endif
#ifndef UNIVERSAL_THIS_RVA
#define UNIVERSAL_THIS_RVA          WORLD_RVA
#endif

#ifndef ENGINE_RVA
#define ENGINE_RVA                  0x9DDC88
#endif
#ifndef ENGINE_TERRAIN_OFFSET
#define ENGINE_TERRAIN_OFFSET       0xF78
#endif

#ifndef CAMERA_RVA
#define CAMERA_RVA                  0xA33D50
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
#define LOAD_TRIGGER_DAT_RVA        0x9DEC9A
#endif
#ifndef WINDOW_FOCUS_BYTE_RVA
#define WINDOW_FOCUS_BYTE_RVA       0x9DC089
#endif
#ifndef TERRAIN_FLAG_DAT_RVA
#define TERRAIN_FLAG_DAT_RVA        0x9DDC77
#endif
#ifndef SAVE_NAME_DAT_RVA
#define SAVE_NAME_DAT_RVA           0x9DECA0
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
#ifndef WORLD_FAST_RESEARCH_CHEAT_OFFSET
#define WORLD_FAST_RESEARCH_CHEAT_OFFSET  0x14501u
#endif
#ifndef WORLD_FAST_CONSTRUCT_CHEAT_OFFSET
#define WORLD_FAST_CONSTRUCT_CHEAT_OFFSET 0x14502u
#endif
#ifndef WORLD_MAP_EDITOR_MODE_OFFSET
#define WORLD_MAP_EDITOR_MODE_OFFSET      0x10438u
#endif
static_assert(WORLD_MAP_EDITOR_MODE_OFFSET == 0x10438u,
              "WORLD_MAP_EDITOR_MODE_OFFSET must match v1.2.0.9 map editor toggle");
#ifndef WORLD_CAMERA_MGR_PTR_OFF
#define WORLD_CAMERA_MGR_PTR_OFF    0x0040
#endif
#ifndef CAM_MGR_STRUCT_BYTES
#define CAM_MGR_STRUCT_BYTES        0x188
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
#define CAM_MGR_FOLLOW_ACTIVE_OFF   0x05F
#endif
#ifndef CAM_MGR_RIDE_VEH_PTR_OFF
#define CAM_MGR_RIDE_VEH_PTR_OFF    0x078
#endif
#ifndef CAM_MGR_FOLLOW_WIN_PTR_OFF
#define CAM_MGR_FOLLOW_WIN_PTR_OFF  0x080
#endif
#ifndef CAM_MGR_PID_PTRS_OFFSET
#define CAM_MGR_PID_PTRS_OFFSET     0x118u
#endif
#ifndef CAM_MGR_PID_COUNT
#define CAM_MGR_PID_COUNT           7
#endif
#ifndef CAM_MGR_PID_STRUCT_BYTES
#define CAM_MGR_PID_STRUCT_BYTES    0x78u
#endif
#ifndef CAM_MGR_TRANS_STEP_OFFSET
#define CAM_MGR_TRANS_STEP_OFFSET   0x16C
#endif
#ifndef CAM_MGR_TRANS_TIMER_OFFSET
#define CAM_MGR_TRANS_TIMER_OFFSET  0x17C
#endif
#ifndef CAM_MGR_TRANS_DUR_OFFSET
#define CAM_MGR_TRANS_DUR_OFFSET    0x180
#endif
#ifndef FOLLOW_WIN_CITIZEN_PTR_OFF
#define FOLLOW_WIN_CITIZEN_PTR_OFF  0x058
#endif
#ifndef FOLLOW_WIN_VEHICLE_PTR_OFF
#define FOLLOW_WIN_VEHICLE_PTR_OFF  0x060
#endif
#ifndef CIT_RIDING_VEHICLE_PTR_OFF
#define CIT_RIDING_VEHICLE_PTR_OFF  0x060
#endif
#ifndef RVA_CAMERA_MGR_UPDATE
#define RVA_CAMERA_MGR_UPDATE       0x16DE0
#endif
#ifndef WORLD_UNLIMITED_MONEY_OFFSET
#define WORLD_UNLIMITED_MONEY_OFFSET 0x5A0
#endif
#ifndef WORLD_USD_OFFSET
#define WORLD_USD_OFFSET            0x5A8
#endif
#ifndef WORLD_RUB_OFFSET
#define WORLD_RUB_OFFSET            0x5B0
#endif
#ifndef WORLD_DAY_OFFSET
#define WORLD_DAY_OFFSET            0x5B8
#endif
#ifndef WORLD_YEAR_OFFSET
#define WORLD_YEAR_OFFSET           0x5BC
#endif
#ifndef WORLD_PROGRESS_OFFSET
#define WORLD_PROGRESS_OFFSET       0x5C4
#endif

#ifndef SAVE_NAME_FIELD_OFFSET
#define SAVE_NAME_FIELD_OFFSET      0xA20
#endif
#ifndef SAVE_NAME_FIELD_MAX_LEN
#define SAVE_NAME_FIELD_MAX_LEN     120
#endif

// Invariant #12: world + 0xB08 is a 1-byte citizen option; 0xB0A+ is written by FormatMoney (0x336420).
#ifndef WORLD_CITIZEN_OPT_BYTE_OFF
#define WORLD_CITIZEN_OPT_BYTE_OFF  0xB30
#endif
#ifndef WORLD_FORMAT_MONEY_BUF_OFF
#define WORLD_FORMAT_MONEY_BUF_OFF  0xB32
#endif

#ifndef CURSOR_X_OFFSET
#define CURSOR_X_OFFSET             0xF9C
#endif
#ifndef CURSOR_Y_OFFSET
#define CURSOR_Y_OFFSET             0xFA0
#endif
#ifndef CURSOR_Z_OFFSET
#define CURSOR_Z_OFFSET             0xFA4
#endif
#ifndef WORLD_HIDE_HUD_OFF
#define WORLD_HIDE_HUD_OFF          0xFEA
#endif
#ifndef TOOL_GUARD1_OFFSET
#define TOOL_GUARD1_OFFSET          0xFEC
#endif
#ifndef TOOL_GUARD2_OFFSET
#define TOOL_GUARD2_OFFSET          0xFEE
#endif
#ifndef TOOL_GUARD3_OFFSET
#define TOOL_GUARD3_OFFSET          0xFF3
#endif
#ifndef WORLD_RESOURCES_OFFSET
#define WORLD_RESOURCES_OFFSET      0xC350
#endif
#ifndef WORLD_WORK_RESOURCE_PTR_OFF
#define WORLD_WORK_RESOURCE_PTR_OFF 0xC368
#endif
#ifndef WORLD_SYSTAB_TAB_PTR_OFFSET
#define WORLD_SYSTAB_TAB_PTR_OFFSET 0xC6F0
#endif
#ifndef WORLD_ECON_TAB_OFFSET
#define WORLD_ECON_TAB_OFFSET       0xC6FC
#endif
#ifndef WORLD_SYSTAB_SUBPAGE_OFFSET
#define WORLD_SYSTAB_SUBPAGE_OFFSET 0xC6FC
#endif
#ifndef WORLD_RESOURCE_STRIDE
#define WORLD_RESOURCE_STRIDE       0x380
#endif
#ifndef WORLD_VEHICLE_TYPES_OFFSET
#define WORLD_VEHICLE_TYPES_OFFSET  0x128F8
#endif
#ifndef WORLD_VEHICLE_TYPE_STRIDE
#define WORLD_VEHICLE_TYPE_STRIDE   0xA0C8
#endif
#ifndef VEH_TYPE_SCRIPT_NAME_OFFSET
#define VEH_TYPE_SCRIPT_NAME_OFFSET 0x200u
#endif
#ifndef VEH_TYPE_CATEGORY_OFFSET
#define VEH_TYPE_CATEGORY_OFFSET    0x2B8u
#endif
#ifndef VEH_TYPE_HAS_LICENCE_OFFSET
#define VEH_TYPE_HAS_LICENCE_OFFSET 0x9CD8u
#endif

#ifndef TOOL_DRAG_ACTIVE_OFFSET
#define TOOL_DRAG_ACTIVE_OFFSET     0xD34C
#endif
#ifndef WORLD_WORKSHOP_MGR_OFF
#define WORLD_WORKSHOP_MGR_OFF      0xD448
#endif
#ifndef WORLD_MINIMAP_PTR_OFF
#define WORLD_MINIMAP_PTR_OFF       0xD4A0
#endif
#ifndef WORLD_SELECTED_TOOL_PTR_OFF
#define WORLD_SELECTED_TOOL_PTR_OFF 0xD4C8
#endif
#ifndef WORLD_AUTO_REPLACE_MGR_OFFSET
#define WORLD_AUTO_REPLACE_MGR_OFFSET 0xD4E0u
#endif
#ifndef WORLD_FORCE_TERRAIN_SAVE_OFF
#define WORLD_FORCE_TERRAIN_SAVE_OFF 0xFD30
#endif

// Bank / Loan & Economy Statistics subsystem offsets inside WORLD (SOVIET64.exe PE 6A3EB6AD)
#ifndef WORLD_HUD_POP_BLOCK_OFFSET
#define WORLD_HUD_POP_BLOCK_OFFSET  0xC768
#endif
#ifndef WORLD_HUD_POP_TIMER_OFFSET
#define WORLD_HUD_POP_TIMER_OFFSET  0xC77C
#endif
#ifndef WORLD_HUD_POP_COUNT_OFFSET
#define WORLD_HUD_POP_COUNT_OFFSET  0xC7C0
#endif
#ifndef WORLD_HUD_POP_BLOCK_SIZE
#define WORLD_HUD_POP_BLOCK_SIZE    0xC4
#endif
#ifndef WORLD_ECON_GRAPH_DIRTY_OFF
#define WORLD_ECON_GRAPH_DIRTY_OFF  0x10778
#endif
#ifndef WORLD_POP_GRAPH_DIRTY_OFF
#define WORLD_POP_GRAPH_DIRTY_OFF   0x10960
#endif
#ifndef WORLD_TRACKER_BLOCK_OFFSET
#define WORLD_TRACKER_BLOCK_OFFSET         0x10A40u
#endif
#ifndef WORLD_TRACKER_BLOCK_SIZE
#define WORLD_TRACKER_BLOCK_SIZE           0x230u
#endif
#ifndef WORLD_TRACKER_HEALTH_CUR_SUM_OFF
#define WORLD_TRACKER_HEALTH_CUR_SUM_OFF   0x10A40u
#endif
#ifndef WORLD_TRACKER_HEALTH_CUR_CNT_OFF
#define WORLD_TRACKER_HEALTH_CUR_CNT_OFF   0x10A70u
#endif
#ifndef WORLD_TRACKER_HEALTH_OLD_SUM_OFF
#define WORLD_TRACKER_HEALTH_OLD_SUM_OFF   0x10AA0u
#endif
#ifndef WORLD_TRACKER_HEALTH_OLD_CNT_OFF
#define WORLD_TRACKER_HEALTH_OLD_CNT_OFF   0x10AD0u
#endif
#ifndef WORLD_TRACKER_HAPPY_CUR_SUM_OFF
#define WORLD_TRACKER_HAPPY_CUR_SUM_OFF    0x10B00u
#endif
#ifndef WORLD_TRACKER_HAPPY_CUR_CNT_OFF
#define WORLD_TRACKER_HAPPY_CUR_CNT_OFF    0x10B5Cu
#endif
#ifndef WORLD_TRACKER_HAPPY_OLD_SUM_OFF
#define WORLD_TRACKER_HAPPY_OLD_SUM_OFF    0x10BB8u
#endif
#ifndef WORLD_TRACKER_HAPPY_OLD_CNT_OFF
#define WORLD_TRACKER_HAPPY_OLD_CNT_OFF    0x10C14u
#endif
#ifndef TRACKER_HEALTH_CAUSE_COUNT
#define TRACKER_HEALTH_CAUSE_COUNT         12
#endif
#ifndef TRACKER_HAPPY_CAUSE_COUNT
#define TRACKER_HAPPY_CAUSE_COUNT          23
#endif
#ifndef WORLD_LOANS_OFFSET
#define WORLD_LOANS_OFFSET          0x10C70
#endif
#ifndef WORLD_LOAN_SELECTED_OFFSET
#define WORLD_LOAN_SELECTED_OFFSET  0x10C88
#endif
#ifndef WORLD_LOAN_REPAY_AMOUNT_OFF
#define WORLD_LOAN_REPAY_AMOUNT_OFF 0x10CA8
#endif
#ifndef WORLD_POP_STAT_TIMER_OFF
#define WORLD_POP_STAT_TIMER_OFF    0x10FC4
#endif
#ifndef WORLD_POP_STAT_CURRENT_OFF
#define WORLD_POP_STAT_CURRENT_OFF  0x10FC8
#endif
#ifndef WORLD_STAT_CURRENT_OFFSET
#define WORLD_STAT_CURRENT_OFFSET   0x113C8
#endif
#ifndef WORLD_LOAN_STATS_OFFSET
#define WORLD_LOAN_STATS_OFFSET     0x117B8
#endif
#ifndef WORLD_STAT_HISTORY_OFFSET
#define WORLD_STAT_HISTORY_OFFSET   0x117C8
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
#define WORLD_RES_PTR_SLOTS_BEGIN_OFF 0xC368
#endif
#ifndef WORLD_RES_PTR_SLOTS_END_OFF
#define WORLD_RES_PTR_SLOTS_END_OFF   0xC4D0
#endif
#ifndef WORLD_ELENERGY_RES_PTR_OFF
#define WORLD_ELENERGY_RES_PTR_OFF    0xC458
#endif
#ifndef WORLD_WASTE_RESOURCES_OFFSET
#define WORLD_WASTE_RESOURCES_OFFSET  0x12118u
#endif

#ifndef RVA_BLD_SIM_LOOP_HEAD
#define RVA_BLD_SIM_LOOP_HEAD       0x15563E
#endif
#ifndef RVA_BLD_SIM_LOOP_TAIL
#define RVA_BLD_SIM_LOOP_TAIL       0x156DF4
#endif
#ifndef RVA_BLD_CONSTRUCT_WORKERS
#define RVA_BLD_CONSTRUCT_WORKERS   0x158D60
#endif
#ifndef RVA_CONSTRUCT_GROUNDWORK
#define RVA_CONSTRUCT_GROUNDWORK    0x15C080
#endif
#ifndef RVA_BLD_WORKER_STEP
#define RVA_BLD_WORKER_STEP         0x165090
#endif
#ifndef RVA_BLD_IMPORT_STEP
#define RVA_BLD_IMPORT_STEP         0x18A920
#endif
#ifndef RVA_BLD_PRODUCTIVITY_STEP
#define RVA_BLD_PRODUCTIVITY_STEP   0x18E6B0
#endif
#ifndef RVA_BLD_COURTHOUSE_STEP
#define RVA_BLD_COURTHOUSE_STEP     0x19A930
#endif
#ifndef RVA_BLD_POLICE_STEP
#define RVA_BLD_POLICE_STEP         0x19B280
#endif
#ifndef RVA_BLD_HOTEL_STEP
#define RVA_BLD_HOTEL_STEP          0x19BCC0
#endif
#ifndef RVA_BLD_ATTRACTION1_STEP
#define RVA_BLD_ATTRACTION1_STEP    0x19C230
#endif
#ifndef RVA_BLD_EXPORT_STEP
#define RVA_BLD_EXPORT_STEP         0x19C9E0
#endif
#ifndef RVA_BLD_ATTRACTION2_STEP
#define RVA_BLD_ATTRACTION2_STEP    0x19D5E0
#endif
#ifndef RVA_CALC_CUSTOMS_TOURIST_SPAWN
#define RVA_CALC_CUSTOMS_TOURIST_SPAWN 0x19DDB0
#endif
#ifndef RVA_BLD_CUSTOMS_TOURIST_STEP
#define RVA_BLD_CUSTOMS_TOURIST_STEP 0x19E190
#endif
#ifndef BLD_CUSTOMS_WAITING_TOURISTS_OFF
#define BLD_CUSTOMS_WAITING_TOURISTS_OFF 0xBC0u
#endif
#ifndef BLD_CUSTOMS_TOURIST_TIMER_OFFSET
#define BLD_CUSTOMS_TOURIST_TIMER_OFFSET 0xFC0u
#endif
#ifndef BLD_CUSTOMS_TOURISTS_BOARDED_OFF
#define BLD_CUSTOMS_TOURISTS_BOARDED_OFF 0x1230u
#endif
#ifndef BLD_CUSTOMS_TOURISTS_SPAWNED_OFF
#define BLD_CUSTOMS_TOURISTS_SPAWNED_OFF 0x1234u
#endif
#ifndef RVA_BLD_TOURISM_STEP
#define RVA_BLD_TOURISM_STEP        0x19E3A0
#endif
#ifndef RVA_BLD_STEP_PRODUCTION
#define RVA_BLD_STEP_PRODUCTION     0x1A1CE0
#endif
#ifndef RVA_CIT_HOME_STEP
#define RVA_CIT_HOME_STEP           0x1A8BD0
#endif
#ifndef RVA_BLD_CONSUME_RESOURCE
#define RVA_BLD_CONSUME_RESOURCE    0x1B1D80
#endif
#ifndef RVA_WATER_HEALTH_STEP
#define RVA_WATER_HEALTH_STEP       0x1C9740
#endif
#ifndef RVA_CUSTOMS_BORDER
#define RVA_CUSTOMS_BORDER          0x1D0EA0
#endif
#ifndef RVA_CUSTOMS_VEHICLE
#define RVA_CUSTOMS_VEHICLE         0x1D2B90
#endif
#ifndef RVA_LIVING_TICK
#define RVA_LIVING_TICK             0x1D5360
#endif
#ifndef RVA_BLD_STEP_PRE_POST
#define RVA_BLD_STEP_PRE_POST       0x1D6680
#endif
#ifndef RVA_BLD_STEP_UTILITY
#define RVA_BLD_STEP_UTILITY        0x1EC7B0
#endif
#ifndef RVA_BLD_UTILITY_BORDER_STEP
#define RVA_BLD_UTILITY_BORDER_STEP 0x1ED700
#endif
#ifndef RVA_LEDGER_ADD
#define RVA_LEDGER_ADD              0x31C090
#endif
#ifndef RVA_STAT_RECORD_CLEAR
#define RVA_STAT_RECORD_CLEAR       0x31A820
#endif
#ifndef CHEAT_KEYS_STEP_RVA
#define CHEAT_KEYS_STEP_RVA         0x328510
#endif
#ifndef CHEAT_KEYS_CALL_RVA
#define CHEAT_KEYS_CALL_RVA         0x32C16C
#endif
#ifndef RVA_CALC_STORED_RESOURCES
#define RVA_CALC_STORED_RESOURCES   0x35D290
#endif
#ifndef RVA_VEHICLE_BORDER_EXPORT
#define RVA_VEHICLE_BORDER_EXPORT   0x3FECD9
#endif
#ifndef RVA_VEC_LOAN_PUSH_BACK
#define RVA_VEC_LOAN_PUSH_BACK      0x46D480
#endif
#ifndef RVA_HEATING_STEP
#define RVA_HEATING_STEP            0x4AE620
#endif
#ifndef RVA_TAKE_LOAN
#define RVA_TAKE_LOAN               0x4DF5B0
#endif
#ifndef RVA_GET_MAX_AVAILABLE_BORROW
#define RVA_GET_MAX_AVAILABLE_BORROW 0x4DF7F0
#endif
#ifndef RVA_STEP_ONE_LOAN_DAY
#define RVA_STEP_ONE_LOAN_DAY       0x4DF900
#endif
#ifndef RVA_STEP_ALL_LOANS_DAILY
#define RVA_STEP_ALL_LOANS_DAILY    0x4DFAD0
#endif
#ifndef RVA_CALC_INTEREST_RATE
#define RVA_CALC_INTEREST_RATE      0x4DFEF0
#endif
#ifndef RVA_VEHICLE_CARGO_TRADE
#define RVA_VEHICLE_CARGO_TRADE     0x6BDD00
#endif
#ifndef RVA_VEHICLE_REFUEL
#define RVA_VEHICLE_REFUEL          0x6E7BA0
#endif
#ifndef RVA_FOREIGN_LABOR_STEP
#define RVA_FOREIGN_LABOR_STEP      0x871F40
#endif
#ifndef RVA_FOREIGN_WORKER_BOARD
#define RVA_FOREIGN_WORKER_BOARD    0x872840
#endif
#ifndef RVA_CITIZEN_VEHICLE_ALIGHT
#define RVA_CITIZEN_VEHICLE_ALIGHT  0x872E20
#endif
#ifndef RVA_CITIZEN_FOOT_ARRIVE
#define RVA_CITIZEN_FOOT_ARRIVE     0x86EF40
#endif
#ifndef RVA_CITIZEN_COMMIT_CRIME
#define RVA_CITIZEN_COMMIT_CRIME    0x270A30
#endif
#ifndef RVA_CRIME_EXPIRE_CASE
#define RVA_CRIME_EXPIRE_CASE       0x272340
#endif
#ifndef RVA_CITIZEN_CHURCH_STEP
#define RVA_CITIZEN_CHURCH_STEP     0x875C40
#endif
#ifndef RVA_CITIZEN_STUDIO_EFFECT
#define RVA_CITIZEN_STUDIO_EFFECT   0x4E8660
#endif
#ifndef RVA_CITIZEN_STUDIO_SELECT
#define RVA_CITIZEN_STUDIO_SELECT   0x4E8A80
#endif
#ifndef RVA_CIT_COMPUTE_LIFESPAN
#define RVA_CIT_COMPUTE_LIFESPAN    0x865C00
#endif
#ifndef RVA_CIT_SEVERE_ILLNESS_TICK
#define RVA_CIT_SEVERE_ILLNESS_TICK 0x874C29
#endif
#ifndef RVA_CIT_SEVERE_ILLNESS_RESUME
#define RVA_CIT_SEVERE_ILLNESS_RESUME 0x874C43
#endif
#ifndef RVA_CITIZEN_BIRTH
#define RVA_CITIZEN_BIRTH           0x876140
#endif
#ifndef RVA_CITIZEN_STARVE_STEP
#define RVA_CITIZEN_STARVE_STEP     0x876500
#endif
#ifndef RVA_CITIZEN_REMOVE
#define RVA_CITIZEN_REMOVE          0x877490
#endif
#ifndef RVA_CITIZEN_EVAL_NEEDS
#define RVA_CITIZEN_EVAL_NEEDS      0x87BE00
#endif
#ifndef RVA_CITIZEN_CAN_MOVE_TO_BLD
#define RVA_CITIZEN_CAN_MOVE_TO_BLD 0x87DCC0
#endif
#ifndef RVA_CITIZEN_MOVE_TO_BLD
#define RVA_CITIZEN_MOVE_TO_BLD     0x87E180
#endif

static_assert(WORLD_SYSTAB_SUBPAGE_OFFSET == WORLD_SYSTAB_TAB_PTR_OFFSET + 0x0Cu &&
              RVA_BLD_COURTHOUSE_STEP < RVA_BLD_POLICE_STEP &&
              RVA_BLD_POLICE_STEP < RVA_BLD_HOTEL_STEP &&
              RVA_BLD_HOTEL_STEP < RVA_BLD_ATTRACTION1_STEP &&
              RVA_BLD_ATTRACTION1_STEP < RVA_BLD_ATTRACTION2_STEP &&
              RVA_BLD_ATTRACTION2_STEP < RVA_CALC_CUSTOMS_TOURIST_SPAWN &&
              RVA_CALC_CUSTOMS_TOURIST_SPAWN < RVA_BLD_CUSTOMS_TOURIST_STEP &&
              RVA_BLD_CUSTOMS_TOURIST_STEP < RVA_BLD_TOURISM_STEP &&
              BLD_CUSTOMS_WAITING_TOURISTS_OFF < BLD_CUSTOMS_TOURIST_TIMER_OFFSET &&
              BLD_CUSTOMS_TOURISTS_SPAWNED_OFF == BLD_CUSTOMS_TOURISTS_BOARDED_OFF + 4u &&
              RVA_CITIZEN_COMMIT_CRIME < RVA_CRIME_EXPIRE_CASE &&
              RVA_CITIZEN_FOOT_ARRIVE < RVA_CITIZEN_VEHICLE_ALIGHT &&
              RVA_CITIZEN_EVAL_NEEDS < RVA_CITIZEN_CAN_MOVE_TO_BLD &&
              RVA_CITIZEN_CAN_MOVE_TO_BLD < RVA_CITIZEN_MOVE_TO_BLD,
              "Tourism & Crime building/citizen step RVAs and WORLD_SYSTAB_TAB_PTR_OFFSET (+0xC6F0) must match v1.2.0.9 layout");

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
#define WORLD_BUILDINGS_OFFSET      0x11BB8
#endif
#ifndef WORLD_CONSTRUCTIONS_OFFSET
#define WORLD_CONSTRUCTIONS_OFFSET  0x11CD8
#endif
#ifndef WORLD_CITIZENS_OFFSET
#define WORLD_CITIZENS_OFFSET       0x12760
#endif
#ifndef WORLD_LINES_OFFSET
#define WORLD_LINES_OFFSET          0x128B8
#endif
#ifndef WORLD_VEHICLES_OFFSET
#define WORLD_VEHICLES_OFFSET       0x128C8
#endif

#ifndef SAVE_TRIGGER_FLAG_OFFSET
#define SAVE_TRIGGER_FLAG_OFFSET    0x14DED
#endif
#ifndef SAVE_ZIP_AFTER_FLAG_OFFSET
#define SAVE_ZIP_AFTER_FLAG_OFFSET  0x14DEE
#endif

// Direct RVAs into WORLD containers
#ifndef VEC_BUILDINGS_BEGIN_RVA
#define VEC_BUILDINGS_BEGIN_RVA     0xA30AD8
#endif
#ifndef VEC_BUILDINGS_END_RVA
#define VEC_BUILDINGS_END_RVA       0xA30AE0
#endif
#ifndef VEC_CONSTRUCTIONS_BEGIN_RVA
#define VEC_CONSTRUCTIONS_BEGIN_RVA 0xA30BF8
#endif
#ifndef VEC_CONSTRUCTIONS_END_RVA
#define VEC_CONSTRUCTIONS_END_RVA   0xA30C00
#endif
#ifndef VEC_VEHICLES_BEGIN_RVA
#define VEC_VEHICLES_BEGIN_RVA      (WORLD_RVA + WORLD_VEHICLES_OFFSET)
#endif
#ifndef VEC_VEHICLES_END_RVA
#define VEC_VEHICLES_END_RVA        (WORLD_RVA + WORLD_VEHICLES_OFFSET + 8)
#endif

// Invariant #6: SOVIET_WORKSHOP_MANAGER* is at 0x9E22B8 (WORLD_RVA + 0xD3A8), NEVER 0x9E7750.
#ifndef WORKSHOP_MGR_PTR_RVA
#define WORKSHOP_MGR_PTR_RVA        0xA2C368
#endif
#ifndef RVA_WORKSHOP_REFRESH
#define RVA_WORKSHOP_REFRESH        0x8824A0
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
#define VEH_POS_OFFSET              0xE18
#endif
#ifndef VEH_SIXTH_NODE_ACTIVE_OFFSET
#define VEH_SIXTH_NODE_ACTIVE_OFFSET 0x134C
#endif
#ifndef VEH_SIXTH_NODE_OFFSET
#define VEH_SIXTH_NODE_OFFSET       0x1350
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
#define VEH_TYPE_PTR_OFFSET         0x1748
#endif
#ifndef VEH_CABLEWAY_START_FLAG_OFF
#define VEH_CABLEWAY_START_FLAG_OFF 0x1A24
#endif
#ifndef VEH_HELI_FLIGHT_STATE_OFF
#define VEH_HELI_FLIGHT_STATE_OFF   0x1A9C
#endif
#ifndef VEH_TYPE_PASSENGER_CAP_OFF
#define VEH_TYPE_PASSENGER_CAP_OFF  0x148
#endif
#ifndef WORLD_WESTERN_BORDER_BLD_OFF
#define WORLD_WESTERN_BORDER_BLD_OFF 0x11B98
#endif
#ifndef WORLD_SOVIET_BORDER_BLD_OFF
#define WORLD_SOVIET_BORDER_BLD_OFF  0x11BA0
#endif
#ifndef RVA_LINES_NOTIFY_VEH
#define RVA_LINES_NOTIFY_VEH        0x4BFC10
#endif
#ifndef RVA_VEH_INVERT_STOPS
#define RVA_VEH_INVERT_STOPS        0x68C2C0
#endif
#ifndef RVA_VEH_ADD_STOP
#define RVA_VEH_ADD_STOP            0x6AFAC0
#endif
#ifndef RVA_VEH_DEL_STOP
#define RVA_VEH_DEL_STOP            0x6AFE00
#endif
#ifndef RVA_VEH_REBUILD_CARGO_SUM
#define RVA_VEH_REBUILD_CARGO_SUM   0x6F3220
#endif
#ifndef RVA_VEH_RELEASE
#define RVA_VEH_RELEASE             0x6F9320
#endif
#ifndef RVA_HELI_REROUTE
#define RVA_HELI_REROUTE            0x713650
#endif
#ifndef VEH_FUEL_OFFSET
#define VEH_FUEL_OFFSET             0x5F0u
#endif
#ifndef VEH_SPEED_OFFSET
#define VEH_SPEED_OFFSET            0xD44u
#endif
#ifndef VEH_WORKER_SIM_FLAG_OFFSET
#define VEH_WORKER_SIM_FLAG_OFFSET  0xD7F
#endif
#ifndef VEH_STEER_CURRENT_OFFSET
#define VEH_STEER_CURRENT_OFFSET    0x1194
#endif
#ifndef VEH_STEER_TARGET_OFFSET
#define VEH_STEER_TARGET_OFFSET     0x1198
#endif
#ifndef VEH_CORNER_CUT_APPLIED_OFFSET
#define VEH_CORNER_CUT_APPLIED_OFFSET 0x76Cu
#endif
#ifndef VEH_CORNER_TURN_ANGLE_OFFSET
#define VEH_CORNER_TURN_ANGLE_OFFSET  0x770u
#endif
#ifndef VEH_TRAIL_DIST_ACCUM_OFFSET
#define VEH_TRAIL_DIST_ACCUM_OFFSET   0x119Cu
#endif
#ifndef VEH_TRAIL_BUFFER_OFFSET
#define VEH_TRAIL_BUFFER_OFFSET       0x11A0u
#endif
#ifndef VEH_TRAIL_ENTRY_COUNT
#define VEH_TRAIL_ENTRY_COUNT         25
#endif
#ifndef VEH_TRAIL_ENTRY_STRIDE
#define VEH_TRAIL_ENTRY_STRIDE        0x10u
#endif
#ifndef VEH_AXLE_FRONT_POS_OFFSET
#define VEH_AXLE_FRONT_POS_OFFSET     0x1330u
#endif
#ifndef VEH_AXLE_REAR_POS_OFFSET
#define VEH_AXLE_REAR_POS_OFFSET      0x133Cu
#endif
#ifndef RVA_VEC_WAYPOINT_PUSH_BACK
#define RVA_VEC_WAYPOINT_PUSH_BACK    0x320D0
#endif
#ifndef RVA_VEH_CLEAR_PATH_NODES
#define RVA_VEH_CLEAR_PATH_NODES      0x6B4330
#endif
#ifndef RVA_VEH_ASSIGN_BAY_PATH
#define RVA_VEH_ASSIGN_BAY_PATH       0x6E6AB0
#endif
#ifndef RVA_VEH_ARRIVAL_STEP
#define RVA_VEH_ARRIVAL_STEP          0x7025B0
#endif
#ifndef BLD_ATTACHED_BEGIN_OFFSET
#define BLD_ATTACHED_BEGIN_OFFSET     0x5B8u
#endif
#ifndef BLD_ATTACHED_END_OFFSET
#define BLD_ATTACHED_END_OFFSET       0x5C0u
#endif
#ifndef BLD_BAY_POINTS_BEGIN_OFFSET
#define BLD_BAY_POINTS_BEGIN_OFFSET   0xA40u
#endif
#ifndef BLD_BAY_POINTS_END_OFFSET
#define BLD_BAY_POINTS_END_OFFSET     0xA48u
#endif
#ifndef BLD_BAY_POINT_STRIDE
#define BLD_BAY_POINT_STRIDE          0x20u
#endif
#ifndef BLD_BAY_STATE_BEGIN_OFFSET
#define BLD_BAY_STATE_BEGIN_OFFSET    0xA58u
#endif
#ifndef BLD_BAY_VEH_BEGIN_OFFSET
#define BLD_BAY_VEH_BEGIN_OFFSET      0xA78u
#endif
#ifndef BLD_CORRIDOR_LOCK_VEH_OFFSET
#define BLD_CORRIDOR_LOCK_VEH_OFFSET  0xBB0u
#endif
#ifndef VEH_LINE_SLOT_IDX_OFFSET
#define VEH_LINE_SLOT_IDX_OFFSET      0x510u
#endif
#ifndef VEH_BAY_TARGET_BLD_OFFSET
#define VEH_BAY_TARGET_BLD_OFFSET     0x528u
#endif
#ifndef VEH_BAY_SLOT_IDX_OFFSET
#define VEH_BAY_SLOT_IDX_OFFSET       0x56Cu
#endif
#ifndef VEH_CORRIDOR_LOCK_BLD_OFFSET
#define VEH_CORRIDOR_LOCK_BLD_OFFSET  0xC80u
#endif
#ifndef VEH_WAIT_TIMER_OFFSET
#define VEH_WAIT_TIMER_OFFSET         0xD2Cu
#endif
#ifndef VEH_BAY_WAITING_FLAG_OFFSET
#define VEH_BAY_WAITING_FLAG_OFFSET   0xD58u
#endif
#ifndef VEH_BAY_ARRIVED_FLAG_OFFSET
#define VEH_BAY_ARRIVED_FLAG_OFFSET   0xD59u
#endif
#ifndef VEH_WAYPOINTS_BEGIN_OFFSET
#define VEH_WAYPOINTS_BEGIN_OFFSET    0x738u
#endif
#ifndef VEH_WAYPOINTS_END_OFFSET
#define VEH_WAYPOINTS_END_OFFSET      0x740u
#endif
#ifndef VEH_WAYPOINTS_CAP_OFFSET
#define VEH_WAYPOINTS_CAP_OFFSET      0x748u
#endif
#ifndef VEH_WAYPOINT_INDEX_OFFSET
#define VEH_WAYPOINT_INDEX_OFFSET     0x768u
#endif
#ifndef VEH_RENDER_VISIBLE_OFFSET
#define VEH_RENDER_VISIBLE_OFFSET   0x1418
#endif
#ifndef VEH_WHEELS_BEGIN_OFFSET
#define VEH_WHEELS_BEGIN_OFFSET     0x1478
#endif
#ifndef VEH_WHEELS_END_OFFSET
#define VEH_WHEELS_END_OFFSET       0x1480
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
#ifndef CIT_CUR_BLD_OFFSET
#define CIT_CUR_BLD_OFFSET          0x58
#endif
#ifndef CIT_DEST_BLD_OFFSET
#define CIT_DEST_BLD_OFFSET         0x690
#endif
#ifndef CIT_DORM_FLAG_OFFSET
#define CIT_DORM_FLAG_OFFSET        0x2D
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
#define RVA_SPAWN_CITIZEN                   0x864B80
#endif
#ifndef RVA_IAT_RAND
#define RVA_IAT_RAND                        0x8B30E8
#endif
#ifndef RVA_IAT_RAND_FLOAT
#define RVA_IAT_RAND_FLOAT                  0x8B1AD8
#endif
#ifndef RVA_INVITE_EXPERT_EDU_MIN_FLOAT
#define RVA_INVITE_EXPERT_EDU_MIN_FLOAT     0x951868
#endif
#ifndef RVA_INVITE_EXPERT_EDU_MAX_FLOAT
#define RVA_INVITE_EXPERT_EDU_MAX_FLOAT     0x9518D0
#endif
#ifndef RVA_INVITE_EXPERT_MULT_FLOAT
#define RVA_INVITE_EXPERT_MULT_FLOAT        0x951924
#endif
#ifndef WORLD_PRICE_IMMIGRANT_USD_OFF
#define WORLD_PRICE_IMMIGRANT_USD_OFF       0x0EAC
#endif
#ifndef WORLD_PRICE_IMMIGRANT_RUB_OFF
#define WORLD_PRICE_IMMIGRANT_RUB_OFF       0x0EB0
#endif
#ifndef WORLD_IMMIGRANT_BASE_SPEND_USD_OFF
#define WORLD_IMMIGRANT_BASE_SPEND_USD_OFF  0x11600
#endif
#ifndef WORLD_IMMIGRANT_BASE_SPEND_RUB_OFF
#define WORLD_IMMIGRANT_BASE_SPEND_RUB_OFF  0x11604
#endif
#ifndef WORLD_IMMIGRANT_RATE_MULT_RUB_OFF
#define WORLD_IMMIGRANT_RATE_MULT_RUB_OFF   0x1374C
#endif
#ifndef WORLD_IMMIGRANT_RATE_MULT_USD_OFF
#define WORLD_IMMIGRANT_RATE_MULT_USD_OFF   0x13754
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
#define BLD_TYPE_RES_SPEC_PTR_OFF           0x3A8
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
#ifndef NET_NODES_OFFSET
#define NET_NODES_OFFSET                    0x298u
#endif
#ifndef MGR_SEGMENTS_BEGIN_OFFSET
#define MGR_SEGMENTS_BEGIN_OFFSET           0x2B0u
#endif
#ifndef MGR_SEGMENTS_END_OFFSET
#define MGR_SEGMENTS_END_OFFSET             0x2B8u
#endif
#ifndef SEG_TYPE_OFFSET
#define SEG_TYPE_OFFSET                     0x120u
#endif
#ifndef SEG_DIRECTION_OFFSET
#define SEG_DIRECTION_OFFSET                0x1BCu
#endif

// ---------------------------------------------------------------------------
// Compile-Time Invariant Assertions (AGENTS.md Section 3 & 4)
// ---------------------------------------------------------------------------

static_assert(VEC_BUILDINGS_BEGIN_RVA == WORLD_RVA + WORLD_BUILDINGS_OFFSET,
              "VEC_BUILDINGS_BEGIN_RVA must equal WORLD_RVA + WORLD_BUILDINGS_OFFSET (0xA30AD8)");
static_assert(VEC_CONSTRUCTIONS_BEGIN_RVA == WORLD_RVA + WORLD_CONSTRUCTIONS_OFFSET,
              "VEC_CONSTRUCTIONS_BEGIN_RVA must equal WORLD_RVA + WORLD_CONSTRUCTIONS_OFFSET (0xA30BF8)");
static_assert(WORKSHOP_MGR_PTR_RVA == WORLD_RVA + WORLD_WORKSHOP_MGR_OFF,
              "WORKSHOP_MGR_PTR_RVA must equal WORLD_RVA + 0xD448 (0xA2C368), never 0x9E7750");
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
              WORLD_FAST_RESEARCH_CHEAT_OFFSET == 0x14501u &&
              WORLD_FAST_CONSTRUCT_CHEAT_OFFSET == WORLD_FAST_RESEARCH_CHEAT_OFFSET + 1u &&
              WORLD_SYSTAB_SUBPAGE_OFFSET == WORLD_ECON_TAB_OFFSET,
              "Cheat/dev mode byte offsets (0x9, 0xA, 0x14449, 0x1444A) and systab subpage offset (0xC65C) must match 0x309D80 / 0x329390 / 0x17E602");
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
              CAM_MGR_TRANS_STEP_OFFSET == 0x16C && CAM_MGR_TRANS_TIMER_OFFSET == 0x17C &&
              CAM_MGR_TRANS_DUR_OFFSET == 0x180,
              "Camera manager position and transition offsets must match 0x14250 layout");
static_assert(RVA_SPAWN_CITIZEN == 0x864B80 &&
              BLD_CITIZENS_END_OFF == BLD_CITIZENS_BEGIN_OFF + 8 &&
              WORLD_STAT_CURRENT_OFFSET + STAT_IMMIGRANT_SPEND_OFF == 0x11608 &&
              WORLD_STAT_CURRENT_OFFSET + STAT_CITIZEN_COUNTS_OFF + 12 == 0x1161C,
              "Invite citizens (0x734FDE -> 0x864B80) offsets must match v1.2.0.9 layout");
static_assert(WORLD_SOVIET_BORDER_BLD_OFF == WORLD_WESTERN_BORDER_BLD_OFF + 8 &&
              VEH_STOP_RECORD_STRIDE == 0x198 &&
              VEH_STOP_HALF_STRIDE * 2 + 8 == VEH_STOP_RECORD_STRIDE,
              "Vehicle window border stop & stop record offsets must match 0x7D6B00 layout");

// ---------------------------------------------------------------------------
// Remote Command Sound, Warning Banner & Notification Popup Isolation Constants
// ---------------------------------------------------------------------------

#ifndef IMPORT_HAS_SOUND_RVA
#define IMPORT_HAS_SOUND_RVA            0x8B1D58
#endif
#ifndef IMPORT_GET_SOUND_RVA
#define IMPORT_GET_SOUND_RVA            0x8B1D60
#endif
#ifndef IMPORT_PLAY_SOUND_RVA
#define IMPORT_PLAY_SOUND_RVA           0x8B22E8
#endif
#ifndef IMPORT_PLAY_SOUND_3D_RVA
#define IMPORT_PLAY_SOUND_3D_RVA        0x8B1C20u
#endif
#ifndef SND_CLICK_NAME_RVA
#define SND_CLICK_NAME_RVA              0x8BB3C8
#endif
#ifndef SOUND_MUTE_BYTE_RVA
#define SOUND_MUTE_BYTE_RVA             0xA1EF5C
#endif
#ifndef SOUND_ENGINE_PTR_RVA
#define SOUND_ENGINE_PTR_RVA            0xA2ECC8
#endif
#ifndef SOUND_MGR_RVA
#define SOUND_MGR_RVA                   0xA2ECD0
#endif
#ifndef RVA_HAS_PLANE_PROPS
#define RVA_HAS_PLANE_PROPS             0x0319A0u
#endif
#ifndef RVA_FIND_TAG_ID
#define RVA_FIND_TAG_ID                 0x277950u
#endif
#ifndef VEH_TYPE_ELECTRIC_OFFSET
#define VEH_TYPE_ELECTRIC_OFFSET        0x86D0u
#endif
#ifndef VEH_TYPE_PROP_VEC_OFFSET
#define VEH_TYPE_PROP_VEC_OFFSET        0x7B08u
#endif
#ifndef VEH_TYPE_TAGS_OFFSET
#define VEH_TYPE_TAGS_OFFSET            0x9708u
#endif
#ifndef WORLD_TAGS_BEGIN_OFFSET
#define WORLD_TAGS_BEGIN_OFFSET         0x128A0u
#endif
#ifndef WORLD_TAG_ENTRY_STRIDE
#define WORLD_TAG_ENTRY_STRIDE          0x44u
#endif
#ifndef CAMERA_EYE_RVA
#define CAMERA_EYE_RVA                  0xA33DD0u
#endif
#ifndef WORLD_OPEN_WINDOWS_BEGIN_OFFSET
#define WORLD_OPEN_WINDOWS_BEGIN_OFFSET 0xCB60u
#endif
#ifndef WORLD_OPEN_WINDOWS_END_OFFSET
#define WORLD_OPEN_WINDOWS_END_OFFSET   0xCB68u
#endif
#ifndef WORLD_OPEN_WINDOWS_BEGIN_RVA
#define WORLD_OPEN_WINDOWS_BEGIN_RVA    (WORLD_RVA + WORLD_OPEN_WINDOWS_BEGIN_OFFSET)
#endif
#ifndef WORLD_OPEN_WINDOWS_END_RVA
#define WORLD_OPEN_WINDOWS_END_RVA      (WORLD_RVA + WORLD_OPEN_WINDOWS_END_OFFSET)
#endif
#ifndef SITE_DENOMINATION_RVA
#define SITE_DENOMINATION_RVA           0x83D090u
#endif
#ifndef FN_CURRENCY_DENOMINATION_RVA
#define FN_CURRENCY_DENOMINATION_RVA    0x318460u
#endif
#ifndef WINDOW_POPUP_TYPE_OFFSET
#define WINDOW_POPUP_TYPE_OFFSET        0x34u
#endif
#ifndef WINDOW_POPUP_TYPE_DENOMINATION
#define WINDOW_POPUP_TYPE_DENOMINATION  0x13
#endif
#ifndef CURRENCY_DENOMINATION_DIVISOR
#define CURRENCY_DENOMINATION_DIVISOR   100.0
#endif
#ifndef WORLD_NOTIF_POPUPS_BEGIN_OFFSET
#define WORLD_NOTIF_POPUPS_BEGIN_OFFSET 0xD2A8u
#endif
#ifndef WORLD_NOTIF_POPUPS_END_OFFSET
#define WORLD_NOTIF_POPUPS_END_OFFSET   0xD2B0u
#endif
#ifndef WORLD_BRUSH_SOUNDS_BEGIN_OFFSET
#define WORLD_BRUSH_SOUNDS_BEGIN_OFFSET 0xD528u
#endif
#ifndef WORLD_BRUSH_SOUNDS_END_OFFSET
#define WORLD_BRUSH_SOUNDS_END_OFFSET   0xD530u
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
#define WORLD_WARNING_TEXT_OFFSET       0xD640u
#endif
#ifndef WORLD_WARNING_TEXT_BYTES
#define WORLD_WARNING_TEXT_BYTES        0x2000u
#endif
#ifndef WORLD_WARNING_TIMER_OFFSET
#define WORLD_WARNING_TIMER_OFFSET      0xF640u
#endif
#ifndef WORLD_PLACEMENT_ERROR_OFFSET
#define WORLD_PLACEMENT_ERROR_OFFSET    0x11AB0u
#endif
#ifndef NOTIF_MSG_POS_X_OFFSET
#define NOTIF_MSG_POS_X_OFFSET          0x4200u
#endif
#ifndef NOTIF_MSG_POS_Z_OFFSET
#define NOTIF_MSG_POS_Z_OFFSET          0x4208u
#endif
#ifndef NOTIFICATION_REBUILD_POPUPS_RVA
#define NOTIFICATION_REBUILD_POPUPS_RVA 0x4EAEB0u
#endif
#ifndef WARNING_POPUP_CREATE_RVA
#define WARNING_POPUP_CREATE_RVA        0x7242F0u
#endif
#ifndef REMOTE_3D_SOUND_MAX_DIST
#define REMOTE_3D_SOUND_MAX_DIST        250.0f
#endif

static_assert(CAMERA_EYE_RVA == CAMERA_RVA + CAMERA_POS_OFFSET,
              "CAMERA_EYE_RVA (0xA33DD0) must equal CAMERA_RVA (0xA33D50) + CAMERA_POS_OFFSET (0x80)");
static_assert(WORLD_OPEN_WINDOWS_END_OFFSET == WORLD_OPEN_WINDOWS_BEGIN_OFFSET + 8 &&
              WORLD_OPEN_WINDOWS_BEGIN_RVA == WORLD_RVA + WORLD_OPEN_WINDOWS_BEGIN_OFFSET &&
              WORLD_OPEN_WINDOWS_END_RVA == WORLD_OPEN_WINDOWS_BEGIN_RVA + 8 &&
              SITE_DENOMINATION_RVA == 0x83D090u &&
              FN_CURRENCY_DENOMINATION_RVA == 0x318460u &&
              WINDOW_POPUP_TYPE_OFFSET == 0x34u &&
              WINDOW_POPUP_TYPE_DENOMINATION == 0x13,
              "WORLD_OPEN_WINDOWS_BEGIN/END_RVA (0xA2BA80/0xA2BA88) and currency denomination RVAs/offsets must match v1.2.0.9 layout");
static_assert(WORLD_NOTIF_POPUPS_END_OFFSET == WORLD_NOTIF_POPUPS_BEGIN_OFFSET + 8 &&
              WORLD_BRUSH_SOUNDS_END_OFFSET == WORLD_BRUSH_SOUNDS_BEGIN_OFFSET + 8 &&
              BRUSH_SOUND_ACTIVE_OFFSET == BRUSH_SOUND_VOL_OFFSET + 4 &&
              BRUSH_SOUND_REC_SIZE == 0x110u,
              "Notification popups (+0xD2A8..+0xD2B0) and brush loop sound records (+0xD528..+0xD530, 0x110 stride) must match v1.2.0.9 layout");
static_assert(WORLD_WARNING_TIMER_OFFSET == WORLD_WARNING_TEXT_OFFSET + WORLD_WARNING_TEXT_BYTES &&
              NOTIF_MSG_POS_Z_OFFSET == NOTIF_MSG_POS_X_OFFSET + 8,
              "Warning banner timer (+0xF640) must follow 0x2000-byte warning text (+0xD640), and notification pos Z (+0x4208) must be 8 bytes after pos X (+0x4200)");

// ---------------------------------------------------------------------------
// Building Resource Purchase (Manual One-Time Buy & Auto-Purchase) Constants
// ---------------------------------------------------------------------------

#ifndef WORLD_UI_RES_SLIDERS_OFFSET
#define WORLD_UI_RES_SLIDERS_OFFSET     0x11FE0u
#endif
#ifndef BLD_STORAGE_VEC_CLEAR_RVA
#define BLD_STORAGE_VEC_CLEAR_RVA       0xC8CF0u
#endif
#ifndef BLD_INIT_RES_SLIDERS_RVA
#define BLD_INIT_RES_SLIDERS_RVA        0x18D7E0u
#endif
#ifndef BLD_APPLY_AUTO_BUY_RVA
#define BLD_APPLY_AUTO_BUY_RVA          0x18D9F0u
#endif
#ifndef BLD_MANUAL_BUY_RES_RVA
#define BLD_MANUAL_BUY_RES_RVA          0x18DAF0u
#endif
#ifndef SITE_BLD_MANUAL_BUY_RES_RVA
#define SITE_BLD_MANUAL_BUY_RES_RVA     0x747E9Bu
#endif
#ifndef SITE_BLD_INIT_SLIDERS_DISABLE_RVA
#define SITE_BLD_INIT_SLIDERS_DISABLE_RVA 0x747D7Bu
#endif
#ifndef SITE_BLD_AUTO_BUY_DISABLE_RVA
#define SITE_BLD_AUTO_BUY_DISABLE_RVA   0x747D8Du
#endif
#ifndef SITE_AUTO_BUY_DISABLE_AFTER_RVA
#define SITE_AUTO_BUY_DISABLE_AFTER_RVA 0x747D92u
#endif
#ifndef SITE_AUTO_BUY_DISABLE_SKIP_RVA
#define SITE_AUTO_BUY_DISABLE_SKIP_RVA  0x747D99u
#endif
#ifndef SITE_BLD_INIT_SLIDERS_CANCEL_RVA
#define SITE_BLD_INIT_SLIDERS_CANCEL_RVA 0x747E10u
#endif
#ifndef SITE_MANUAL_BUY_AFTER_RVA
#define SITE_MANUAL_BUY_AFTER_RVA       0x747EA0u
#endif
#ifndef SITE_MANUAL_BUY_SKIP_RVA
#define SITE_MANUAL_BUY_SKIP_RVA        0x747EA7u
#endif
#ifndef SITE_BLD_AUTO_BUY_APPLY_RVA
#define SITE_BLD_AUTO_BUY_APPLY_RVA     0x747F22u
#endif
#ifndef SITE_AUTO_BUY_APPLY_AFTER_RVA
#define SITE_AUTO_BUY_APPLY_AFTER_RVA   0x747F27u
#endif
#ifndef SITE_AUTO_BUY_APPLY_SKIP_RVA
#define SITE_AUTO_BUY_APPLY_SKIP_RVA    0x747F2Eu
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
              BLD_MANUAL_BUY_RES_RVA == BLD_APPLY_AUTO_BUY_RVA + 0x100u &&
              SITE_AUTO_BUY_DISABLE_AFTER_RVA == SITE_BLD_AUTO_BUY_DISABLE_RVA + 5u &&
              SITE_AUTO_BUY_DISABLE_SKIP_RVA == SITE_AUTO_BUY_DISABLE_AFTER_RVA + 7u &&
              SITE_MANUAL_BUY_AFTER_RVA == SITE_BLD_MANUAL_BUY_RES_RVA + 5u &&
              SITE_MANUAL_BUY_SKIP_RVA == SITE_MANUAL_BUY_AFTER_RVA + 7u &&
              SITE_AUTO_BUY_APPLY_AFTER_RVA == SITE_BLD_AUTO_BUY_APPLY_RVA + 5u &&
              SITE_AUTO_BUY_APPLY_SKIP_RVA == SITE_AUTO_BUY_APPLY_AFTER_RVA + 7u,
              "Building resource purchase offsets and auto-buy skip RVAs must match v1.2.0.9 layout");

// ---------------------------------------------------------------------------
// C3D Material IAT RVAs (SOVIET64.exe Import Address Table for C3DDLL64.dll)
// ---------------------------------------------------------------------------

#ifndef RVA_IAT_SAVE_ORIGINAL_VALUES
#define RVA_IAT_SAVE_ORIGINAL_VALUES        0x8B25A8
#endif
#ifndef RVA_IAT_CHANGE_VALUES_TO
#define RVA_IAT_CHANGE_VALUES_TO            0x8B2598
#endif
#ifndef RVA_IAT_RESTORE_ORIGINAL_VALUES
#define RVA_IAT_RESTORE_ORIGINAL_VALUES     0x8B25A0
#endif

static_assert(RVA_IAT_SAVE_ORIGINAL_VALUES == 0x8B25A8, "RVA_IAT_SAVE_ORIGINAL_VALUES mismatch");
static_assert(RVA_IAT_CHANGE_VALUES_TO == 0x8B2598, "RVA_IAT_CHANGE_VALUES_TO mismatch");
static_assert(RVA_IAT_RESTORE_ORIGINAL_VALUES == 0x8B25A0, "RVA_IAT_RESTORE_ORIGINAL_VALUES mismatch");

// ---------------------------------------------------------------------------
// University & Research Tree Constants (SOVIET64.exe PE 6A3EB6AD)
// ---------------------------------------------------------------------------

#ifndef WORLD_RESEARCH_UNLOCKS_OFFSET
#define WORLD_RESEARCH_UNLOCKS_OFFSET       0x117FCu
#endif
#ifndef WORLD_RESEARCH_UNLOCKS_BYTES
#define WORLD_RESEARCH_UNLOCKS_BYTES        0x16u
#endif
#ifndef WORLD_RESEARCH_VEC_BEGIN_OFFSET
#define WORLD_RESEARCH_VEC_BEGIN_OFFSET     0x11818u
#endif
#ifndef WORLD_RESEARCH_VEC_END_OFFSET
#define WORLD_RESEARCH_VEC_END_OFFSET       0x11820u
#endif
#ifndef RESEARCH_ITEM_STRIDE
#define RESEARCH_ITEM_STRIDE                0x138u
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
#define RESEARCH_ITEM_REQ_WORK_OFFSET       0x5Cu
#endif
#ifndef RESEARCH_ITEM_AVAILABLE_OFFSET
#define RESEARCH_ITEM_AVAILABLE_OFFSET      0x11Cu
#endif
#ifndef RESEARCH_ITEM_PROGRESS_OFFSET
#define RESEARCH_ITEM_PROGRESS_OFFSET       0x120u
#endif
#ifndef RESEARCH_ITEM_BUILDING_PTR_OFFSET
#define RESEARCH_ITEM_BUILDING_PTR_OFFSET   0x128u
#endif
#ifndef RESEARCH_ITEM_BLD_PTR_OFFSET
#define RESEARCH_ITEM_BLD_PTR_OFFSET        RESEARCH_ITEM_BUILDING_PTR_OFFSET
#endif
#ifndef RESEARCH_ITEM_TIMER_E0_OFFSET
#define RESEARCH_ITEM_TIMER_E0_OFFSET       0x130u
#endif
#ifndef BLD_UNIV_STAFF_RATIO_OFFSET
#define BLD_UNIV_STAFF_RATIO_OFFSET         0x11A8u
#endif
#ifndef RVA_BLD_UNIV_STEP
#define RVA_BLD_UNIV_STEP                   0x1969A0u
#endif
#ifndef RVA_RESEARCH_DAILY_UPDATE
#define RVA_RESEARCH_DAILY_UPDATE           0x315060u
#endif
#ifndef RVA_RESEARCH_RECOMPUTE
#define RVA_RESEARCH_RECOMPUTE              0x315120u
#endif
#ifndef RVA_RESEARCH_FIND_BY_BLD
#define RVA_RESEARCH_FIND_BY_BLD            0x3160A0u
#endif
#ifndef RVA_UNIV_RESEARCH_PANEL
#define RVA_UNIV_RESEARCH_PANEL             0x316460u
#endif

static_assert(WORLD_RESEARCH_VEC_END_OFFSET == WORLD_RESEARCH_VEC_BEGIN_OFFSET + 8 &&
              RESEARCH_ITEM_STRIDE == 0x138u &&
              RESEARCH_ITEM_PROGRESS_OFFSET == RESEARCH_ITEM_AVAILABLE_OFFSET + 4 &&
              RESEARCH_ITEM_BUILDING_PTR_OFFSET == RESEARCH_ITEM_PROGRESS_OFFSET + 8 &&
              RESEARCH_ITEM_TIMER_E0_OFFSET == RESEARCH_ITEM_BUILDING_PTR_OFFSET + 8,
              "ResearchItem layout (+0x11778, stride 0xE8, +0xCC/+0xD0/+0xD8/+0xE0) must match 0x2F6C90 / 0x2F7ED0");

// ---------------------------------------------------------------------------
// Construction & Demolition Office Assignment RVAs (SOVIET64.exe PE 6A3EB6AD)
// ---------------------------------------------------------------------------

#ifndef WORLD_ACTIVE_WINDOW_BLD_OFF
#define WORLD_ACTIVE_WINDOW_BLD_OFF         0xD338u
#endif
#ifndef RVA_CALLER_OFFICE_ROAD_SCAN
#define RVA_CALLER_OFFICE_ROAD_SCAN         0x159B0Bu
#endif
#ifndef RVA_ASSIGN_CONSTRUCTION_OFFICE
#define RVA_ASSIGN_CONSTRUCTION_OFFICE      0x17A5E0u
#endif
#ifndef RVA_UNASSIGN_CONSTRUCTION_OFFICE
#define RVA_UNASSIGN_CONSTRUCTION_OFFICE    0x17ACB0u
#endif
#ifndef RVA_START_PLANNED_CONSTRUCTION
#define RVA_START_PLANNED_CONSTRUCTION      0x1C1A50u
#endif
#ifndef SITE_COPY_OFFICE_ASSIGN_NET_RVA
#define SITE_COPY_OFFICE_ASSIGN_NET_RVA     0x1F1A1Eu
#endif
#ifndef SITE_COPY_OFFICE_ASSIGN_BLD_RVA
#define SITE_COPY_OFFICE_ASSIGN_BLD_RVA     0x1F2CF2u
#endif
#ifndef SITE_CONSTRUCT_TOOL_START_PLAN_RVA
#define SITE_CONSTRUCT_TOOL_START_PLAN_RVA  0x1F492Du
#endif
#ifndef SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA
#define SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA  0x1F495Au
#endif
#ifndef SITE_CONSTRUCT_TOOL_ASSIGN_BLD_RVA
#define SITE_CONSTRUCT_TOOL_ASSIGN_BLD_RVA  0x1F4DBDu
#endif

static_assert(RVA_UNASSIGN_CONSTRUCTION_OFFICE == RVA_ASSIGN_CONSTRUCTION_OFFICE + 0x6D0u &&
              RVA_CALLER_OFFICE_ROAD_SCAN < RVA_ASSIGN_CONSTRUCTION_OFFICE &&
              SITE_CONSTRUCT_TOOL_ASSIGN_NET_RVA == SITE_CONSTRUCT_TOOL_START_PLAN_RVA + 0x2Du,
              "Construction Office assignment RVAs must match v1.2.0.9 layout");

static_assert(VEH_TYPE_SCRIPT_NAME_OFFSET == 0x200u &&
              VEH_TYPE_CATEGORY_OFFSET == 0x2B8u &&
              VEH_TYPE_HAS_LICENCE_OFFSET == 0x9CD8u &&
              VEH_TYPE_HAS_LICENCE_OFFSET < WORLD_VEHICLE_TYPE_STRIDE,
              "VEHICLE_TYPE license & script offsets (+0x200, +0x2B8, +0x9CD8, stride 0xA0C8) must match v1.2.0.9");


// ---------------------------------------------------------------------------
// City Creation/Deletion, On-the-Fly Rename & Car Dealer Filter Offsets
// ---------------------------------------------------------------------------

#ifndef CITY_CREATE_RVA
#define CITY_CREATE_RVA                     0x34E060u
#endif
#ifndef CITY_DELETE_RVA
#define CITY_DELETE_RVA                     0x34E1D0u
#endif
#ifndef CITY_REASSIGN_BLDS_RVA
#define CITY_REASSIGN_BLDS_RVA              0x34EB60u
#endif
#ifndef REBUILD_STOP_LINE_NAMES_RVA
#define REBUILD_STOP_LINE_NAMES_RVA         0x4BC850u
#endif
#ifndef WORLD_RENAME_REFRESH_SRC_OFF
#define WORLD_RENAME_REFRESH_SRC_OFF        0xC750u
#endif
#ifndef WORLD_RENAME_REFRESH_DST_OFF
#define WORLD_RENAME_REFRESH_DST_OFF        0xC758u
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
#define SITE_RENAME_AFTER_CALL_RVA          0x842335u
#endif
#ifndef SITE_RENAME_SKIP_MUTATION_RVA
#define SITE_RENAME_SKIP_MUTATION_RVA       0x84242Eu
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
              CITY_REASSIGN_BLDS_RVA == CITY_CREATE_RVA + 0xB00u &&
              BLD_CITIZEN_FILTER_END == BLD_CITIZEN_FILTER_AT + 0x10u &&
              VEH_NAME_END_OFFSET == 0x18Cu,
              "City creation/rename RVAs and building/vehicle name/filter offsets must match v1.2.0.9 layout");
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
#define RVA_HUD_DRAW_BTN_FRAME              0x46AA70u
#endif
#ifndef RVA_MINIMAP_DRAW_CONTENT
#define RVA_MINIMAP_DRAW_CONTENT            0x4E42F0u
#endif
#ifndef RVA_MINIMAP_DRAW_BUTTONS
#define RVA_MINIMAP_DRAW_BUTTONS            0x4E63B0u
#endif
#ifndef WORLD_MINIMAP_PTR_OFFSET
#define WORLD_MINIMAP_PTR_OFFSET            0xD4A0u
#endif
#ifndef MINIMAP_TOOLTIP_WSTR_RVA
#define MINIMAP_TOOLTIP_WSTR_RVA            0xA2C560u
#endif
#ifndef HUD_QUAD_STAGE_RVA
#define HUD_QUAD_STAGE_RVA                  0xA08070u
#endif
#ifndef HUD_QUAD_SIZE_X_RVA
#define HUD_QUAD_SIZE_X_RVA                 0xA082F8u
#endif
#ifndef HUD_QUAD_SIZE_Y_RVA
#define HUD_QUAD_SIZE_Y_RVA                 0xA082FCu
#endif
#ifndef HUD_QUAD_POS_X_RVA
#define HUD_QUAD_POS_X_RVA                  0xA08300u
#endif
#ifndef HUD_QUAD_POS_Y_RVA
#define HUD_QUAD_POS_Y_RVA                  0xA08304u
#endif
#ifndef HUD_QUAD_Z_RVA
#define HUD_QUAD_Z_RVA                      0xA08308u
#endif
#ifndef HUD_QUAD_ROT_RVA
#define HUD_QUAD_ROT_RVA                    0xA08318u
#endif
#ifndef HUD_QUAD_COLOR_RVA
#define HUD_QUAD_COLOR_RVA                  0xA0831Cu
#endif
#ifndef HUD_UI_SCALE_RVA
#define HUD_UI_SCALE_RVA                    0x9DC08Cu
#endif
#ifndef HUD_SCREEN_HEIGHT_RVA
#define HUD_SCREEN_HEIGHT_RVA               0x9DC06Cu
#endif
#ifndef HUD_RENDERER_PTR_RVA
#define HUD_RENDERER_PTR_RVA                0xA34DC8u
#endif
#ifndef HUD_TEX_BIND_VTABLE_OFF
#define HUD_TEX_BIND_VTABLE_OFF             0x70u
#endif
#ifndef HUD_SOLID_TEX_PTR_RVA
#define HUD_SOLID_TEX_PTR_RVA               0xA2ABE8u
#endif
#ifndef HUD_BTN_FRAME_TEX_PTR_RVA
#define HUD_BTN_FRAME_TEX_PTR_RVA           0xA29F78u
#endif
#ifndef HUD_PIN_ICON_TEX_PTR_RVA
#define HUD_PIN_ICON_TEX_PTR_RVA            0xA2A460u
#endif
#ifndef HUD_MAP_CAMERA_TEX_PTR_RVA
#define HUD_MAP_CAMERA_TEX_PTR_RVA          0xA2A868u
#endif
#ifndef MINIMAP_CAMERA_ICON_MAP_FRACTION
#define MINIMAP_CAMERA_ICON_MAP_FRACTION    0.15f   // exe+0x909CA0
#endif
#ifndef MINIMAP_CAMERA_ICON_MAX_PX
#define MINIMAP_CAMERA_ICON_MAX_PX          70.0f   // exe+0x90AAB8 (x uiScale)
#endif
#ifndef HUD_FONT_MAIN_PTR_RVA
#define HUD_FONT_MAIN_PTR_RVA               0x9DF330u
#endif
#ifndef HUD_TEXT_MGR_RVA
#define HUD_TEXT_MGR_RVA                    0x9E0FC0u
#endif
#ifndef HUD_STYLE_BLOCK_RVA
#define HUD_STYLE_BLOCK_RVA                 0xA29D00u
#endif
#ifndef HUD_MIDDLEPOINT_OBJ_RVA
#define HUD_MIDDLEPOINT_OBJ_RVA             0xA34D90u
#endif
#ifndef HUD_FMT_WSTR_RVA
#define HUD_FMT_WSTR_RVA                    0x8FD6E0u
#endif
#ifndef HUD_BASIC_PASS_STR_RVA
#define HUD_BASIC_PASS_STR_RVA              0x8B708Cu
#endif
#ifndef HUD_MOUSE_STATE_RVA
#define HUD_MOUSE_STATE_RVA                 0xA9EC50u
#endif
#ifndef HUD_MOUSE_LCLICK_EDGE_RVA
#define HUD_MOUSE_LCLICK_EDGE_RVA           0xA9EF51u
#endif
#ifndef MOUSE_WHEEL_DELTA_RVA
#define MOUSE_WHEEL_DELTA_RVA               0xA9EF80u
#endif
#ifndef RVA_IAT_CREATE_MANAGED_TEX
#define RVA_IAT_CREATE_MANAGED_TEX          0x8B1518u
#endif
#ifndef RVA_IAT_HUD_HIT_TEST_QUAD
#define RVA_IAT_HUD_HIT_TEST_QUAD           0x8B18F0u
#endif
#ifndef RVA_IAT_HUD_DRAW_QUAD
#define RVA_IAT_HUD_DRAW_QUAD               0x8B1900u
#endif
#ifndef RVA_IAT_HUD_GET_MOUSE_POS
#define RVA_IAT_HUD_GET_MOUSE_POS           0x8B1908u
#endif
#ifndef RVA_IAT_TEXT_PRINT_ALL
#define RVA_IAT_TEXT_PRINT_ALL              0x8B1910u
#endif
#ifndef RVA_IAT_TEXT_DRAW_W
#define RVA_IAT_TEXT_DRAW_W                 0x8B1920u
#endif
#ifndef RVA_IAT_HUD_END_DRAW
#define RVA_IAT_HUD_END_DRAW                0x8B1998u
#endif
#ifndef RVA_IAT_HUD_BEGIN_DRAW
#define RVA_IAT_HUD_BEGIN_DRAW              0x8B19B8u
#endif

static_assert(RVA_MINIMAP_DRAW_BUTTONS == RVA_MINIMAP_DRAW_CONTENT + 0x20C0u &&
              HUD_SOLID_TEX_PTR_RVA == 0xA2ABE8u &&
              HUD_MAP_CAMERA_TEX_PTR_RVA == WORLD_RVA + 0xB948u &&
              HUD_QUAD_POS_X_RVA == HUD_QUAD_SIZE_X_RVA + 8u &&
              HUD_QUAD_COLOR_RVA == HUD_QUAD_SIZE_X_RVA + 0x24u &&
              RVA_IAT_HUD_DRAW_QUAD == RVA_IAT_HUD_HIT_TEST_QUAD + 0x10u,
              "Minimap DrawContent/DrawButtons RVAs, camera texture and C3D_HUD quad offsets must match v1.2.0.9 layout");

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
              "Building search range and auto-search flag offsets must match v1.2.0.9 BUILDING layout");

// Building TV/Radio studio, Upbringing/Rehabilitation, staff loyalty, and notification alert offsets
#ifndef WORLD_STUDIOS_BEGIN_OFFSET
#define WORLD_STUDIOS_BEGIN_OFFSET          0xD2D0u // std::vector<Studio*> begin pointer
#endif
#ifndef WORLD_STUDIOS_END_OFFSET
#define WORLD_STUDIOS_END_OFFSET            0xD2D8u // std::vector<Studio*> end pointer
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

static_assert(WORLD_STUDIOS_BEGIN_OFFSET == 0xD2D0u &&
              WORLD_STUDIOS_END_OFFSET == 0xD2D8u &&
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
              "Studio broadcast, upbringing sliders, staff loyalty, and alert checkbox offsets must match v1.2.0.9 layout");

static_assert(CAM_MGR_PID_PTRS_OFFSET + CAM_MGR_PID_COUNT * sizeof(void*) == 0x150u &&
              CAM_MGR_PID_STRUCT_BYTES == 0x78u &&
              VEH_CORNER_TURN_ANGLE_OFFSET == VEH_CORNER_CUT_APPLIED_OFFSET + 4u &&
              VEH_TRAIL_BUFFER_OFFSET == VEH_TRAIL_DIST_ACCUM_OFFSET + 4u &&
              VEH_TRAIL_BUFFER_OFFSET + VEH_TRAIL_ENTRY_COUNT * VEH_TRAIL_ENTRY_STRIDE == VEH_AXLE_FRONT_POS_OFFSET &&
              VEH_AXLE_REAR_POS_OFFSET == VEH_AXLE_FRONT_POS_OFFSET + 3 * sizeof(float) &&
              BLD_BAY_POINT_STRIDE == 0x20u &&
              RVA_VEH_ARRIVAL_STEP == 0x7025B0 &&
              RVA_VEH_ASSIGN_BAY_PATH == 0x6E6AB0,
              "Camera manager PID pointers (+0x118..+0x148, 0x78B each) and vehicle trail buffer/axle offsets must match v1.2.0.9 layout");

// Building animated sub-mesh (oil pumpjack / excavator / mine hoist / wind turbine / factory rotor) offsets
#ifndef BLD_CONSTRUCTION_SITE_OFFSET
#define BLD_CONSTRUCTION_SITE_OFFSET        0x628u   // BUILDING::constructionSite pointer (non-null while under construction)
#endif
#ifndef BLD_ANIM_SUBMESH_OFFSET
#define BLD_ANIM_SUBMESH_OFFSET             0x5E0u   // C3D_MESH* animated sub-mesh pointer on BUILDING (matches TYPE+0x420)
#endif
#ifndef BLD_ANIM_FRAME_OFFSET
#define BLD_ANIM_FRAME_OFFSET               0x5F0u   // float current animation frame [0.0f .. maxFrames)
#endif
#ifndef BLD_UTILIZATION_RATIO_OFFSET
#define BLD_UTILIZATION_RATIO_OFFSET        0xDE0u   // float current production / load utilization ratio
#endif
#ifndef BLD_TYPE_ANIM_SUBMESH_OFFSET
#define BLD_TYPE_ANIM_SUBMESH_OFFSET        0x428u   // C3D_MESH* animated sub-mesh pointer in BUILDING::TYPE
#endif
#ifndef BLD_TYPE_ANIM_SPEED_OFFSET
#define BLD_TYPE_ANIM_SPEED_OFFSET          0x630u   // float animation speed multiplier in BUILDING::TYPE
#endif
#ifndef MESH_ANIM_CTRL_OFFSET
#define MESH_ANIM_CTRL_OFFSET               0x1B0u   // C3D_MESH animation controller pointer
#endif
#ifndef ANIM_CTRL_MAX_FRAMES_OFFSET
#define ANIM_CTRL_MAX_FRAMES_OFFSET         0x09Cu   // int32_t total animation frames in controller
#endif
#ifndef BLD_PASS_BUCKET_COUNTER_RVA
#define BLD_PASS_BUCKET_COUNTER_RVA         0x9E03B8u // int32_t 1-in-3 building pass bucket counter (incremented at 0x13CB52)
#endif

static_assert(BLD_CONSTRUCTION_SITE_OFFSET == 0x628u &&
              BLD_ANIM_FRAME_OFFSET == BLD_ANIM_SUBMESH_OFFSET + 0x10u &&
              BLD_UTILIZATION_RATIO_OFFSET == BLD_PRODUCED_OFFSET - 4u &&
              BLD_TYPE_ANIM_SPEED_OFFSET == 0x630u &&
              MESH_ANIM_CTRL_OFFSET == 0x1B0u &&
              ANIM_CTRL_MAX_FRAMES_OFFSET == 0x09Cu &&
              BLD_PASS_BUCKET_COUNTER_RVA == 0x9E03B8u,
              "Building animated sub-mesh (+0x5E0/+0x5F0), type speed (+0x428), and mesh anim controller (+0x1B0/+0x9C) offsets must match v1.2.0.9 layout");

// Building per-type free / total count limit table (world + 0x58, BUILDING::TYPE + 0x208, 0x5F3180 / 0x5F3220 / 0x7AC6B7)
#ifndef WORLD_BLD_COUNT_LIMIT_VEC_PTR_OFFSET
#define WORLD_BLD_COUNT_LIMIT_VEC_PTR_OFFSET 0x0080u // std::vector<SovietCountLimitEntry>* pointer on WORLD (0x1409D4F68)
#endif
#ifndef BLD_TYPE_COUNT_LIMIT_OFFSET
#define BLD_TYPE_COUNT_LIMIT_OFFSET          0x0208u // int32_t $COUNT_LIMIT in BUILDING::TYPE
#endif
#ifndef RVA_BLD_COUNT_LIMIT_CHECK
#define RVA_BLD_COUNT_LIMIT_CHECK            0x61ACF0u // bool __fastcall(const void* limitTableVec, const void* btype)
#endif
#ifndef RVA_BLD_COUNT_LIMIT_GET
#define RVA_BLD_COUNT_LIMIT_GET              0x61AD90u // SovietCountLimitEntry* __fastcall(const void* limitTableVec, SovietCountLimitEntry* out, const void* btype)
#endif
#ifndef RVA_BLD_COUNT_LIMIT_TOOLTIP
#define RVA_BLD_COUNT_LIMIT_TOOLTIP          0x7EA697u // Inlined $COUNT_LIMIT lookup inside building card / tooltip (0x7ABF20)
#endif
#ifndef RVA_BLD_COUNT_LIMIT_TOOLTIP_FOUND
#define RVA_BLD_COUNT_LIMIT_TOOLTIP_FOUND    0x7EA72Au // Resume inside 0x7ABF20 when count-limit entry is found
#endif
#ifndef RVA_BLD_COUNT_LIMIT_TOOLTIP_SKIP
#define RVA_BLD_COUNT_LIMIT_TOOLTIP_SKIP     0x7EA930u // Resume inside 0x7ABF20 when count-limit entry is absent
#endif

static_assert(WORLD_BLD_COUNT_LIMIT_VEC_PTR_OFFSET == 0x0080u &&
              BLD_TYPE_COUNT_LIMIT_OFFSET == 0x0208u &&
              RVA_BLD_COUNT_LIMIT_GET == RVA_BLD_COUNT_LIMIT_CHECK + 0xA0u &&
              RVA_BLD_COUNT_LIMIT_TOOLTIP == 0x7EA697u &&
              RVA_BLD_COUNT_LIMIT_TOOLTIP_FOUND == RVA_BLD_COUNT_LIMIT_TOOLTIP + 0x93u &&
              RVA_BLD_COUNT_LIMIT_TOOLTIP_SKIP == RVA_BLD_COUNT_LIMIT_TOOLTIP + 0x299u,
              "Building count limit table pointer (+0x80), type limit (+0x208), and check/get/tooltip RVAs must match v1.2.0.9 layout");

// Distribution office, technical services, waste/transfer copy tools, carrier mechanism & storage slot RVAs
#ifndef RVA_VEC_CARGO_ENTRY_PUSH_BACK
#define RVA_VEC_CARGO_ENTRY_PUSH_BACK        0xC9690u // void __fastcall(void* vec, uint64_t count)
#endif
#ifndef RVA_OPERATOR_NEW
#define RVA_OPERATOR_NEW                     0x890C14u // void* __fastcall(size_t bytes)
#endif
#ifndef RVA_CARGO_RECORD_CTOR
#define RVA_CARGO_RECORD_CTOR                0x169D80u // void* __fastcall(void* rec0x198)
#endif
#ifndef RVA_VEC_PTR_PUSH_BACK
#define RVA_VEC_PTR_PUSH_BACK                0x54960u // void __fastcall(void* vec, const void* ptrAddr)
#endif
#ifndef RVA_VEC_LINK_PUSH_BACK
#define RVA_VEC_LINK_PUSH_BACK               0xA5DF0u // void __fastcall(void* vec, const void* ptrAddr)
#endif
#ifndef RVA_REMOVE_PARTNER_LINK
#define RVA_REMOVE_PARTNER_LINK              0x1E2120u // void __fastcall(uint64_t world, uint64_t bld, uint64_t partnerBld)
#endif
#ifndef RVA_REMOVE_LINK_BY_INDEX
#define RVA_REMOVE_LINK_BY_INDEX             0x17B490u // void __fastcall(uint64_t world, uint64_t bld, int index)
#endif
#ifndef SITE_COPY_DIST_OFFICE_RVA
#define SITE_COPY_DIST_OFFICE_RVA            0x2D536Eu
#endif
#ifndef FN_COPY_DIST_OFFICE_RVA
#define FN_COPY_DIST_OFFICE_RVA              0x2D53F0u
#endif
#ifndef SITE_COPY_WASTE_SETTINGS_RVA
#define SITE_COPY_WASTE_SETTINGS_RVA         0x1F65EEu
#endif
#ifndef FN_COPY_WASTE_SETTINGS_RVA
#define FN_COPY_WASTE_SETTINGS_RVA           0x1F6F00u
#endif
#ifndef SITE_COPY_TRANSFER_SETTINGS_RVA
#define SITE_COPY_TRANSFER_SETTINGS_RVA      0x1F693Fu
#endif
#ifndef FN_COPY_TRANSFER_SETTINGS_RVA
#define FN_COPY_TRANSFER_SETTINGS_RVA        0x1F7100u
#endif
#ifndef VEH_CARRIED_BEGIN_OFFSET
#define VEH_CARRIED_BEGIN_OFFSET             0x0488u   // std::vector<VEHICLE*> begin on flatbed carrier
#endif
#ifndef VEH_CARRIED_END_OFFSET
#define VEH_CARRIED_END_OFFSET               0x0490u   // std::vector<VEHICLE*> end on flatbed carrier
#endif
#ifndef VEH_DEPOT_PTR_OFFSET
#define VEH_DEPOT_PTR_OFFSET                 0x04F0u   // BUILDING* depot owner
#endif
#ifndef VEH_WORKPLACE_BLD_OFFSET
#define VEH_WORKPLACE_BLD_OFFSET             0x04F8u   // BUILDING* workplace / construction office owner
#endif
#ifndef BLD_VEHICLES_BEGIN_OFFSET
#define BLD_VEHICLES_BEGIN_OFFSET            0x0C70u   // std::vector<VEHICLE*> begin on workplace building
#endif
#ifndef BLD_VEHICLES_END_OFFSET
#define BLD_VEHICLES_END_OFFSET              0x0C78u   // std::vector<VEHICLE*> end on workplace building
#endif
#ifndef RVA_IS_VEHICLE_AT_DEPOT_OR_STOP
#define RVA_IS_VEHICLE_AT_DEPOT_OR_STOP      0x3DA6A0u // bool __fastcall(const void* world, const void* bld, const void* veh, uint8_t a4)
#endif
#ifndef RVA_VEH_FIND_PATH
#define RVA_VEH_FIND_PATH                    0x6B33C0u // bool __fastcall(void* veh, uint8_t a2, int32_t slotIdx, uint8_t a4, uint8_t a5)
#endif
#ifndef RVA_VEH_BUILD_WAYPOINTS
#define RVA_VEH_BUILD_WAYPOINTS              0x6B5B60u // void __fastcall(void* veh, uint8_t a2)
#endif
#ifndef RVA_SET_AUTO_REPLACE
#define RVA_SET_AUTO_REPLACE                 0x63C410u // void __fastcall(void* mgr, void* targetType, void* newType)
#endif
#ifndef SITE_AUTO_REPLACE_CLEAR_RVA
#define SITE_AUTO_REPLACE_CLEAR_RVA          0x84FE9Fu // purchase window: clear auto-replace rule -> RVA_SET_AUTO_REPLACE(mgr, targetType, nullptr)
#endif
#ifndef SITE_FACTORY_REPLACE_MACHINES_1_RVA
#define SITE_FACTORY_REPLACE_MACHINES_1_RVA  0x7B36C4u // factory window: replace machines (1st currency, r8d=0, r9b=1)
#endif
#ifndef SITE_FACTORY_REPLACE_MACHINES_2_RVA
#define SITE_FACTORY_REPLACE_MACHINES_2_RVA  0x7B3937u // factory window: replace machines (2nd currency, r8d=1, r9b=1)
#endif
#ifndef SITE_LINES_LIST_ASSIGN_VEHICLE_RVA
#define SITE_LINES_LIST_ASSIGN_VEHICLE_RVA   0x379610u // lines list: assign line to vehicle (line_set -> 0x497AA0)
#endif
#ifndef RVA_LINES_ASSIGN_VEHICLE
#define RVA_LINES_ASSIGN_VEHICLE             0x4BDF90u // void __fastcall(void* linesMgr, void* line, void* veh, u8 flag, void* zero)
#endif
#ifndef VEH_DEPOT_SLOT_IDX_OFFSET
#define VEH_DEPOT_SLOT_IDX_OFFSET            0x0510u   // int32_t depot / construction slot index (-1 when none)
#endif
#ifndef VEH_IS_RAIL_FLAG_OFFSET
#define VEH_IS_RAIL_FLAG_OFFSET              0x067Du   // uint8_t 1 for rail / track builder vehicles
#endif
#ifndef VEH_ORDERS_BEGIN_OFFSET
#define VEH_ORDERS_BEGIN_OFFSET              0x0680u   // std::vector<BUILDING*> begin on VEHICLE (8 bytes/ptr; alias of VEH_STOP_BLD_PTR_VEC_OFF)
#endif
#ifndef VEH_ORDERS_END_OFFSET
#define VEH_ORDERS_END_OFFSET                0x0688u   // std::vector<BUILDING*> end on VEHICLE
#endif
#ifndef VEH_ORDER_INDEX_OFFSET
#define VEH_ORDER_INDEX_OFFSET               0x0698u   // int32_t active order index on VEHICLE
#endif
#ifndef VEH_REVERSE_NEEDED_FLAG_OFFSET
#define VEH_REVERSE_NEEDED_FLAG_OFFSET       0xD78u   // uint8_t 1 when rail vehicle needs reversal/repath at end of waypoints
#endif
#ifndef VEH_TYPE_SKILLS_BEGIN_OFFSET
#define VEH_TYPE_SKILLS_BEGIN_OFFSET         0x8630u   // std::vector<VehicleSkill> begin on VEHICLE_TYPE (16 bytes/entry, +0x00 int32 skillId)
#endif
#ifndef VEH_TYPE_SKILLS_END_OFFSET
#define VEH_TYPE_SKILLS_END_OFFSET           0x8638u   // std::vector<VehicleSkill> end on VEHICLE_TYPE
#endif
#ifndef VEH_TYPE_SKILL_ENTRY_BYTES
#define VEH_TYPE_SKILL_ENTRY_BYTES           16u       // sizeof(VehicleSkill) in 0x66BD30..0x66BDFF (sar rax, 4)
#endif
#ifndef SKILL_CONSTRUCTION_RAIL_CRANE
#define SKILL_CONSTRUCTION_RAIL_CRANE        0x16      // $SKILL_CONSTRUCTION_RAIL_CRANE (0x3EEC90 / 0x66BD55)
#endif
#ifndef SKILL_CONSTRUCTION_RAIL_TRACK_LAYER
#define SKILL_CONSTRUCTION_RAIL_TRACK_LAYER  0x1A      // $SKILL_CONSTRUCTION_RAIL_TRACK_LAYER (0x3EED41 / 0x66BD9E)
#endif
#ifndef SKILL_CONSTRUCTION_RAIL_TUNNEL_BORING
#define SKILL_CONSTRUCTION_RAIL_TUNNEL_BORING 0x1B     // $SKILL_CONSTRUCTION_RAIL_TUNNEL_BORING (0x3EEEBB / 0x66BDC1)
#endif
#ifndef SKILL_CONSTRUCTION_RAIL_CRANE2
#define SKILL_CONSTRUCTION_RAIL_CRANE2       0x1C      // $SKILL_CONSTRUCTION_RAIL_CRANE2 (0x3EEDFE / 0x66BD7B)
#endif
#ifndef BLD_CONSTR_SITE_PTR_OFFSET
#define BLD_CONSTR_SITE_PTR_OFFSET           0x05D0u   // ConstructionSite* active construction site data on BUILDING
#endif

static_assert(VEH_CARRIED_END_OFFSET == VEH_CARRIED_BEGIN_OFFSET + 8u &&
              BLD_VEHICLES_END_OFFSET == BLD_VEHICLES_BEGIN_OFFSET + 8u &&
              VEH_WORKPLACE_BLD_OFFSET == VEH_CUR_BLD_OFFSET + 8u &&
              VEH_ORDERS_BEGIN_OFFSET == VEH_STOP_BLD_PTR_VEC_OFF &&
              VEH_ORDERS_END_OFFSET == VEH_ORDERS_BEGIN_OFFSET + 8u &&
              VEH_REVERSE_NEEDED_FLAG_OFFSET == 0xD78u &&
              VEH_TYPE_SKILLS_END_OFFSET == VEH_TYPE_SKILLS_BEGIN_OFFSET + 8u &&
              VEH_TYPE_SKILL_ENTRY_BYTES == 16u &&
              FN_COPY_DIST_OFFICE_RVA == 0x2D53F0u &&
              FN_COPY_WASTE_SETTINGS_RVA == 0x1F6F00u &&
              FN_COPY_TRANSFER_SETTINGS_RVA == 0x1F7100u &&
              RVA_IS_VEHICLE_AT_DEPOT_OR_STOP == 0x3DA6A0u &&
              RVA_VEH_FIND_PATH == 0x6B33C0u &&
              RVA_VEH_BUILD_WAYPOINTS == 0x6B5B60u &&
              RVA_SET_AUTO_REPLACE == 0x63C410u &&
              SITE_AUTO_REPLACE_CLEAR_RVA == 0x84FE9Fu &&
              SITE_FACTORY_REPLACE_MACHINES_1_RVA == 0x7B36C4u &&
              SITE_FACTORY_REPLACE_MACHINES_2_RVA == 0x7B3937u &&
              SITE_LINES_LIST_ASSIGN_VEHICLE_RVA == 0x379610u &&
              RVA_LINES_ASSIGN_VEHICLE == 0x4BDF90u,
              "Carrier mechanism, workplace vehicle list, rail path/waypoint RVAs, auto replacement RVA, and building copy tool RVAs must match v1.2.0.9 layout");

// ---------------------------------------------------------------------------
// R3 Medium Bugs (#5 Recall Personal Cars, #7 Factory Queue Worker Limit)
// ---------------------------------------------------------------------------

#ifndef RVA_DETACH_PERSONAL_CAR
#define RVA_DETACH_PERSONAL_CAR              0x873370u // void __fastcall(void* world, void* citizen)
#endif
#ifndef SITE_RECALL_DETACH_PERSONAL_CAR_RVA
#define SITE_RECALL_DETACH_PERSONAL_CAR_RVA  0x7DF968u // building window (party_member_car): detach personal car
#endif
#ifndef SITE_RECALL_SEND_TO_DEPOT_RVA
#define SITE_RECALL_SEND_TO_DEPOT_RVA        0x7DF98Bu // building window (party_member_car): send personal car to depot
#endif
#ifndef VEH_OWNER_CITIZEN_OFFSET
#define VEH_OWNER_CITIZEN_OFFSET             0x04E8u   // CITIZEN* personal car owner back-pointer on VEHICLE
#endif
#ifndef CIT_PERSONAL_CAR_OFFSET
#define CIT_PERSONAL_CAR_OFFSET              0x0688u   // VEHICLE* owned personal car pointer on CITIZEN
#endif

#ifndef RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT
#define RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT    0x1C53E0u // void __fastcall(void* unused, BUILDING* bld)
#endif
#ifndef SITE_FACTORY_QUEUE_LIMIT_DEC_RVA
#define SITE_FACTORY_QUEUE_LIMIT_DEC_RVA     0x78A026u // factory window: recompute queue worker limit (-)
#endif
#ifndef SITE_FACTORY_QUEUE_LIMIT_INC_RVA
#define SITE_FACTORY_QUEUE_LIMIT_INC_RVA     0x78A56Bu // factory window: recompute queue worker limit (+)
#endif
#ifndef BLD_FACTORY_MAX_WORKERS_OFFSET
#define BLD_FACTORY_MAX_WORKERS_OFFSET       0x0DF0u   // int32_t factory max workers limit on BUILDING
#endif
#ifndef BLD_FACTORY_QUEUE_BEGIN_OFFSET
#define BLD_FACTORY_QUEUE_BEGIN_OFFSET       0x0FF0u   // std::vector<FactoryQueueEntry> begin on BUILDING
#endif
#ifndef BLD_FACTORY_QUEUE_END_OFFSET
#define BLD_FACTORY_QUEUE_END_OFFSET         0x0FF8u   // std::vector<FactoryQueueEntry> end on BUILDING
#endif
#ifndef FACTORY_QUEUE_ENTRY_STRIDE
#define FACTORY_QUEUE_ENTRY_STRIDE           0x48u     // sizeof(FactoryQueueEntry); +0x0C workerLimit, +0x20 templateVeh
#endif

static_assert(RVA_DETACH_PERSONAL_CAR == 0x873370u &&
              SITE_RECALL_DETACH_PERSONAL_CAR_RVA == 0x7DF968u &&
              SITE_RECALL_SEND_TO_DEPOT_RVA == 0x7DF98Bu &&
              VEH_OWNER_CITIZEN_OFFSET == 0x04E8u &&
              CIT_PERSONAL_CAR_OFFSET == 0x0688u &&
              RVA_RECOMPUTE_FACTORY_QUEUE_LIMIT == 0x1C53E0u &&
              SITE_FACTORY_QUEUE_LIMIT_DEC_RVA == 0x78A026u &&
              SITE_FACTORY_QUEUE_LIMIT_INC_RVA == 0x78A56Bu &&
              BLD_FACTORY_MAX_WORKERS_OFFSET == 0x0DF0u &&
              BLD_FACTORY_QUEUE_END_OFFSET == BLD_FACTORY_QUEUE_BEGIN_OFFSET + 8u &&
              FACTORY_QUEUE_ENTRY_STRIDE == 0x48u,
              "M2 personal car recall (#5) and factory queue worker limit (#7) RVAs and offsets must match v1.2.0.9 layout");

// ---------------------------------------------------------------------------
// R1 Second-Pass Audit Fixes (BioWaste Container Split/Merge & SetForBio)
// ---------------------------------------------------------------------------

#ifndef SITE_WASTE_CONTAINER_SPLIT_RVA
#define SITE_WASTE_CONTAINER_SPLIT_RVA       0x7CBF7Au // waste/storage window: split container slot -> 0x19D8D0
#endif
#ifndef FN_WASTE_CONTAINER_SPLIT_RVA
#define FN_WASTE_CONTAINER_SPLIT_RVA         0x1B6570u // bool __fastcall(void* unused, BUILDING* bld, StorageSlot* srcSlot)
#endif
#ifndef SITE_WASTE_CONTAINER_MERGE_RVA
#define SITE_WASTE_CONTAINER_MERGE_RVA       0x7CC1ABu // waste/storage window: merge container slot -> 0x19D3D0
#endif
#ifndef FN_WASTE_CONTAINER_MERGE_RVA
#define FN_WASTE_CONTAINER_MERGE_RVA         0x1B6070u // bool __fastcall(void* world, BUILDING* bld, StorageSlot* dstSlot)
#endif
#ifndef SITE_SET_FOR_BIO_WASTE_WIN_RVA
#define SITE_SET_FOR_BIO_WASTE_WIN_RVA       0x73ED98u // bio-waste dump window (0x706DA0): SetForBio -> 0x18A8B0
#endif
#ifndef SITE_SET_FOR_BIO_RES_WIN_RVA
#define SITE_SET_FOR_BIO_RES_WIN_RVA         0x72E400u // building resources/compost window: SetForBio -> 0x18A8B0
#endif
#ifndef FN_SET_FOR_BIO_RVA
#define FN_SET_FOR_BIO_RVA                   0x1A3540u // float __fastcall(void* unused, StorageSlot* srcSlot, StorageSlot* dstSlot, float maxReq)
#endif
#ifndef BLD_BIO_TRANSFER_LOG0_OFFSET
#define BLD_BIO_TRANSFER_LOG0_OFFSET         0x1308u   // float bio-waste moved log accumulator (+moved)
#endif
#ifndef BLD_BIO_TRANSFER_LOG1_OFFSET
#define BLD_BIO_TRANSFER_LOG1_OFFSET         0x130Cu   // float bio-waste scaled log accumulator (+moved * 60.0f)
#endif
#ifndef FLT_BIO_SCALE_130C_RVA
#define FLT_BIO_SCALE_130C_RVA               0x952064u // .rdata float 60.0f multiplier for bld+0x130C at 0x6F848D
#endif

static_assert(BLD_STORAGE_OFFSET == 0x970u &&
              BLD_STORAGE_SLOT_SIZE == 0xE0u &&
              SITE_WASTE_CONTAINER_SPLIT_RVA == 0x7CBF7Au &&
              FN_WASTE_CONTAINER_SPLIT_RVA == 0x1B6570u &&
              SITE_WASTE_CONTAINER_MERGE_RVA == 0x7CC1ABu &&
              FN_WASTE_CONTAINER_MERGE_RVA == 0x1B6070u &&
              SITE_SET_FOR_BIO_WASTE_WIN_RVA == 0x73ED98u &&
              SITE_SET_FOR_BIO_RES_WIN_RVA == 0x72E400u &&
              FN_SET_FOR_BIO_RVA == 0x1A3540u &&
              BLD_BIO_TRANSFER_LOG0_OFFSET == 0x1308u &&
              BLD_BIO_TRANSFER_LOG1_OFFSET == BLD_BIO_TRANSFER_LOG0_OFFSET + 4u &&
              FLT_BIO_SCALE_130C_RVA == 0x952064u,
              "M3 R1-1A/1B waste container & SetForBio RVAs/offsets must match v1.2.0.9 layout");

// ---------------------------------------------------------------------------
// Firestation Dispatch & Assign-CO Click Mutators (SOVIET64.exe v1.1.1.13)
// ---------------------------------------------------------------------------

#ifndef FN_FIRE_DISPATCH_RVA
#define FN_FIRE_DISPATCH_RVA                 0x273440u // void __fastcall(void* unusedRcx, FireObj* fireObj, VEHICLE* veh)
#endif
#ifndef SITE_FIRESTATION_DISPATCH_ROAD_RVA
#define SITE_FIRESTATION_DISPATCH_ROAD_RVA   0x1EF7F9u // firestation_addfire road dispatch -> FN_FIRE_DISPATCH_RVA
#endif
#ifndef SITE_FIRESTATION_DISPATCH_HELI_RVA
#define SITE_FIRESTATION_DISPATCH_HELI_RVA   0x1EF978u // firestation_addfire heli dispatch -> FN_FIRE_DISPATCH_RVA
#endif
#ifndef SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA
#define SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA 0x1EF97Du // 7-byte mov byte ptr [rbx+0xD39], 0 immediately after 0x1D4FC8
#endif
#ifndef SITE_FIRESTATION_DISPATCH_HELI_SKIP_RVA
#define SITE_FIRESTATION_DISPATCH_HELI_SKIP_RVA  0x1EF984u // resume after 0xD39 clear in firestation_addfire heli loop
#endif
#ifndef BLD_FIRE_OBJ_OFFSET
#define BLD_FIRE_OBJ_OFFSET                  0x1160u   // BUILDING* -> FireObj* (distinct from VEH_STEER_TARGET_OFFSET = 0x1160 on VEHICLE*)
#endif
#ifndef WORLD_ASSIGN_CO_TOOL_OFFSET
#define WORLD_ASSIGN_CO_TOOL_OFFSET          0xD4E8u   // AssignCoTool* on WORLD (tool + 0xD0..0xD8 = std::vector<BUILDING*>)
#endif
#ifndef ASSIGN_CO_TOOL_OFFICES_OFF
#define ASSIGN_CO_TOOL_OFFICES_OFF           0x00D0u   // std::vector<BUILDING*> begin on AssignCoTool
#endif
#ifndef SITE_ASSIGN_CO_SINGLE_START_RVA
#define SITE_ASSIGN_CO_SINGLE_START_RVA      0x611786u // assign_co single regular -> RVA_START_PLANNED_CONSTRUCTION
#endif
#ifndef SITE_ASSIGN_CO_SINGLE_ASSIGN_RVA
#define SITE_ASSIGN_CO_SINGLE_ASSIGN_RVA     0x6117B1u // assign_co single regular -> RVA_ASSIGN_CONSTRUCTION_OFFICE
#endif
#ifndef SITE_ASSIGN_CO_SINGLE_RAIL_START_RVA
#define SITE_ASSIGN_CO_SINGLE_RAIL_START_RVA 0x611876u // assign_co single rail -> RVA_START_PLANNED_CONSTRUCTION
#endif
#ifndef SITE_ASSIGN_CO_SINGLE_RAIL_ASSIGN_RVA
#define SITE_ASSIGN_CO_SINGLE_RAIL_ASSIGN_RVA 0x611895u // assign_co single rail -> RVA_ASSIGN_CONSTRUCTION_OFFICE
#endif
#ifndef SITE_ASSIGN_CO_RECT_START_RVA
#define SITE_ASSIGN_CO_RECT_START_RVA        0x611BF6u // assign_co rect regular -> RVA_START_PLANNED_CONSTRUCTION
#endif
#ifndef SITE_ASSIGN_CO_RECT_ASSIGN_RVA
#define SITE_ASSIGN_CO_RECT_ASSIGN_RVA       0x611C21u // assign_co rect regular -> RVA_ASSIGN_CONSTRUCTION_OFFICE
#endif
#ifndef SITE_ASSIGN_CO_RECT_RAIL_START_RVA
#define SITE_ASSIGN_CO_RECT_RAIL_START_RVA   0x611CE6u // assign_co rect rail -> RVA_START_PLANNED_CONSTRUCTION
#endif
#ifndef SITE_ASSIGN_CO_RECT_RAIL_ASSIGN_RVA
#define SITE_ASSIGN_CO_RECT_RAIL_ASSIGN_RVA  0x611D05u // assign_co rect rail -> RVA_ASSIGN_CONSTRUCTION_OFFICE
#endif
#ifndef SITE_ASSIGN_CO_SINGLE_UNASSIGN_RVA
#define SITE_ASSIGN_CO_SINGLE_UNASSIGN_RVA   0x611E05u // assign_co single unassign -> RVA_UNASSIGN_CONSTRUCTION_OFFICE
#endif
#ifndef SITE_ASSIGN_CO_RECT_UNASSIGN_RVA
#define SITE_ASSIGN_CO_RECT_UNASSIGN_RVA     0x611ED6u // assign_co rect unassign -> RVA_UNASSIGN_CONSTRUCTION_OFFICE
#endif

static_assert(FN_FIRE_DISPATCH_RVA == 0x273440u &&
              SITE_FIRESTATION_DISPATCH_ROAD_RVA == 0x1EF7F9u &&
              SITE_FIRESTATION_DISPATCH_HELI_RVA == 0x1EF978u &&
              SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA == SITE_FIRESTATION_DISPATCH_HELI_RVA + 5u &&
              SITE_FIRESTATION_DISPATCH_HELI_SKIP_RVA == SITE_FIRESTATION_DISPATCH_HELI_AFTER_RVA + 7u &&
              BLD_FIRE_OBJ_OFFSET == 0x1160u &&
              WORLD_ASSIGN_CO_TOOL_OFFSET == 0xD4E8u &&
              ASSIGN_CO_TOOL_OFFICES_OFF == 0x00D0u &&
              SITE_ASSIGN_CO_SINGLE_START_RVA == 0x611786u &&
              SITE_ASSIGN_CO_SINGLE_ASSIGN_RVA == 0x6117B1u &&
              SITE_ASSIGN_CO_SINGLE_RAIL_START_RVA == 0x611876u &&
              SITE_ASSIGN_CO_SINGLE_RAIL_ASSIGN_RVA == 0x611895u &&
              SITE_ASSIGN_CO_RECT_START_RVA == 0x611BF6u &&
              SITE_ASSIGN_CO_RECT_ASSIGN_RVA == 0x611C21u &&
              SITE_ASSIGN_CO_RECT_RAIL_START_RVA == 0x611CE6u &&
              SITE_ASSIGN_CO_RECT_RAIL_ASSIGN_RVA == 0x611D05u &&
              SITE_ASSIGN_CO_SINGLE_UNASSIGN_RVA == 0x611E05u &&
              SITE_ASSIGN_CO_RECT_UNASSIGN_RVA == 0x611ED6u,
              "Firestation dispatch & assign_co click mutator RVAs/offsets must match v1.2.0.9 layout");

// ---------------------------------------------------------------------------
// Audit-7 Sync Fixes (SOVIET64.exe v1.1.1.13)
// ---------------------------------------------------------------------------

// Bug #2: Discard cargo / Empty storage / Empty parked vehicles
#ifndef FN_DISCARD_VEH_CARGO_RVA
#define FN_DISCARD_VEH_CARGO_RVA                0x6F2F30u // void __fastcall(VEHICLE* veh, uint8_t includeNonType11)
#endif
#ifndef FN_DISCARD_CARRIED_VEH_RVA
#define FN_DISCARD_CARRIED_VEH_RVA              0x3FFA60u // void __fastcall(WORLD*, VEHICLE* veh, uint8_t, uint8_t, uint8_t, uint8_t, float)
#endif
#ifndef FN_CAN_DISCARD_STORAGE_RVA
#define FN_CAN_DISCARD_STORAGE_RVA              0x124D90u // bool __fastcall(STORAGE* slot)
#endif
#ifndef SITE_DISCARD_VEH_CARGO_RVA
#define SITE_DISCARD_VEH_CARGO_RVA              0x848D8Au // discard popup: vehicle discard cargo -> FN_DISCARD_VEH_CARGO_RVA
#endif
#ifndef SITE_DISCARD_VEH_CARGO_AFTER_RVA
#define SITE_DISCARD_VEH_CARGO_AFTER_RVA        0x848D8Fu // immediately after 0x8084CA
#endif
#ifndef SITE_DISCARD_VEH_CARGO_SKIP_RVA
#define SITE_DISCARD_VEH_CARGO_SKIP_RVA         0x848E58u // skips unloading-counter reset 0x8084CF..0x808596 before carried-veh loop
#endif
#ifndef SITE_DISCARD_CARRIED_VEH_RVA
#define SITE_DISCARD_CARRIED_VEH_RVA            0x848F66u // discard popup: discard carried vehicle -> FN_DISCARD_CARRIED_VEH_RVA
#endif
#ifndef SITE_EMPTY_STORAGE_RVA
#define SITE_EMPTY_STORAGE_RVA                  0x84900Du // discard popup: empty whole storage check -> FN_CAN_DISCARD_STORAGE_RVA
#endif
#ifndef SITE_EMPTY_BLD_VEH_CARGO_POPUP_RVA
#define SITE_EMPTY_BLD_VEH_CARGO_POPUP_RVA      0x8490B0u // discard popup: empty parked vehicle cargo -> FN_DISCARD_VEH_CARGO_RVA
#endif
#ifndef SITE_EMPTY_BLD_VEH_CARGO_CONFIRM_RVA
#define SITE_EMPTY_BLD_VEH_CARGO_CONFIRM_RVA    0x781492u // building confirm: empty parked vehicle cargo -> FN_DISCARD_VEH_CARGO_RVA
#endif
#ifndef VEH_ATTACHED_WAGONS_BEGIN_OFF
#define VEH_ATTACHED_WAGONS_BEGIN_OFF           0x03A0u   // std::vector<VEHICLE*> begin on VEHICLE*
#endif
#ifndef VEH_ATTACHED_WAGONS_END_OFF
#define VEH_ATTACHED_WAGONS_END_OFF             0x03A8u   // std::vector<VEHICLE*> end on VEHICLE*
#endif
#ifndef VEH_LOAD_TIMER_55C_OFF
#define VEH_LOAD_TIMER_55C_OFF                  0x055Cu   // qword cleared in 0x8084F6
#endif
#ifndef VEH_LOAD_STATE_564_OFF
#define VEH_LOAD_STATE_564_OFF                  0x0564u   // dword cleared in 0x808514
#endif
#ifndef VEH_LOAD_RATE_568_OFF
#define VEH_LOAD_RATE_568_OFF                   0x0568u   // float reset to 1.0f in 0x808504
#endif
#ifndef VEH_LOAD_TIMER_594_OFF
#define VEH_LOAD_TIMER_594_OFF                  0x0594u   // qword cleared in 0x8084FD
#endif
#ifndef VEH_LOAD_FLAG_59C_OFF
#define VEH_LOAD_FLAG_59C_OFF                   0x059Cu   // byte cleared in 0x808520
#endif
#ifndef VEH_LOAD_STATE_1310_OFF
#define VEH_LOAD_STATE_1310_OFF                 0x1348u   // dword cleared in 0x80851A
#endif
#ifndef VEH_LOAD_STATE_179C_OFF
#define VEH_LOAD_STATE_179C_OFF                 0x1834u   // dword cleared in 0x80850E
#endif

// Bug #3: Repair Station vehicle queue cancellation
#ifndef FN_REPAIR_CANCEL_QUEUE_RVA
#define FN_REPAIR_CANCEL_QUEUE_RVA              0x1C6EE0u // void __fastcall(WORLD*, BUILDING*, int idx, uint8_t flag)
#endif
#ifndef SITE_REPAIR_CANCEL_QUEUE_RVA
#define SITE_REPAIR_CANCEL_QUEUE_RVA            0x7E657Eu // repair station queue cancel -> FN_REPAIR_CANCEL_QUEUE_RVA
#endif

// Bug #4: Crime/Emergency Cancel assignment
#ifndef FN_UI_PLAY_SOUND_RVA
#define FN_UI_PLAY_SOUND_RVA                    0x422E60u // UI sound helper (world, strPtr, uint8_t)
#endif
#ifndef FN_CRIME_DELETE_CASE_RVA
#define FN_CRIME_DELETE_CASE_RVA                0x273230u // void __fastcall(WORLD*, CrimeCase*)
#endif
#ifndef SITE_CANCEL_CRIME_ASSIGN_RVA
#define SITE_CANCEL_CRIME_ASSIGN_RVA            0x7A5B0Cu // crime window: cancel assignment -> FN_UI_PLAY_SOUND_RVA
#endif
#ifndef SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA
#define SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA      0x7A5B11u // immediately after 0x76874C
#endif
#ifndef SITE_CANCEL_CRIME_ASSIGN_SKIP_RVA
#define SITE_CANCEL_CRIME_ASSIGN_SKIP_RVA       0x7A5BE1u // register-identical to non-clicked branch 0x768739 -> 0x768821
#endif
#ifndef WORLD_CRIME_CASES_BEGIN_OFF
#define WORLD_CRIME_CASES_BEGIN_OFF             0xD308u   // std::vector<CrimeCase*> begin
#endif
#ifndef WORLD_CRIME_CASES_END_OFF
#define WORLD_CRIME_CASES_END_OFF               0xD310u   // std::vector<CrimeCase*> end
#endif
#ifndef WORLD_CRIME_EXPIRED_STAT0_OFF
#define WORLD_CRIME_EXPIRED_STAT0_OFF           0x116F8u  // world dword stat (crimeType == 0)
#endif
#ifndef WORLD_CRIME_EXPIRED_STAT2_OFF
#define WORLD_CRIME_EXPIRED_STAT2_OFF           0x116FCu  // world dword stat (crimeType == 2)
#endif
#ifndef WORLD_CRIME_EXPIRED_STAT3_OFF
#define WORLD_CRIME_EXPIRED_STAT3_OFF           0x11700u  // world dword stat (crimeType == 3)
#endif
#ifndef CASE_STATUS_OFF
#define CASE_STATUS_OFF                         0x04u     // int32 status on CrimeCase*
#endif
#ifndef CASE_BLD_OFF
#define CASE_BLD_OFF                            0x08u     // BUILDING* on CrimeCase*
#endif
#ifndef CASE_TYPE_OFF
#define CASE_TYPE_OFF                           0x70u     // int32 crimeType on CrimeCase*
#endif
#ifndef BLD_CRIME_CANCEL_COUNTERS_OFF
#define BLD_CRIME_CANCEL_COUNTERS_OFF           0x13F8u   // uint16[status-3] at stride 4 on BUILDING*
#endif
#ifndef BLD_CITY_REGION_PTR_OFF
#define BLD_CITY_REGION_PTR_OFF                 0x0108u   // Region* on BUILDING*
#endif
#ifndef REGION_CITY_STATS_PTR_OFF
#define REGION_CITY_STATS_PTR_OFF               0x01B8u   // CityStats* on Region*
#endif
#ifndef CITY_STATS_CRIME0_OFF
#define CITY_STATS_CRIME0_OFF                   0x0330u   // CityStats dword (crimeType == 0)
#endif
#ifndef CITY_STATS_CRIME2_OFF
#define CITY_STATS_CRIME2_OFF                   0x0334u   // CityStats dword (crimeType == 2)
#endif
#ifndef CITY_STATS_CRIME3_OFF
#define CITY_STATS_CRIME3_OFF                   0x0338u   // CityStats dword (crimeType == 3)
#endif

// Bug #5: Distribution Office "Find connections with factories and storages"
#ifndef SITE_DO_FIND_CONNECTIONS_RVA
#define SITE_DO_FIND_CONNECTIONS_RVA            0x7826C6u // DO window: Pathfinder ctor -> FN_PF_CTOR_RVA
#endif
#ifndef SITE_DO_FIND_CONNECTIONS_AFTER_RVA
#define SITE_DO_FIND_CONNECTIONS_AFTER_RVA      0x7826CBu // immediately after 0x74AA16
#endif
#ifndef SITE_DO_FIND_CONNECTIONS_SKIP_RVA
#define SITE_DO_FIND_CONNECTIONS_SKIP_RVA       0x7829E5u // register-identical to non-clicked branch 0x74AA09 -> 0x74AD35
#endif
#ifndef FN_PF_CTOR_RVA
#define FN_PF_CTOR_RVA                          0x1051C0u // Pathfinder* __fastcall(void* pf)
#endif
#ifndef FN_PF_DTOR_RVA
#define FN_PF_DTOR_RVA                          0x105300u // void __fastcall(void* pf)
#endif
#ifndef FN_DO_REMOVE_LINK_RVA
#define FN_DO_REMOVE_LINK_RVA                   0x1E2040u // void __fastcall(U64 unused, BUILDING* doBld, BUILDING* targetBld)
#endif
#ifndef FN_PF_ADD_START_RVA
#define FN_PF_ADD_START_RVA                     0x5AA0A0u // void __fastcall(void* pf, U64 nodePtr, float, int, float)
#endif
#ifndef FN_PF_RUN_RVA
#define FN_PF_RUN_RVA                           0x5A8900u // void __fastcall(void* pf)
#endif
#ifndef FN_DO_VEC_PUSH_RVA
#define FN_DO_VEC_PUSH_RVA                      0xA5DF0u // std::vector<DOEntry*>::push_back(&vec, &entry)
#endif
#ifndef FN_GAME_OP_NEW_RVA
#define FN_GAME_OP_NEW_RVA                      0x890C14u // void* __fastcall(size_t bytes)
#endif
#ifndef FN_DO_CFG_CTOR_RVA
#define FN_DO_CFG_CTOR_RVA                      0x169D80u // DOCfg* __fastcall(void* cfg)
#endif
#ifndef FLT_NEG_ONE_RVA
#define FLT_NEG_ONE_RVA                         0x95294Cu // float -1.0f constant in .rdata
#endif
#ifndef BLD_DO_CONNECTIONS_BEGIN_OFF
#define BLD_DO_CONNECTIONS_BEGIN_OFF            0x0D38u   // std::vector<DOEntry*> begin on DO BUILDING*
#endif
#ifndef BLD_DO_CONNECTIONS_END_OFF
#define BLD_DO_CONNECTIONS_END_OFF              0x0D40u   // std::vector<DOEntry*> end on DO BUILDING*
#endif
#ifndef BLD_DO_HAS_CONN_FLAG_OFF
#define BLD_DO_HAS_CONN_FLAG_OFF                0x0D50u   // byte flag set to 1 on DO BUILDING*
#endif
#ifndef BLD_DO_TIMER_OFF
#define BLD_DO_TIMER_OFF                        0x0FC0u   // float timer set to 3.0f on DO BUILDING*
#endif
#ifndef BLD_DO_VEH_ENTRIES_BEGIN_OFF
#define BLD_DO_VEH_ENTRIES_BEGIN_OFF            0x0A10u   // parked vehicle entries begin on DO BUILDING*
#endif
#ifndef BLD_DO_VEH_ENTRIES_END_OFF
#define BLD_DO_VEH_ENTRIES_END_OFF              0x0A18u   // parked vehicle entries end on DO BUILDING*
#endif
#ifndef BLD_DO_VEH_ENTRY_STRIDE
#define BLD_DO_VEH_ENTRY_STRIDE                 0x60u     // stride of each entry in DO_VEH_ENTRIES
#endif

// Bug #6: building_copyweights Late tool
#ifndef SITE_TOOL_COPY_WEIGHTS_RVA
#define SITE_TOOL_COPY_WEIGHTS_RVA              0x332E37u // Late tool dispatch -> FN_TOOL_COPY_WEIGHTS_RVA
#endif
#ifndef FN_TOOL_COPY_WEIGHTS_RVA
#define FN_TOOL_COPY_WEIGHTS_RVA                0x1F1AF0u // U64 __fastcall(WORLD*, U64)
#endif
#ifndef WORLD_COPY_TOOL_SRC_BLD_OFF
#define WORLD_COPY_TOOL_SRC_BLD_OFF             0xD338u   // source BUILDING* on WORLD*
#endif
#ifndef WORLD_HOVERED_BLD_OFF
#define WORLD_HOVERED_BLD_OFF                   0xF658u   // hovered BUILDING* written by 0x1D7140
#endif
#ifndef BLD_COPY_WEIGHTS_BEGIN_OFFSET
#define BLD_COPY_WEIGHTS_BEGIN_OFFSET           0x11F4u   // first of 9 dwords copied by 0x1D7140
#endif
#ifndef BLD_COPY_WEIGHTS_END_OFFSET
#define BLD_COPY_WEIGHTS_END_OFFSET             0x1218u   // exclusive end (9 * 4 = 36 bytes)
#endif
#ifndef BLD_IS_PLANNED_OFF
#define BLD_IS_PLANNED_OFF                      0x0EA8u   // non-zero while building is planned construction
#endif
#ifndef BLD_BUILT_PROGRESS_OFF
#define BLD_BUILT_PROGRESS_OFF                  0x0604u   // float built progress (>= 1.0f when completed)
#endif

// Bug #7: Pre-0x3370F0 building storage container rebuild loop
#ifndef FN_REBUILD_BLD_STORAGES_RVA
#define FN_REBUILD_BLD_STORAGES_RVA             0x1B9E80u // void __fastcall(WORLD*, BUILDING*)
#endif
#ifndef SITE_GAME_RULES_REBUILD_1_RVA
#define SITE_GAME_RULES_REBUILD_1_RVA           0x355D4Cu // settings page close loop -> FN_REBUILD_BLD_STORAGES_RVA
#endif
#ifndef SITE_GAME_RULES_REBUILD_2_RVA
#define SITE_GAME_RULES_REBUILD_2_RVA           0x35921Cu // settings page save loop -> FN_REBUILD_BLD_STORAGES_RVA
#endif
#ifndef SITE_GAME_RULES_REBUILD_3_RVA
#define SITE_GAME_RULES_REBUILD_3_RVA           0x83F47Cu // settings popup apply loop -> FN_REBUILD_BLD_STORAGES_RVA
#endif
#ifndef WORLD_RULE_COMPLEX_STORAGES_OFF
#define WORLD_RULE_COMPLEX_STORAGES_OFF         0x05E0u   // active rule dword compared with WORLD+0xC848
#endif
#ifndef WORLD_RULE_COPY_COMPLEX_STORAGES_OFF
#define WORLD_RULE_COPY_COMPLEX_STORAGES_OFF    0xC8E8u   // edited copy dword (> 1 triggers 0x1A11E0)
#endif

static_assert(FN_DISCARD_VEH_CARGO_RVA == 0x6F2F30u &&
              FN_DISCARD_CARRIED_VEH_RVA == 0x3FFA60u &&
              FN_CAN_DISCARD_STORAGE_RVA == 0x124D90u &&
              SITE_DISCARD_VEH_CARGO_RVA == 0x848D8Au &&
              SITE_DISCARD_VEH_CARGO_AFTER_RVA == SITE_DISCARD_VEH_CARGO_RVA + 5u &&
              SITE_DISCARD_VEH_CARGO_SKIP_RVA == 0x848E58u &&
              SITE_DISCARD_CARRIED_VEH_RVA == 0x848F66u &&
              SITE_EMPTY_STORAGE_RVA == 0x84900Du &&
              SITE_EMPTY_BLD_VEH_CARGO_POPUP_RVA == 0x8490B0u &&
              SITE_EMPTY_BLD_VEH_CARGO_CONFIRM_RVA == 0x781492u &&
              FN_REPAIR_CANCEL_QUEUE_RVA == 0x1C6EE0u &&
              SITE_REPAIR_CANCEL_QUEUE_RVA == 0x7E657Eu &&
              FN_UI_PLAY_SOUND_RVA == 0x422E60u &&
              FN_CRIME_DELETE_CASE_RVA == 0x273230u &&
              SITE_CANCEL_CRIME_ASSIGN_RVA == 0x7A5B0Cu &&
              SITE_CANCEL_CRIME_ASSIGN_AFTER_RVA == SITE_CANCEL_CRIME_ASSIGN_RVA + 5u &&
              SITE_CANCEL_CRIME_ASSIGN_SKIP_RVA == 0x7A5BE1u &&
              WORLD_CRIME_CASES_END_OFF == WORLD_CRIME_CASES_BEGIN_OFF + 8u &&
              SITE_DO_FIND_CONNECTIONS_RVA == 0x7826C6u &&
              SITE_DO_FIND_CONNECTIONS_AFTER_RVA == SITE_DO_FIND_CONNECTIONS_RVA + 5u &&
              SITE_DO_FIND_CONNECTIONS_SKIP_RVA == 0x7829E5u &&
              FN_PF_CTOR_RVA == 0x1051C0u &&
              FN_PF_DTOR_RVA == 0x105300u &&
              FN_DO_REMOVE_LINK_RVA == 0x1E2040u &&
              FN_PF_ADD_START_RVA == 0x5AA0A0u &&
              FN_PF_RUN_RVA == 0x5A8900u &&
              FN_DO_VEC_PUSH_RVA == 0x0A5DF0u &&
              FN_GAME_OP_NEW_RVA == 0x890C14u &&
              FN_DO_CFG_CTOR_RVA == 0x169D80u &&
              FLT_NEG_ONE_RVA == 0x95294Cu &&
              BLD_DO_CONNECTIONS_END_OFF == BLD_DO_CONNECTIONS_BEGIN_OFF + 8u &&
              BLD_DO_VEH_ENTRIES_END_OFF == BLD_DO_VEH_ENTRIES_BEGIN_OFF + 8u &&
              SITE_TOOL_COPY_WEIGHTS_RVA == 0x332E37u &&
              FN_TOOL_COPY_WEIGHTS_RVA == 0x1F1AF0u &&
              BLD_COPY_WEIGHTS_END_OFFSET == BLD_COPY_WEIGHTS_BEGIN_OFFSET + 36u &&
              FN_REBUILD_BLD_STORAGES_RVA == 0x1B9E80u &&
              SITE_GAME_RULES_REBUILD_1_RVA == 0x355D4Cu &&
              SITE_GAME_RULES_REBUILD_2_RVA == 0x35921Cu &&
              SITE_GAME_RULES_REBUILD_3_RVA == 0x83F47Cu,
              "Audit-7 sync fix RVAs and offsets must match v1.2.0.9 layout");

// ---------------------------------------------------------------------------
// Hotfix v1.7.1: Crash B (Filling worker UAF), Client Exit-to-Menu, Crash A (Signal block)
// ---------------------------------------------------------------------------

// Filling worker & queues (0x2592C0 / 0x259350 / 0x294BA5)
#ifndef WORLD_FILLING_VEHICLES_OFFSET
#define WORLD_FILLING_VEHICLES_OFFSET       0x128E0u  // 0x9E7738 - WORLD_RVA (0x9D4F10)
#endif
#ifndef VEC_FILLING_VEHICLES_BEGIN_RVA
#define VEC_FILLING_VEHICLES_BEGIN_RVA      (WORLD_RVA + WORLD_FILLING_VEHICLES_OFFSET) // 0x9E7738
#endif
#ifndef VEC_FILLING_VEHICLES_END_RVA
#define VEC_FILLING_VEHICLES_END_RVA        (VEC_FILLING_VEHICLES_BEGIN_RVA + 8u)        // 0x9E7740
#endif
#ifndef FILLING_OBJ_RVA
#define FILLING_OBJ_RVA                     0x9DDC80u // DAT_140995290: Filling worker object ptr
#endif
#ifndef FILLING_QUIT_OFFSET
#define FILLING_QUIT_OFFSET                 0x28u     // byte: 1 when 0x2592C0 signals worker shutdown
#endif
#ifndef FILLING_DONE_OFFSET
#define FILLING_DONE_OFFSET                 0x59u     // byte: 1 when current Filling step finishes
#endif
#ifndef FILLING_START_EVENT_OFFSET
#define FILLING_START_EVENT_OFFSET          0x60u     // HANDLE: FillingrStart event
#endif
#ifndef FILLING_QUEUE_PTR_RVA
#define FILLING_QUEUE_PTR_RVA               0xA317D8u // [exe+0x9E7710]+0x60 transient Filling queue
#endif
#ifndef FILLING_PATHS_PTR_RVA
#define FILLING_PATHS_PTR_RVA               0xA2C400u // [exe+0x9E2350]+0x40 transient Filling paths
#endif

// Client Pause/Exit-to-Main-Menu trigger flags (0x10520B / 0x1059B0 / 0x3151A5)
#ifndef EXIT_TO_MENU_FLAG_RVA
#define EXIT_TO_MENU_FLAG_RVA               0x9DC090u // byte: 1 triggers UnloadWorld + Main Menu
#endif
#ifndef WORLD_EXIT_DESKTOP_FLAG_OFF
#define WORLD_EXIT_DESKTOP_FLAG_OFF         0x5064u   // byte on WORLD*: 1 = exit to desktop, 0 = Main Menu
#endif

// Rail signal block recalculation guard (Crash A: 0x41E9F0 / 0x41EB40 / 0x41EDF0)
#ifndef RVA_UPDATE_SIGNAL_BLOCK
#define RVA_UPDATE_SIGNAL_BLOCK             0x442C20u // void __fastcall(WORLD*, void* segPtr)
#endif
#ifndef SEG_CONNECTED_NODES_BEGIN_OFF
#define SEG_CONNECTED_NODES_BEGIN_OFF       0x10u     // std::vector<RAIL_NODE*> begin on segment
#endif
#ifndef SEG_CONNECTED_NODES_END_OFF
#define SEG_CONNECTED_NODES_END_OFF         0x18u     // std::vector<RAIL_NODE*> end on segment
#endif
#ifndef SEG_SIGNAL_BLOCK_PTR_OFFSET
#define SEG_SIGNAL_BLOCK_PTR_OFFSET         0xB8u     // RAIL_SIGNAL_BLOCK* on segment (0x41EA00)
#endif
#ifndef SIGNAL_BLOCK_NODE_PTR_OFFSET
#define SIGNAL_BLOCK_NODE_PTR_OFFSET        0x08u     // RAIL_NODE* read at 0x41EB5A in 0x41EB40
#endif
#ifndef SIGNAL_BLOCK_KIND_OFFSET
#define SIGNAL_BLOCK_KIND_OFFSET            0x14u     // uint32_t kind read at 0x41EA0C
#endif
#ifndef SIGNAL_BLOCK_KIND_SKIP
#define SIGNAL_BLOCK_KIND_SKIP              0x0Au     // kind == 0x0A takes direct-nodes path
#endif

static_assert(VEC_FILLING_VEHICLES_BEGIN_RVA == 0xA31800u &&
              VEC_FILLING_VEHICLES_END_RVA == 0xA31808u &&
              FILLING_OBJ_RVA == 0x9DDC80u &&
              FILLING_QUIT_OFFSET == 0x28u &&
              FILLING_DONE_OFFSET == 0x59u &&
              FILLING_START_EVENT_OFFSET == 0x60u &&
              FILLING_QUEUE_PTR_RVA == 0xA317D8u &&
              FILLING_PATHS_PTR_RVA == 0xA2C400u &&
              EXIT_TO_MENU_FLAG_RVA == 0x9DC090u &&
              WORLD_EXIT_DESKTOP_FLAG_OFF == 0x5064u &&
              RVA_UPDATE_SIGNAL_BLOCK == 0x442C20u &&
              SEG_CONNECTED_NODES_BEGIN_OFF == 0x10u &&
              SEG_CONNECTED_NODES_END_OFF == 0x18u &&
              SEG_SIGNAL_BLOCK_PTR_OFFSET == 0xB8u &&
              SIGNAL_BLOCK_NODE_PTR_OFFSET == 0x08u &&
              SIGNAL_BLOCK_KIND_OFFSET == 0x14u &&
              SIGNAL_BLOCK_KIND_SKIP == 0x0Au,
              "Hotfix v1.7.1 RVAs and struct offsets must match SOVIET64.exe v1.2.0.9 disassembly");

// Rail Construction Office (RCO) & Rail Construction Fixes (v1.1.1.13)
// Site CO-enable (0x160570) & Site Suspend/Cancel (0x161440) UI caller return RVAs (call + 5)
#ifndef RVA_SITE_STOP_UI_RET
#define RVA_SITE_STOP_UI_RET                0x730087u // 0x6FA0E2 + 5 (UI toggle to enable CO construction)
#endif
#ifndef BLD_STOPPED_OFFSET
#define BLD_STOPPED_OFFSET                  0x879u    // byte on BUILDING: 1 = CO construction enabled, 0 = suspended/stopped
#endif
#ifndef RVA_SITE_RESUME_UI_RET_SUSPEND
#define RVA_SITE_RESUME_UI_RET_SUSPEND      0x72FE12u // 0x6F9E6D + 5 (UI toggle to suspend CO construction)
#endif
#ifndef RVA_SITE_RESUME_UI_RET_PAY_RUB
#define RVA_SITE_RESUME_UI_RET_PAY_RUB      0x730ACEu // 0x6FAB09 + 5 (UI button: finish with rubles)
#endif
#ifndef RVA_SITE_RESUME_UI_RET_PAY_USD
#define RVA_SITE_RESUME_UI_RET_PAY_USD      0x730EE8u // 0x6FAF23 + 5 (UI button: finish with dollars)
#endif
#ifndef RVA_SITE_RESUME_UI_RET_WIN2_RUB
#define RVA_SITE_RESUME_UI_RET_WIN2_RUB     0x7B3012u // 0x775C2D + 5 (second window: finish with rubles)
#endif
#ifndef RVA_SITE_RESUME_UI_RET_WIN2_USD
#define RVA_SITE_RESUME_UI_RET_WIN2_USD     0x7B32E6u // 0x775F01 + 5 (second window: finish with dollars)
#endif

// Rail CO, Demolition Office & Auto-Chain constants
#ifndef RVA_CALLER_RAIL_CHAIN_ASSIGN
#define RVA_CALLER_RAIL_CHAIN_ASSIGN        0x17287Du // 0x159F48 + 5 (ConstructStep rail next-segment auto-chain -> 0x161CB0)
#endif
#ifndef BLD_CAT_RAIL_CO
#define BLD_CAT_RAIL_CO                     0x1Cu     // 28: Rail Construction Office (BUILDING::TYPE+0x360)
#endif
#ifndef BLD_CAT_DEMOLISH_CO
#define BLD_CAT_DEMOLISH_CO                 0x6Au     // 106: Demolition Office (BUILDING::TYPE+0x360)
#endif
#ifndef BLD_IS_DEMOLITION_SITE_OFFSET
#define BLD_IS_DEMOLITION_SITE_OFFSET       0xEA8u    // non-zero byte on demolition site (planned construction)
#endif
#ifndef BLD_TYPE_CAT_OFF
#define BLD_TYPE_CAT_OFF                    0x368u    // int32_t category on BUILDING::TYPE
#endif
#ifndef BLD_TYPE_CUSTOMS_SUBTYPE_OFF
#define BLD_TYPE_CUSTOMS_SUBTYPE_OFF        0x738u    // uint8_t customs border subtype on BUILDING::TYPE (was 0x528)
#endif
#ifndef BLD_TYPE_IS_DORM_OFF
#define BLD_TYPE_IS_DORM_OFF                0xAF8u    // uint8_t dormant/student dorm flag on BUILDING::TYPE (was 0x8E8)
#endif

// RCO/CO Queue Reorder Popup (0x7707E0) constants
#ifndef RVA_OFFICE_QUEUE_REORDER
#define RVA_OFFICE_QUEUE_REORDER            0x7ADBC0u // void __fastcall(std::vector<ListItem>* itemsVec, BYTE* office)
#endif
#ifndef RVA_OFFICE_REORDER_FREE_LIST
#define RVA_OFFICE_REORDER_FREE_LIST        0x459E0u // std::vector<ListItem>::~vector() tail-called by 0x7707E0
#endif
#ifndef BLD_ASSIGNED_SITES_BEGIN_OFF
#define BLD_ASSIGNED_SITES_BEGIN_OFF        0xF90u    // std::vector<BUILDING*> begin on CO/RCO
#endif
#ifndef BLD_ASSIGNED_SITES_END_OFF
#define BLD_ASSIGNED_SITES_END_OFF          0xF98u    // std::vector<BUILDING*> end on CO/RCO
#endif
#ifndef BLD_ASSIGNED_DISTS_BEGIN_OFF
#define BLD_ASSIGNED_DISTS_BEGIN_OFF        0xFA8u    // std::vector<float> begin on CO/RCO
#endif
#ifndef BLD_ASSIGNED_DISTS_END_OFF
#define BLD_ASSIGNED_DISTS_END_OFF          0xFB0u    // std::vector<float> end on CO/RCO
#endif
#ifndef OFFICE_REORDER_ITEM_STRIDE
#define OFFICE_REORDER_ITEM_STRIDE          0x18u     // 24 bytes per popup list item
#endif
#ifndef OFFICE_REORDER_ITEM_IDX_OFF
#define OFFICE_REORDER_ITEM_IDX_OFF         0x10u     // float(oldIdx) at +0x10 inside each 24-byte ListItem
#endif

static_assert(BLD_STOPPED_OFFSET == 0x879u &&
              BLD_ASSIGNED_SITES_END_OFF == BLD_ASSIGNED_SITES_BEGIN_OFF + 8u &&
              BLD_ASSIGNED_DISTS_BEGIN_OFF == BLD_ASSIGNED_SITES_BEGIN_OFF + 0x18u &&
              BLD_ASSIGNED_DISTS_END_OFF == BLD_ASSIGNED_DISTS_BEGIN_OFF + 8u &&
              OFFICE_REORDER_ITEM_IDX_OFF + 4u <= OFFICE_REORDER_ITEM_STRIDE &&
              RVA_CALLER_RAIL_CHAIN_ASSIGN > RVA_CALLER_OFFICE_ROAD_SCAN &&
              RVA_CALLER_RAIL_CHAIN_ASSIGN < RVA_ASSIGN_CONSTRUCTION_OFFICE &&
              BLD_CAT_RAIL_CO == 28u && BLD_CAT_DEMOLISH_CO == 106u &&
              BLD_IS_DEMOLITION_SITE_OFFSET == BLD_IS_PLANNED_OFF,
              "RCO/Rail construction RVAs and offsets must match SOVIET64.exe v1.2.0.9 disassembly");

// Space Program DLC (dlc5) offsets & RVAs (v1.2.0.9, PE 6B05A574)
#ifndef WORLD_ACTIVE_ORBIT_VIEW_OFFSET
#define WORLD_ACTIVE_ORBIT_VIEW_OFFSET      0x0014u // Local UI selected orbit index - NEVER hash
#endif
#ifndef WORLD_ORBIT_CLOCK_OFFSET
#define WORLD_ORBIT_CLOCK_OFFSET            0x0018u // float global orbital clock (orbits.dta header)
#endif
#ifndef WORLD_ORBITS_VEC_OFFSET
#define WORLD_ORBITS_VEC_OFFSET             0x0020u // std::vector<ORBIT_SLOT> (+0x20 begin, +0x28 end)
#endif
#ifndef ORBIT_SLOT_STRIDE
#define ORBIT_SLOT_STRIDE                   0x0060u // sizeof(ORBIT_SLOT) == 96 bytes (flat POD in orbits.dta)
#endif
#ifndef ORBIT_SLOT_TYPE_OFFSET
#define ORBIT_SLOT_TYPE_OFFSET              0x0028u // int32_t orbit type/direction (from pad+0xCD8)
#endif
#ifndef ORBIT_SLOT_START_CLOCK_OFFSET
#define ORBIT_SLOT_START_CLOCK_OFFSET       0x0044u // float orbit start clock (synced in LoadOrbits)
#endif
#ifndef VEH_ORBIT_INDEX_OFFSET
#define VEH_ORBIT_INDEX_OFFSET              0x175Cu // int32_t m_iVehicleOrbitIndex (-1 when not on orbit)
#endif
#ifndef VEH_TARGET_ORBIT_INDEX_OFFSET
#define VEH_TARGET_ORBIT_INDEX_OFFSET       0x1760u // int32_t m_iTargetOrbitIndex (-1 deorbit, -2 reentry, >=0 target orbit)
#endif
#ifndef VEH_SPACE_DOCK_PARTNER_OFFSET
#define VEH_SPACE_DOCK_PARTNER_OFFSET       0x1768u // Vehicle* docked/approaching orbital station
#endif
#ifndef VEH_SPACE_DOCK_SLOT_OFFSET
#define VEH_SPACE_DOCK_SLOT_OFFSET          0x1770u // int32_t docking port index (-1 when undocked)
#endif
#ifndef VEH_SPACE_DOCK_APPROACH_FLAG_OFF
#define VEH_SPACE_DOCK_APPROACH_FLAG_OFF    0x17B4u // uint8_t orbital docking approach flag (cleared by 0x693D40 at 0x693E1F)
#endif
#ifndef VEH_ROCKET_PROGRESS_OFFSET
#define VEH_ROCKET_PROGRESS_OFFSET          0x1A68u // float rocket flight progress (0.0f..1.0f, 0.999f on deorbit/abort)
#endif
#ifndef VEH_ROCKET_FLIGHT_STATE_OFFSET
#define VEH_ROCKET_FLIGHT_STATE_OFFSET      0x1A6Cu // int32_t HorseFast (0=ground, 1=launch/orbit, 2=reentry, 10/11=abort)
#endif
#ifndef VEH_TYPE_ROCKET_FLAG_OFFSET
#define VEH_TYPE_ROCKET_FLAG_OFFSET         0x0294u // uint8_t $SPACE_ROCKET (+0x295 $SPACE_ROCKET_BIG)
#endif
#ifndef VEH_TYPE_ROCKETPART_FLAG_OFFSET
#define VEH_TYPE_ROCKETPART_FLAG_OFFSET     0x0296u // uint8_t $SPACE_ROCKETPART (+0x297 $SPACE_ROCKETPART_BIG)
#endif
#ifndef VEH_TYPE_SPACECRAFT_FLAG_OFFSET
#define VEH_TYPE_SPACECRAFT_FLAG_OFFSET     0x0298u // uint8_t $SPACE_SHIP (+0x299 $SPACE_SHIP_BIG)
#endif
#ifndef VEH_TYPE_SPACE_REENTRY_FLAG_OFFSET
#define VEH_TYPE_SPACE_REENTRY_FLAG_OFFSET  0x029Au // uint8_t $SPACE_SHIP_REENTRY (also sets +0x298)
#endif
#ifndef VEH_TYPE_SPACE_STAGES_BEGIN_OFFSET
#define VEH_TYPE_SPACE_STAGES_BEGIN_OFFSET  0x9F60u // std::vector<RocketStage> (+0x9F60 begin, +0x9F68 end, stride 0x2A8, stage vt at +0x100)
#endif
#ifndef VEH_TYPE_SPACE_STAGE_STRIDE
#define VEH_TYPE_SPACE_STAGE_STRIDE         0x02A8u
#endif
#ifndef VEH_TYPE_SPACE_STAGE_VT_OFF
#define VEH_TYPE_SPACE_STAGE_VT_OFF         0x0100u
#endif
#ifndef VEH_TYPE_BUILT_IN_PROD_OFFSET
#define VEH_TYPE_BUILT_IN_PROD_OFFSET       0x9CE4u // int32_t $VEHICLE_BUILT_IN_PRODUCTION_LINE
#endif
#ifndef VEH_TYPE_SPACE_ORBIT_OFFSET
#define VEH_TYPE_SPACE_ORBIT_OFFSET         0x9CE8u // int32_t $VEHICLE_SPACE_REACHED_ORBIT
#endif
#ifndef VEH_TYPE_SPACE_REENTRY_OFFSET
#define VEH_TYPE_SPACE_REENTRY_OFFSET       0x9CECu // int32_t $VEHICLE_SPACE_REENTRY_LANDED
#endif
#ifndef RVA_ORBIT_SIM_STEP
#define RVA_ORBIT_SIM_STEP                  0x41D240u
#endif
#ifndef RVA_SAVE_ORBITS
#define RVA_SAVE_ORBITS                     0x41DC60u
#endif
#ifndef RVA_LOAD_ORBITS
#define RVA_LOAD_ORBITS                     0x41DD60u
#endif
#ifndef RVA_SPACE_MC_RAND1_RET
#define RVA_SPACE_MC_RAND1_RET              0x791F28u // 0x791F22 + 6 crt!rand in Mission Control UI
#endif
#ifndef RVA_SPACE_MC_RAND2_RET
#define RVA_SPACE_MC_RAND2_RET              0x791F4Du // 0x791F47 + 6 crt!rand in Mission Control UI
#endif
#ifndef RVA_SPACE_EXHAUST_RAND_RET
#define RVA_SPACE_EXHAUST_RAND_RET          0x698098u // 0x698092 + 6 C3DRandom_Int in rocket exhaust particles
#endif
#ifndef RVA_SPACE_SOUND_RAND1_RET
#define RVA_SPACE_SOUND_RAND1_RET           0x6901E5u // 0x6901DF + 6 C3DRandom_Float in rocket 3D sound playback
#endif
#ifndef RVA_SPACE_SOUND_RAND2_RET
#define RVA_SPACE_SOUND_RAND2_RET           0x691C5Du // 0x691C57 + 6 C3DRandom_Float in rocket 3D sound playback
#endif
#ifndef RVA_SPACE_SOUND_RAND3_RET
#define RVA_SPACE_SOUND_RAND3_RET           0x695EAEu // 0x695EA8 + 6 C3DRandom_Float in rocket 3D sound playback
#endif

// Space Program (dlc5) Launch Pad orbit config & vehicle dynamic exclusions (v1.2.0.9)
#ifndef BLD_CAT_POLLUTION_MONITOR
#define BLD_CAT_POLLUTION_MONITOR           0x24u     // Pollution monitoring station (uses +0x0E3C for alert threshold)
#endif
#ifndef BLD_CAT_SPACE_LAUNCH_PAD
#define BLD_CAT_SPACE_LAUNCH_PAD            0x2Fu     // Launch Pad category at BUILDING::TYPE+0x368 ($TYPE_AIRPLANE_PARKING)
#endif
#ifndef BLD_TYPE_SUBTYPE_OFF
#define BLD_TYPE_SUBTYPE_OFF                0x036Cu   // int32_t BUILDING::TYPE subtype (0x2D small pad, 0x30 Proton pad in 0x78EA94)
#endif
#ifndef BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL
#define BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL  0x2Du     // Small rocket launch pad subtype at BUILDING::TYPE+0x36C
#endif
#ifndef BLD_SUBTYPE_SPACE_LAUNCH_PAD_BIG
#define BLD_SUBTYPE_SPACE_LAUNCH_PAD_BIG    0x30u     // Heavy (Proton) rocket launch pad subtype at BUILDING::TYPE+0x36C
#endif
#ifndef BLD_LAUNCH_PAD_ORBIT_TYPE_OFF
#define BLD_LAUNCH_PAD_ORBIT_TYPE_OFF       0x0CD8u   // int32_t orbit type/direction on Launch Pad (overlaps BLD_CITIZEN_FILTER_AT)
#endif
#ifndef BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF
#define BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF 0x0E34u // 4 floats: 0xE34, 0xE38, 0xE3C, 0xE40 (orbit inclination/phase/apoapsis/periapsis)
#endif
#ifndef BLD_LAUNCH_PAD_ORBIT_PARAMS_END_OFF
#define BLD_LAUNCH_PAD_ORBIT_PARAMS_END_OFF 0x0E44u
#endif
#ifndef VEH_SPACE_DYN_664_BEGIN_OFF
#define VEH_SPACE_DYN_664_BEGIN_OFF         0x0664u   // float orbital dwell timer (increments in orbit; Reentry unlocks at >= 60.0f)
#endif
#ifndef VEH_SPACE_DYN_664_END_OFF
#define VEH_SPACE_DYN_664_END_OFF           0x0668u
#endif
#ifndef VEH_SPACE_ORBIT_DYNAMIC_BEGIN_OFF
#define VEH_SPACE_ORBIT_DYNAMIC_BEGIN_OFF   0x175Cu   // +0x175C..+0x1770 orbitIdx, targetOrbitIdx, dockPartner ptr, dockSlot
#endif
#ifndef VEH_SPACE_ORBIT_DYNAMIC_END_OFF
#define VEH_SPACE_ORBIT_DYNAMIC_END_OFF     0x1774u
#endif
#ifndef VEH_SPACE_DYN_1774_BEGIN_OFF
#define VEH_SPACE_DYN_1774_BEGIN_OFF        0x1774u   // +0x1774..+0x17B8 dynamic orbital docking / approach kinematics
#endif
#ifndef VEH_SPACE_DYN_1774_END_OFF
#define VEH_SPACE_DYN_1774_END_OFF          0x17B8u
#endif
#ifndef VEH_SPACE_DYN_1860_BEGIN_OFF
#define VEH_SPACE_DYN_1860_BEGIN_OFF        0x1860u   // +0x1860..+0x1868 dynamic space propulsion/state
#endif
#ifndef VEH_SPACE_DYN_1860_END_OFF
#define VEH_SPACE_DYN_1860_END_OFF          0x1868u
#endif
#ifndef VEH_SPACE_DYN_1938_BEGIN_OFF
#define VEH_SPACE_DYN_1938_BEGIN_OFF        0x1938u   // +0x1938..+0x1940 dynamic space telemetry/render state
#endif
#ifndef VEH_SPACE_DYN_1938_END_OFF
#define VEH_SPACE_DYN_1938_END_OFF          0x1940u
#endif
#ifndef VEH_SPACE_STAGE_DYNAMIC_BEGIN_OFF
#define VEH_SPACE_STAGE_DYNAMIC_BEGIN_OFF   0x1AD8u   // +0x1AD8/+0x1AF0 stage vectors, +0x1B08 counters, +0x1B10 exhaust timer
#endif
#ifndef VEH_SPACE_STAGE_DYNAMIC_END_OFF
#define VEH_SPACE_STAGE_DYNAMIC_END_OFF     0x1B18u
#endif
#ifndef VEH_SPACE_DYN_1B50_BEGIN_OFF
#define VEH_SPACE_DYN_1B50_BEGIN_OFF        0x1B50u   // +0x1B50..+0x1B78 dynamic space reentry/parachute/stage timers
#endif
#ifndef VEH_SPACE_DYN_1B50_END_OFF
#define VEH_SPACE_DYN_1B50_END_OFF          0x1B78u
#endif
#ifndef VEH_SPACE_TIMER_DYNAMIC_BEGIN_OFF
#define VEH_SPACE_TIMER_DYNAMIC_BEGIN_OFF   0x1B78u   // rocket failure / burnup / stage simulation timers
#endif
#ifndef VEH_SPACE_TIMER_DYNAMIC_END_OFF
#define VEH_SPACE_TIMER_DYNAMIC_END_OFF     0x1B98u
#endif

// Space Program (dlc5) Mutator routines & UI call/patch sites (v1.2.0.9)
#ifndef SITE_TOOL_ORBITAL_ATTACH_RVA
#define SITE_TOOL_ORBITAL_ATTACH_RVA        0x332D05u // call 0x2D2B30 in Late dispatch for "orbital_attach"
#endif
#ifndef FN_TOOL_ORBITAL_ATTACH_RVA
#define FN_TOOL_ORBITAL_ATTACH_RVA          0x2D2B30u // orbital_attach tool handler
#endif
#ifndef FN_SPACE_LAUNCH_ROCKET_RVA
#define FN_SPACE_LAUNCH_ROCKET_RVA          0x698720u // void __fastcall(Vehicle* rcx, int targetOrbitIdx edx)
#endif
#ifndef FN_SPACE_ORBIT_ATTACH_RVA
#define FN_SPACE_ORBIT_ATTACH_RVA           0x693B00u // void __fastcall(Vehicle* spacecraft rcx, Vehicle* station rdx, int dockSlot r8d)
#endif
#ifndef FN_SPACE_ORBIT_DETACH_RVA
#define FN_SPACE_ORBIT_DETACH_RVA           0x693D40u // void __fastcall(Vehicle* spacecraft rcx)
#endif
#ifndef FN_SPACE_HAS_REENTRY_RVA
#define FN_SPACE_HAS_REENTRY_RVA            0x694920u // bool __fastcall(Vehicle* rcx): true if another vehicle shares orbitIdx
#endif
#ifndef FN_SPACE_SETUP_REENTRY_RVA
#define FN_SPACE_SETUP_REENTRY_RVA          0x694710u // void __fastcall(Vehicle* rcx)
#endif
#ifndef FN_SPACE_SETUP_DEORBIT_RVA
#define FN_SPACE_SETUP_DEORBIT_RVA          0x694310u // void __fastcall(Vehicle* rcx)
#endif
#ifndef SITE_SPACE_LAUNCH_NEW_ORBIT_RVA
#define SITE_SPACE_LAUNCH_NEW_ORBIT_RVA     0x78FEEBu // call 0x698720 (Launch Pad new/configured orbit)
#endif
#ifndef SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA
#define SITE_SPACE_LAUNCH_CATCH_ORBIT_RVA   0x79031Au // call 0x698720 (Launch Pad catch/existing orbit)
#endif
#ifndef SITE_SPACE_ORBIT_ATTACH_RVA
#define SITE_SPACE_ORBIT_ATTACH_RVA         0x2D31DFu // call 0x693B00 (orbital_attach tool)
#endif
#ifndef SITE_SPACE_ORBIT_DETACH_RVA
#define SITE_SPACE_ORBIT_DETACH_RVA         0x817674u // call 0x693D40 (Vehicle window undock button)
#endif
#ifndef SITE_SPACE_REENTRY_PATCH_RVA
#define SITE_SPACE_REENTRY_PATCH_RVA        0x819293u // 22B pre-mutation block in Vehicle window button 0x0A
#endif
#ifndef SITE_SPACE_DEORBIT_ABORT_PATCH_RVA
#define SITE_SPACE_DEORBIT_ABORT_PATCH_RVA  0x819544u // 22B entry of Deorbit/Abort block in Vehicle window button 0x0D
#endif
#ifndef SITE_SPACE_VEH_WIN_EXIT_RVA
#define SITE_SPACE_VEH_WIN_EXIT_RVA         0x819815u // Vehicle window switch-case break target
#endif

// Space Program (dlc5) Phase 4 Two-States Space Race RVAs & offsets (v1.2.0.9)
#ifndef WORLD_SPACE_CARGO_WEST_UNLOCK_OFF
#define WORLD_SPACE_CARGO_WEST_UNLOCK_OFF   0x11BA8u  // uint8_t written by 0x314920 (spacecargo_west)
#endif
#ifndef WORLD_SPACE_CARGO_EAST_UNLOCK_OFF
#define WORLD_SPACE_CARGO_EAST_UNLOCK_OFF   0x11BA9u  // uint8_t written by 0x314920 (spacecargo_east)
#endif
#ifndef WORLD_MISSION_CONTROL_BLD_PTR_OFF
#define WORLD_MISSION_CONTROL_BLD_PTR_OFF   0x11BB0u  // Building* global cached Mission Control (0x1BA1EE / 0x17ECD0)
#endif
#ifndef BLD_CAT_MISSION_CONTROL
#define BLD_CAT_MISSION_CONTROL             0x30u
#endif
#ifndef BLD_SUBTYPE_MISSION_CONTROL
#define BLD_SUBTYPE_MISSION_CONTROL         0x2Du
#endif
#ifndef STAT_VEHICLES_EXPORT_USD_OFF
#define STAT_VEHICLES_EXPORT_USD_OFF        0x190u    // float $Vehicles_ExportUSD inside $STAT_CURRENT (WORLD+0x11558)
#endif
#ifndef STAT_VEHICLES_EXPORT_RUB_OFF
#define STAT_VEHICLES_EXPORT_RUB_OFF        0x194u    // float $Vehicles_ExportRUB inside $STAT_CURRENT (WORLD+0x1155C)
#endif
#ifndef WINDOW_POPUP_TYPE_SPACE_PAYOUT
#define WINDOW_POPUP_TYPE_SPACE_PAYOUT      0x27      // 0x7242F0(WORLD, kind=4, 0, subkind=4, 0, 0x27, stringId=0, 0, 0)
#endif
#ifndef RVA_PROD_LINE_STEP
#define RVA_PROD_LINE_STEP                  0x1E42C0u // void __fastcall(BYTE* world, BYTE* building) (cat 0x28 rocket/veh factories)
#endif
#ifndef RVA_CREATE_VEHICLE
#define RVA_CREATE_VEHICLE                  0x403E60u // BYTE* __fastcall(WORLD*, VT*, BLD*, ...) (arg7 byte = factoryBuilt -> +0x9CE4)
#endif
#ifndef RVA_SPACE_VEH_ORBIT_STEP
#define RVA_SPACE_VEH_ORBIT_STEP            0x695AC0u // void __fastcall(BYTE* veh) (+0x9CE8, commercial export payout, split orbit)
#endif
#ifndef RVA_SPACE_VEH_REENTRY_STEP
#define RVA_SPACE_VEH_REENTRY_STEP          0x692790u // void __fastcall(BYTE* veh) (+0x9CEC landing milestone)
#endif
#ifndef RVA_REMOVE_ORBIT
#define RVA_REMOVE_ORBIT                    0x41DAB0u // void __fastcall(BYTE* world, int orbitIdx) (all 6 orbit erase callers)
#endif
#ifndef RVA_SPAWN_SPACE_CARGO_AT_BORDER
#define RVA_SPAWN_SPACE_CARGO_AT_BORDER     0x314A60u // void __fastcall(BYTE* world) (competitive space_cargo_* border spawn)
#endif
#ifndef RVA_MISSION_CONTROL_WINDOW
#define RVA_MISSION_CONTROL_WINDOW          0x78DB70u // void __fastcall(BYTE* world, BYTE* window) (Mission Control UI)
#endif
#ifndef RVA_TOP_BAR_HUD_DRAW
#define RVA_TOP_BAR_HUD_DRAW                0x094020u // void __fastcall(BYTE* world) (top HUD space shortcut -> [WORLD+0x11BB0])
#endif

static_assert(WORLD_ORBIT_CLOCK_OFFSET == 0x18u &&
              WORLD_ORBITS_VEC_OFFSET == 0x20u &&
              ORBIT_SLOT_STRIDE == 0x60u &&
              ORBIT_SLOT_TYPE_OFFSET + 4u <= ORBIT_SLOT_STRIDE &&
              ORBIT_SLOT_START_CLOCK_OFFSET + 4u <= ORBIT_SLOT_STRIDE &&
              VEH_TARGET_ORBIT_INDEX_OFFSET == VEH_ORBIT_INDEX_OFFSET + 4u &&
              VEH_SPACE_DOCK_PARTNER_OFFSET == VEH_ORBIT_INDEX_OFFSET + 12u &&
              VEH_SPACE_DOCK_SLOT_OFFSET == VEH_SPACE_DOCK_PARTNER_OFFSET + 8u &&
              VEH_ROCKET_FLIGHT_STATE_OFFSET == VEH_ROCKET_PROGRESS_OFFSET + 4u &&
              VEH_TYPE_ROCKET_FLAG_OFFSET == 0x0294u &&
              VEH_TYPE_ROCKETPART_FLAG_OFFSET == 0x0296u &&
              VEH_TYPE_SPACECRAFT_FLAG_OFFSET == 0x0298u &&
              VEH_TYPE_SPACE_REENTRY_FLAG_OFFSET == 0x029Au &&
              VEH_TYPE_SPACE_ORBIT_OFFSET == VEH_TYPE_BUILT_IN_PROD_OFFSET + 4u &&
              VEH_TYPE_SPACE_REENTRY_OFFSET == VEH_TYPE_SPACE_ORBIT_OFFSET + 4u &&
              RVA_SPACE_MC_RAND1_RET < RVA_SPACE_MC_RAND2_RET &&
              RVA_SPACE_SOUND_RAND1_RET < RVA_SPACE_SOUND_RAND2_RET &&
              RVA_SPACE_SOUND_RAND2_RET < RVA_SPACE_SOUND_RAND3_RET &&
              BLD_CAT_SPACE_LAUNCH_PAD == 0x2Fu &&
              BLD_TYPE_SUBTYPE_OFF == 0x036Cu &&
              BLD_SUBTYPE_SPACE_LAUNCH_PAD_SMALL == 0x2Du &&
              BLD_SUBTYPE_SPACE_LAUNCH_PAD_BIG == 0x30u &&
              BLD_LAUNCH_PAD_ORBIT_PARAMS_END_OFF == BLD_LAUNCH_PAD_ORBIT_PARAMS_BEGIN_OFF + 16u &&
              VEH_SPACE_ORBIT_DYNAMIC_BEGIN_OFF == VEH_ORBIT_INDEX_OFFSET &&
              VEH_SPACE_ORBIT_DYNAMIC_END_OFF == VEH_SPACE_DOCK_SLOT_OFFSET + 4u &&
              VEH_SPACE_DYN_1774_BEGIN_OFF == VEH_SPACE_ORBIT_DYNAMIC_END_OFF &&
              VEH_SPACE_DOCK_APPROACH_FLAG_OFF >= VEH_SPACE_DYN_1774_BEGIN_OFF &&
              VEH_SPACE_DOCK_APPROACH_FLAG_OFF < VEH_SPACE_DYN_1774_END_OFF &&
              VEH_SPACE_STAGE_DYNAMIC_BEGIN_OFF < VEH_SPACE_STAGE_DYNAMIC_END_OFF &&
              VEH_SPACE_DYN_1B50_END_OFF == VEH_SPACE_TIMER_DYNAMIC_BEGIN_OFF &&
              VEH_SPACE_TIMER_DYNAMIC_BEGIN_OFF < VEH_SPACE_TIMER_DYNAMIC_END_OFF &&
              SITE_SPACE_REENTRY_PATCH_RVA < SITE_SPACE_DEORBIT_ABORT_PATCH_RVA &&
              SITE_SPACE_DEORBIT_ABORT_PATCH_RVA < SITE_SPACE_VEH_WIN_EXIT_RVA &&
              RESEARCH_ITEM_REQ_WORK_OFFSET == 0x5Cu &&
              CIT_RIDING_VEHICLE_PTR_OFF == 0x060 &&
              WORLD_SPACE_CARGO_EAST_UNLOCK_OFF == WORLD_SPACE_CARGO_WEST_UNLOCK_OFF + 1u &&
              WORLD_MISSION_CONTROL_BLD_PTR_OFF == 0x11BB0u &&
              STAT_VEHICLES_EXPORT_RUB_OFF == STAT_VEHICLES_EXPORT_USD_OFF + 4u &&
              RVA_TOP_BAR_HUD_DRAW < RVA_PROD_LINE_STEP &&
              RVA_PROD_LINE_STEP < RVA_SPAWN_SPACE_CARGO_AT_BORDER &&
              RVA_SPAWN_SPACE_CARGO_AT_BORDER < RVA_CREATE_VEHICLE &&
              RVA_CREATE_VEHICLE < RVA_REMOVE_ORBIT &&
              RVA_REMOVE_ORBIT < RVA_SPACE_VEH_REENTRY_STEP &&
              RVA_SPACE_VEH_REENTRY_STEP < RVA_SPACE_VEH_ORBIT_STEP &&
              RVA_SPACE_VEH_ORBIT_STEP < RVA_MISSION_CONTROL_WINDOW,
              "Space Program (dlc5) v1.2.0.9 RVAs and offsets must remain consistent");

static_assert(FN_UI_PLAY_SOUND_RVA == 0x422E60u &&
              RVA_HAS_PLANE_PROPS == 0x0319A0u &&
              RVA_FIND_TAG_ID == 0x277950u &&
              VEH_TYPE_ELECTRIC_OFFSET == 0x86D0u &&
              VEH_TYPE_PROP_VEC_OFFSET == 0x7B08u &&
              VEH_TYPE_TAGS_OFFSET == 0x9708u &&
              WORLD_TAGS_BEGIN_OFFSET == 0x128A0u &&
              WORLD_TAG_ENTRY_STRIDE == 0x44u &&
              IMPORT_HAS_SOUND_RVA == 0x8B1D58 &&
              IMPORT_GET_SOUND_RVA == 0x8B1D60 &&
              SOUND_ENGINE_PTR_RVA == 0xA2ECC8 &&
              SOUND_MGR_RVA == 0xA2ECD0 &&
              CIT_CUR_BLD_OFFSET == 0x58 &&
              CIT_DEST_BLD_OFFSET == 0x690 &&
              CIT_DORM_FLAG_OFFSET == 0x2D,
              "Purchase sound & citizen desync v1.2.0.9 RVAs/offsets must match SOVIET64.exe");





