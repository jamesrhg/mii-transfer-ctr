#pragma once

#include <string>
#include <vector>

#include "ver3_mii.h"

// Reads this console's Mii Maker library (CFL_DB.dat) - the 3DS equivalent
// of the Wii U build's FFL_ODB.dat-based "Library" tab. CFL_DB.dat lives in
// the Mii Maker applet's *shared* extdata (archive 0x00000007
// ARCHIVE_SHARED_EXTDATA, extdata ID 0xf000000b) rather than under any
// particular title's own save data - the same location and file name
// SpecializeMii (github.com/phijor/SpecializeMii) reads/writes for the exact
// same reason (it's a shared, system-wide Mii library, not per-title data).
//
// Unlike the Wii U path, CFL_DB.dat's 92-byte records need no endian swap:
// they're already the native little-endian Ver3 layout every other source in
// this app uses (see ver3_mii.h) - the swap in the original code was only
// ever needed for the Cafe (Wii U) database's own on-disk byte order.
namespace CtrMiiDb {

struct LoadResult {
    bool success = false;
    bool crcValid = false;
    std::string error;
    std::vector<Ver3MiiDecoded> miis;
    // Decoded from the same file's embedded "CFRA" section (see Load()'s
    // own comment) - Miis recently picked/used in *other* titles (e.g.
    // choosing a Mii for a friend request or as an NPC in-game), not
    // necessarily anyone in this console's own Library. Each record is
    // Nintendo's more compact 72-byte "Core" Mii format rather than the
    // 92-byte one CFL_DB.dat's main array uses - it has no creator-name
    // field at all (zero-padded out to the usual 96-byte StoreData shape
    // here, same as every other Ver3MiiDecoded in this app, so
    // GetVer3MiiCreatorName() on these always comes back empty - that's
    // expected, not a bug).
    std::vector<Ver3MiiDecoded> recentMiis;
    // The whole file's raw bytes, kept around only so MiiHttpServer can
    // serve /data/CFL_DB.dat from memory - unlike the Wii U build's
    // FFL_ODB.dat (a plain SD file MiiHttpServer could just fopen() fresh
    // per request), CFL_DB.dat lives inside a shared-extdata archive with no
    // equivalent plain path, so it's read once here and handed off as a
    // blob instead (same shape as SetActMiiData()/SetFriendMiiData()).
    std::vector<uint8_t> rawBytes;
};

// Opens the shared extdata archive, reads /CFL_DB.dat in full, and decodes
// every non-empty slot (of up to 100 - CFL_DB.dat's own capacity) into a
// Ver3MiiDecoded, plus every non-empty slot of the file's separate "recent
// Miis" section (see LoadResult::recentMiis's own comment - confirmed via
// 3dbrew's own CFL_DB.dat layout documentation: a "CFRA" section embedded
// in the same file, not a separate one, at a fixed offset). Must be called
// after fsInit(). Synchronous - the whole file is small (310560 bytes) and
// local, so this is cheap.
LoadResult Load();

} // namespace CtrMiiDb
