// SPDX-License-Identifier: GPL-3.0-only
// See money.h.
#include "money.h"
#include "two_states.h"
#include "../ui/sync_block.h"
#include "../world_sync/buildings/build_sync.h"
#include <math.h>

#define MONEY_SYNC_INTERVAL_MS 1000

void Money_Bind(const TsmHost* host)
{
    TsmBind(host);
}

static double* MoneyField(int offset)
{
    return (double*)(g_exeBase + MONEY_THIS_RVA + offset);
}

static ULONGLONG g_lastSendMs = 0;
static bool g_wasSyncing = false;
static bool g_appliedOnce = false; // client: logged the first take-over of this sync

void Money_SyncTick()
{
    if (TwoStates_IsActive()) return;
    bool syncing = BuildSync_IsConnected() && !SyncBlock_IsActive();
    if (!syncing) {
        g_wasSyncing = false;
        g_appliedOnce = false;
        return;
    }
    if (!BuildSync_IsHostRole()) return;

    ULONGLONG now = GetTickCount64();
    if (g_wasSyncing && now - g_lastSendMs < MONEY_SYNC_INTERVAL_MS) return;

    double rub = 0.0, usd = 0.0;
    __try {
        rub = *MoneyField(MONEY_RUB_OFFSET);
        usd = *MoneyField(MONEY_USD_OFFSET);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!g_wasSyncing)
        Logf("republicmp  MONEY: sync started - sending host balances rub=%.0f usd=%.0f", rub, usd);
    g_wasSyncing = true;
    g_lastSendMs = now;
    BuildSync_SendMoney(rub, usd);
}

void Money_OnRemote(double rub, double usd)
{
    if (TwoStates_IsActive()) return;
    if (SyncBlock_IsActive() || BuildSync_IsHostRole()) return;
    if (!isfinite(rub) || !isfinite(usd)) return;

    __try {
        double* localRub = MoneyField(MONEY_RUB_OFFSET);
        double* localUsd = MoneyField(MONEY_USD_OFFSET);
        if (!g_appliedOnce) {
            Logf("republicmp  MONEY: following the host - rub %.0f -> %.0f, usd %.0f -> %.0f",
                 *localRub, rub, *localUsd, usd);
            g_appliedOnce = true;
        }
        *localRub = rub;
        *localUsd = usd;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
