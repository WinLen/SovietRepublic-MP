// SPDX-License-Identifier: GPL-3.0-only
// Default host IP for the Connect field. Taken from the optional, gitignored
// mp_config.h (see mp_config.h.example); empty when that file is absent - the
// IP can always be typed in the main window.
#pragma once

#if __has_include("../mp_config.h")
#include "../mp_config.h"
#endif

#ifndef REPUBLICMP_HOST_IP
#define REPUBLICMP_HOST_IP ""
#endif
