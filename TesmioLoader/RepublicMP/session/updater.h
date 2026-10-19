// SPDX-License-Identifier: GPL-3.0-only
// Auto-updater for RepublicMP via GitHub Releases & Steam Workshop (AppID 784150).
#pragma once

#include "tesmio_plugin.h"

enum UpdateStatus {
    UPDATE_IDLE = 0,
    UPDATE_CHECKING,
    UPDATE_UP_TO_DATE,
    UPDATE_AVAILABLE_GITHUB,
    UPDATE_AVAILABLE_WORKSHOP,
    UPDATE_DOWNLOADING,
    UPDATE_APPLIED_RESTART_NEEDED,
    UPDATE_ERROR
};

void Updater_Bind(const TsmHost* host);
void Updater_Init(void);

// Triggers an asynchronous check against GitHub Releases + local Steam Workshop content.
void Updater_CheckNow(void);

// Triggers asynchronous download and in-place atomic replacement of RepublicMP.dll.
void Updater_DownloadAndApply(void);

UpdateStatus Updater_GetStatus(void);
const char* Updater_GetLatestVersion(void);
const char* Updater_GetStatusMessage(void);
const char* Updater_GetGithubRepo(void);
void Updater_SetGithubRepo(const char* repo);
