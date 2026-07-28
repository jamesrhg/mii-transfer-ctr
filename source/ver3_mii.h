#pragma once

#include <array>
#include <cstdint>
#include <string>

/// Size of 3DS/Wii U Mii data, aka "Ver3StoreData".
constexpr unsigned int VER3_STORE_DATA_SIZE = 96;
/// Size of 3DS/Wii U Mii data within the database.
constexpr unsigned int VER3_DATA_SIZE = 92;
constexpr unsigned int VER3_CREATE_ID_SIZE = 10;

// Nickname plus a ready-to-send Ver3StoreData blob for one Mii - the common
// currency between all three Mii sources this app has (CFL_DB.dat via
// ctr_mii_db.cpp, ACT via ctr_account.cpp, FRD via ctr_friends.cpp), and
// what MiiImageFetcher/the HTTP server's fp_db.dat endpoint consumes. Named
// for the format itself (Ver3StoreData, per
// source/lib/DbAndCreateId.hpp) rather than either platform - it's shared
// byte-for-byte between the Wii U's FFLStoreData and the 3DS's CFLStoreData
// (see 3ds/mii.h).
struct Ver3MiiDecoded {
    // Little-endian Ver3StoreData (96 bytes) with a valid CRC-16: the raw
    // 92-byte record plus 2 zero padding bytes and a 2-byte checksum. This
    // is the format Mii face-render services (e.g. mii-unsecure.ariankordi.net)
    // expect as their base64 "data" query parameter.
    std::array<uint8_t, VER3_STORE_DATA_SIZE> storeData{};
    std::string nickname;
    std::string creatorName;
};

// Decodes a Mii already in finished StoreData form - what ACT's
// ACT_GetAccountInfo(INFO_TYPE_MII) and FRD's FRD_GetFriendMii() hand back
// directly, unlike CFL_DB.dat's raw 92-byte records (which need zero-padding
// and a fresh CRC; see ctr_mii_db.cpp).
Ver3MiiDecoded DecodeVer3MiiFromStoreData(
    const std::array<uint8_t, VER3_STORE_DATA_SIZE> &storeData);

std::string GetVer3MiiNickname(const std::array<uint8_t, VER3_STORE_DATA_SIZE>& storeData);
std::string GetVer3MiiCreatorName(const std::array<uint8_t, VER3_STORE_DATA_SIZE>& storeData);

// Everything shown on the Mii detail panel, beyond the name/creator name
// already in Ver3MiiDecoded - all derived from the CreateID embedded in
// the Mii's own data (see source/lib/DbAndCreateId.hpp), not looked up
// externally.
struct MiiDetails {
    // "YYYY-MM-DD HH:MM UTC", or "Unknown" (DS-origin Miis don't encode a
    // recoverable creation date in this ID format).
    std::string createdDate;
    // Where the Mii was made, in one field: the platform (from the
    // CreateID's own platform bits), plus any transfer history
    // (Ver3DataHeuristic::guessOriginVer3, e.g. "Wii, transferred to Wii
    // U", "Wii U, exported to 3DS/QR code") or ", Camera" suffix
    // (isMiiVersionForWiiuCamera) when applicable - e.g. "Wii U", "3DS",
    // "Wii U, Camera", "Wii, transferred to Wii U".
    std::string madeOn;
    // The Mii's own 10-byte Ver3 CreateID (MiiExtraInfo::createId,
    // ver3CreateIdLength bytes of it - the same field madeOn/createdDate
    // above are themselves derived from), hex-formatted. This is a CreateID,
    // not a MAC address - do not label or treat it as one.
    std::string createIdHex;
    bool copyingEnabled = false;
    // Both the "local only" and "NG word" (blocked name) flags gate
    // whether this Mii can be shared/used online.
    bool sharingAllowed = false;
    bool isSpecial = false;
    bool isFavorite = false;
    // "Male" / "Female".
    std::string gender;
    // "March 14", or "Not set" (birthdays are optional in Mii Maker).
    std::string birthday;
};

// Re-decodes `mii.storeData` to compute the above - cheap, meant to be
// called on demand (e.g. when the user focuses a Mii in the list), not
// stored per-Mii for the whole list.
MiiDetails ComputeMiiDetails(const Ver3MiiDecoded &mii);
