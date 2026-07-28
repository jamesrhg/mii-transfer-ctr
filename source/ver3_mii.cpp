#include "ver3_mii.h"

#include "lib/DbAndCreateId.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {
    void loadArrayU16LittleEndian(uint8_t const * src, int srcOffset, uint16_t * dst, int dstOffset, int count)
    {
        for (int i = 0; i < count; i++)
            dst[dstOffset + i] = static_cast<uint16_t>((src[srcOffset + i * 2] & 255) | src[srcOffset + i * 2 + 1] << 8);
    }

    int char16ToUtf8(uint8_t * dst, uint16_t const * src, int characterCount, int srcOffset, int dstOffset)
    {
    	int iDst = 0;
    	for (int i = 0; i < characterCount && src[srcOffset + i] != 0; i++) {
    		int chr = src[srcOffset + i];
    		if (chr <= 127)
    			dst[dstOffset + iDst++] = static_cast<uint8_t>(chr);
    		else if (chr <= 2047) {
    			dst[dstOffset + iDst + 0] = static_cast<uint8_t>(192 | chr >> 6);
    			dst[dstOffset + iDst + 1] = static_cast<uint8_t>(128 | (chr & 63));
    			iDst += 2;
    		}
    		else {
    			if (chr >= 55296 && chr <= 57343) {
    				chr = 65533;
    			}
    			dst[dstOffset + iDst + 0] = static_cast<uint8_t>(224 | chr >> 12);
    			dst[dstOffset + iDst + 1] = static_cast<uint8_t>(128 | (chr >> 6 & 63));
    			dst[dstOffset + iDst + 2] = static_cast<uint8_t>(128 | (chr & 63));
    			iDst += 3;
    		}
    	}
    	return iDst;
    }

    auto GetLittleEndianNameAtOffset(const uint8_t* src, int offset)
    {
        std::array<uint16_t, 10 /**< Common Mii name length */> name;
       	loadArrayU16LittleEndian(src, offset, name.data(), 0, name.size());
        {
            std::array<uint8_t, 10 * 3> name8;
            auto length = char16ToUtf8(name8.data(), name.data(), name.size(), 0, 0);
            return std::string(reinterpret_cast<const char *>(name8.data()), length);
        }
    }
}

std::string GetVer3MiiNickname(const std::array<uint8_t, VER3_STORE_DATA_SIZE>& storeData)
{
    return GetLittleEndianNameAtOffset(storeData.data(), 26);
}

std::string GetVer3MiiCreatorName(const std::array<uint8_t, VER3_STORE_DATA_SIZE>& storeData)
{
    return GetLittleEndianNameAtOffset(storeData.data(), 72);
}

Ver3MiiDecoded DecodeVer3MiiFromStoreData(
    const std::array<uint8_t, VER3_STORE_DATA_SIZE> &storeData) {
    return Ver3MiiDecoded{
        .storeData = storeData,
        .nickname = GetVer3MiiNickname(storeData),
        .creatorName = GetVer3MiiCreatorName(storeData),
    };
}

namespace {

using OriginTheory = DbAndCreateId::OriginTheory;

std::string OriginToString(OriginTheory origin) {
    switch (origin) {
        case OriginTheory::wii: return "Wii";
        case OriginTheory::ds: return "DS";
        case OriginTheory::ctr: return "3DS";
        case OriginTheory::wiiu: return "Wii U";
        case OriginTheory::wiiuCamera: return "Wii U (Mii Maker Camera)";
        case OriginTheory::wiiuProbably: return "Probably Wii U";
        case OriginTheory::notWiiu: return "Not Wii U (possibly Switch)";
        case OriginTheory::wiiToCtr: return "Wii, transferred to 3DS";
        case OriginTheory::wiiToWiiu: return "Wii, transferred to Wii U";
        case OriginTheory::wiiToVer3Unofficial: return "Wii, converted unofficially";
        case OriginTheory::dsToCtr: return "DS, transferred to 3DS";
        case OriginTheory::dsToWiiu: return "DS, transferred to Wii U";
        case OriginTheory::wiiuToCtrOrQr: return "Wii U, exported to 3DS/QR code";
        case OriginTheory::birthPlatformFuture: return "Unknown (future platform)";
        case OriginTheory::abnormal:
        default:
            return "Unknown/abnormal";
    }
}

std::string FormatBirthday(int month, int day) {
    static constexpr const char *kMonthNames[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };
    if (month < 1 || month > 12 || day < 1 || day > 31) return "Not set";
    return std::string(kMonthNames[month - 1]) + " " + std::to_string(day);
}

std::string FormatDate(int64_t unixSeconds) {
    if (unixSeconds < 0) return "Unknown";
    time_t t = static_cast<time_t>(unixSeconds);
    struct tm tmv{};
    gmtime_r(&t, &tmv);
    char buf[32];
    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tmv) == 0) return "Unknown";
    return buf;
}

// Plain contiguous hex, no colon separators - a colon-joined byte string
// reads as a MAC address, which a CreateID is explicitly not.
std::string FormatHexBytes(const uint8_t *data, size_t size) {
    std::string out;
    char byteBuf[3];
    for (size_t i = 0; i < size; i++) {
        std::snprintf(byteBuf, sizeof(byteBuf), "%02X", data[i]);
        out += byteBuf;
    }
    return out;
}

auto GetVer3CreateId(const std::array<uint8_t, VER3_STORE_DATA_SIZE>& storeData)
{
    std::array<uint8_t, VER3_CREATE_ID_SIZE> id;
   	std::copy_n(&storeData.at(12), VER3_CREATE_ID_SIZE, id.data());
    return id;
}

} // namespace

MiiDetails ComputeMiiDetails(const Ver3MiiDecoded &mii) {
    MiiDetails d;
    auto createId = GetVer3CreateId(mii.storeData);

    d.createdDate = FormatDate(DbAndCreateId::Ver3CreateId::getDate(createId.data()));

    d.madeOn = OriginToString(DbAndCreateId::Ver3DataHeuristic::guessOriginVer3(mii.storeData.data()));

    d.createIdHex = FormatHexBytes(createId.data(), VER3_CREATE_ID_SIZE);

    // The following fields are from Ver3StoreData:
    // https://github.com/Genwald/MiiPort/blob/4ee38bbb8aa68a2365e9c48d59d7709f760f9b5d/include/mii_ext.h#L170-L264
    {
        const auto& src = mii.storeData;
        d.copyingEnabled = (src[1] & 1) == 1; // copyable

        // d.sharingAllowed = !ex.localOnly && !ex.ngWord;
        d.sharingAllowed = (src[48] & 1) == 0; // localonly

        bool isCreateIdFlagNormal = (createId[0] & 128) == 128;
        d.isSpecial = !isCreateIdFlagNormal;
        d.isFavorite = (src[25] >> 6 & 1) == 1; // favorite
        d.gender = (src[24] & 1) == 0
            ? "Male" : "Female"; // gender

        int birthMonth = src[24] >> 1 & 15;
        int birthDay = (src[25] & 3) << 3 | src[24] >> 5;
        d.birthday = FormatBirthday(birthMonth, birthDay);
    }
    return d;
}
