#include "ctr_mii_db.h"

#include "lib/DbAndCreateId.hpp"

#include <3ds.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace CtrMiiDb {

namespace {

constexpr u32 kSharedExtdataId = 0xf000000b;
constexpr size_t kDbExpectedFileSize = 310560; // CharDatabaseCtr::setDesc()'s fileSize.

// CFL_DB.dat's embedded "CFRA" ("recent Miis") section - confirmed via
// 3dbrew's own CFL_DB.dat layout documentation (not guessed): CFRA starts
// at file offset 0xC820 ("CFRA" magic + a u32 count + a 100-byte order-index
// array), and its 100-entry Mii array starts right after that at 0xC88C,
// each entry 0x48 (72) bytes - Nintendo's more compact "Core" Mii format
// (no creator-name field), matching DbAndCreateId.hpp's own
// CharDataTypeForId::ver3Core classification for 72-byte data. Reusing
// CharDatabaseAccessor for this (rather than a bespoke loop) gets its
// isEmpty()/getCharDataOffset() for free - same idea as CharDatabaseCtr's
// own setDesc(), just built here directly since this section doesn't have
// a matching preset in the generated library.
constexpr int kCfraCharDataOffset = 0xC88C;
constexpr int kCfraCharDataSize = 72;
constexpr int kCfraCharDataNum = 100;

// Reads /CFL_DB.dat's raw bytes out of the Mii Maker applet's shared
// extdata. Returns false (and fills *outError) on any failure - a missing
// Mii Maker library (never opened Mii Maker) or FS error is expected/benign,
// not a crash.
bool ReadCflDbFile(std::vector<uint8_t> *outData, std::string *outError) {
    u32 archivePath[3] = {MEDIATYPE_NAND, kSharedExtdataId, 0x00048000};
    FS_Path fsArchivePath = {PATH_BINARY, sizeof(archivePath), archivePath};

    FS_Archive archive;
    Result res = FSUSER_OpenArchive(&archive, ARCHIVE_SHARED_EXTDATA, fsArchivePath);
    if (R_FAILED(res)) {
        if (outError) *outError = std::format("Could not open the shared Mii Maker extdata (0x{:08X}).", static_cast<unsigned>(res));
        return false;
    }

    FS_Path filePath = fsMakePath(PATH_UTF16, u"/CFL_DB.dat");
    Handle file;
    res = FSUSER_OpenFile(&file, archive, filePath, FS_OPEN_READ, 0);
    if (R_FAILED(res)) {
        FSUSER_CloseArchive(archive);
        if (outError) *outError = std::format("Could not open CFL_DB.dat (0x{:08X}). Has Mii Maker ever been opened?", static_cast<unsigned>(res));
        return false;
    }

    outData->resize(kDbExpectedFileSize);
    u32 bytesRead = 0;
    res = FSFILE_Read(file, &bytesRead, 0, outData->data(), static_cast<u32>(outData->size()));
    FSFILE_Close(file);
    FSUSER_CloseArchive(archive);

    if (R_FAILED(res) || bytesRead != kDbExpectedFileSize) {
        if (outError) {
            *outError = std::format("Unexpected CFL_DB.dat size (read {} of {} bytes).", bytesRead, kDbExpectedFileSize);
        }
        return false;
    }
    return true;
}

// No endian swap needed here - see this file's header comment. Mirrors
// main.cpp's LoadMiiDb() on the Wii U side, minus that step. `srcSize` is
// how many bytes actually exist at `offset` (92 for CFL_DB.dat's main
// array, 72 for the CFRA "recent Miis" section's more compact Core format -
// see kCfraCharDataSize's own comment) - whatever's left up to
// VER3_STORE_DATA_SIZE (96) is zero-padded, same as the Wii U build's own
// 92-to-96 padding, just with more padding for the 72-byte case (which also
// zero-pads out the creator-name field entirely, since Core data doesn't
// have one).
Ver3MiiDecoded DecodeSlot(const uint8_t *dbData, size_t offset, size_t srcSize = VER3_DATA_SIZE) {
    std::array<uint8_t, VER3_STORE_DATA_SIZE> data{}; // zero-initialized: everything past srcSize stays 0 until the CRC is computed below.
    std::copy_n(dbData + offset, srcSize, data.data());
    DbAndCreateId::Crc16Ccitt::updateBigEndian(data.data(), static_cast<int>(data.size()), 0);
    return Ver3MiiDecoded{
        .storeData = data,
        .nickname = GetVer3MiiNickname(data),
        .creatorName = GetVer3MiiCreatorName(data),
    };
}

} // namespace

LoadResult Load() {
    LoadResult result{};

    std::vector<uint8_t> dbData;
    if (!ReadCflDbFile(&dbData, &result.error)) {
        return result;
    }

    DbAndCreateId::CharDatabaseAccessor database{};
    DbAndCreateId::CharDatabaseCtr::setDesc(&database.desc);
    database.data = dbData.data();

    result.crcValid = database.isValid();

    for (int i = 0; i < database.desc.charDataNum; i++) {
        if (database.isEmpty(i)) continue;
        size_t offset = static_cast<size_t>(database.getCharDataOffset(i));
        result.miis.push_back(DecodeSlot(dbData.data(), offset));
    }

    // The CFRA "recent Miis" section - see LoadResult::recentMiis's own
    // comment. Not gated on result.crcValid (that's the main CFOG+CFHE
    // checksum, a different one than CFRA's own - see this file's own
    // comment on kCfraCharDataOffset) - same "read what's there, don't
    // block on a checksum" approach the main array above already takes.
    DbAndCreateId::CharDatabaseAccessor recentDatabase{};
    recentDatabase.desc.fileSize = static_cast<int>(kDbExpectedFileSize);
    recentDatabase.desc.charDataOffset = kCfraCharDataOffset;
    recentDatabase.desc.charDataSize = kCfraCharDataSize;
    recentDatabase.desc.charDataNum = kCfraCharDataNum;
    recentDatabase.data = dbData.data();

    for (int i = 0; i < recentDatabase.desc.charDataNum; i++) {
        if (recentDatabase.isEmpty(i)) continue;
        size_t offset = static_cast<size_t>(recentDatabase.getCharDataOffset(i));
        result.recentMiis.push_back(DecodeSlot(dbData.data(), offset, kCfraCharDataSize));
    }

    result.rawBytes = std::move(dbData);
    result.success = true;
    return result;
}

} // namespace CtrMiiDb
