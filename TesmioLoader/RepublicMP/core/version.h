// SPDX-License-Identifier: GPL-3.0-only
// RepublicMP version and wire-protocol number.
//
// REPUBLICMP_VERSION: MAJOR.FEATURE.PATCH (see README.md's Versioning section).
// REPUBLICMP_PROTOCOL: bump whenever any network message layout changes.
//
// When a player joins, both sides exchange the version, the protocol and a
// CRC32 of their own RepublicMP.dll file (session/save_sync); any difference
// refuses the join, so two different builds never exchange messages.
#pragma once

#ifndef REPUBLICMP_VERSION
#define REPUBLICMP_VERSION "1.2.4"
#endif

#define REPUBLICMP_WIDE2(x) L##x
#define REPUBLICMP_WIDE(x) REPUBLICMP_WIDE2(x)
#define REPUBLICMP_VERSION_W REPUBLICMP_WIDE(REPUBLICMP_VERSION)

#define REPUBLICMP_PROTOCOL 47

#ifndef REPUBLICMP_GITHUB_REPO
#define REPUBLICMP_GITHUB_REPO "WinLen/SovietRepublic-MP"
#endif
