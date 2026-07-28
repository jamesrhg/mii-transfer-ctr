// Generated automatically with "fut". Do not edit.
#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace DbAndCreateId
{

enum class BirthPlatform
{
	min = 1,
	wii = 1,
	ds = 2,
	ctr = 3,
	ctrMax = 3,
	/**
	 * The Wii U birth platform value is also used for
	 * Miitomo (AFL) and nn::mii (Switch amiibo).
	 */
	wiiu = 4,
	max = 7
};

/**
 * Represents a type of Mii data that can have an ID.
 */
enum class CharDataTypeForId
{
	/**
	 * Wii/DS Mii data.
	 * Can be 76 bytes (StoreData), 74 bytes (CharData), or 64 bytes (HiddenCharData).
	 */
	rflCore = 0,
	/**
	 * 3DS/Wii U Mii data.
	 * Can be 96 bytes (StoreData), 92 bytes (Official), or 72 bytes (Core).
	 */
	ver3Core = 1,
	/**
	 * Switch nn::mii::CharInfo type, 88 bytes long.
	 */
	nxCharInfo = 2,
	/**
	 * Switch nn::mii::StoreData type, 68 bytes long.
	 */
	nxStoreData = 3,
	count,
	unknown = -1
};
class CharDataUtility;
class Ver3CreateId;
class NxCreateId;
class CharDatabaseAccessor;
class CharDatabaseWriter;
class CharDatabaseDesc;
class CharDatabaseRfl;
class CharDatabaseRflCtrl;
class CharDatabaseCtr;
class CharDatabaseCafe;
class CharDatabaseNx;
class Ver3DataHeuristic;

enum class OriginTheory
{
	wii,
	ds,
	/**
	 * Created on a 3DS.
	 */
	ctr,
	/**
	 * Data is definitely from Wii U or Miitomo.
	 */
	wiiu,
	/**
	 * Author ID could be from a Wii U.
	 */
	wiiuProbably,
	/**
	 * Data is not from a Wii U, potentially a Switch.
	 */
	notWiiu,
	/**
	 * Author ID suggests an actual Wii U as the source.
	 * Data could be from a Wii U, Miitomo, or other tool.
	 * Platform &amp; ID suggest Wii U and non-Switch features are used.
	 * Created using the "create from camera" feature on Wii U.
	 */
	wiiuCamera,
	/**
	 * Transferred from the Mii Channel to a 3DS.
	 */
	wiiToCtr,
	/**
	 * Transferred from the vWii to a Wii U.
	 */
	wiiToWiiu,
	/**
	 * Converted from Wii to 3DS/Wii U data by unofficial means.
	 */
	wiiToVer3Unofficial,
	/**
	 * Transferred from Tomodachi Collection to Mii Maker (JPN).
	 */
	dsToCtr,
	/**
	 * Transferred from DS -&gt; Wii -&gt; Wii U.
	 */
	dsToWiiu,
	/**
	 * Created on a Wii U and converted to a QR code or transferred to a 3DS.
	 */
	wiiuToCtrOrQr,
	/**
	 * Either birth platform is set to 0 or another
	 * out-of-bounds value, or CreateID does not agree with it.
	 */
	abnormal,
	/**
	 * Birth platform is set to UNUSED "future" values
	 * of 5, 6, 7 accepted on Wii U, Switch, and Miitomo.
	 */
	birthPlatformFuture
};
class Crc16Ccitt;

class CharDataUtility
{
public:
	/**
	 * General-purpose utility for testing an array for all zeroes.
	 */
	static bool isAllZero(uint8_t const * bytes, int length, int offset = 0);
	/**
	 * Gets the data type from the character data length.
	 */
	static CharDataTypeForId getTypeFromLength(int length);
	static constexpr int idLengthMax = 16;
	/**
	 * Offsets of the Create ID for each data type.
	 */
	static constexpr std::array<uint8_t, 4> idOffset = { 24, 12, 0, 48 };
	/**
	 * Length of the Create ID for each data type.
	 */
	static constexpr std::array<uint8_t, 4> idLength = { 8, 10, 16, 16 };
	/**
	 * Returns true if the Ver3CreateId class can be used on this ID.
	 */
	static bool dataTypeHasVer3Id(CharDataTypeForId type);
	/**
	 * Returns true if the data has a CRC-16 checksum requiring an update.
	 */
	static constexpr bool dataHasCrc(int length)
	{
		return length == 96 || length == 76;
	}
	static void createStoreData(uint8_t * dst, int length, uint8_t const * src);
	static void updateCrc16(uint8_t * data, int end, int start = 0);
	static int getNameOffset(CharDataTypeForId type);
	static bool getNameIsLittleEndian(CharDataTypeForId type);
private:
	CharDataUtility() = delete;
	static constexpr int nameLeFlag = 128;
	/**
	 * Offsets of the nickname, plus a flag indicating endianness.
	 */
	static constexpr std::array<uint8_t, 4> nameOffsetAndIsLittle = { 2, 154, 144, 156 };
};

/**
 * Accessor class for Wii/DS/3DS/Wii U Mii Create IDs.
 */
class Ver3CreateId
{
public:
	/**
	 * True = ID is normal, False = ID is special.
	 */
	static bool getNormal(uint8_t const * id);
	static void setNormal(uint8_t * id, bool value);
	/**
	 * True means that the ID is temporary. This
	 * means that it wil never be considered equal
	 * to another ID, and is invalid for saving.
	 */
	static bool getTemporary(uint8_t const * id);
	static void setTemporary(uint8_t * id, bool value);
	/**
	 * Gets the platform in which the ID was created.
	 */
	static BirthPlatform getPlatform(uint8_t const * id);
	static void setPlatform(uint8_t * id, BirthPlatform value);
	/**
	 * Gets the 28-bit offset of the creation date.
	 */
	static int getDateOffset(uint8_t const * id);
	static void setDateOffset(uint8_t * id, int value);
	/**
	 * Gets the DS-specific 18-bit offset of the creation date.
	 */
	static int getDateOffsetDs(uint8_t const * id);
	static constexpr int dateTickVer3 = 2;
	static constexpr int dateEpochVer3 = 1262304000;
	static int64_t getDateVer3(uint8_t const * id);
	static constexpr int dateTickRfl = 4;
	static constexpr int dateEpochRfl = 1136073601;
	static int64_t getDateRfl(uint8_t const * id);
	static constexpr int dateTickDs = 32;
	static constexpr int dateEpochDs = 946702800;
	static int64_t getDate(uint8_t const * id);
	/**
	 * Gets the single-byte checksum of the first three
	 * bytes of the MAC address as calculated in Wii Create ID bases.
	 * Reference: https://github.com/koopthekoopa/RFL/blob/2a5a571f30100c70e855f882e55a53136bc101b3/src/RFL_Database.c#L790-L827
	 */
	static int getWiiAddrSum(uint8_t const * addr);
private: // internal
	static BirthPlatform getPlatformP(int id0);
	friend Ver3DataHeuristic;
private:
	Ver3CreateId() = delete;
	static constexpr int bitNormal = 128;
	static constexpr int bitTemporary = 32;
	/**
	 * The NTR bit is set on the DS, and in the
	 * Cafe Face Library (Wii U, Miitomo, also Switch)
	 */
	static constexpr int bitNtr = 64;
	/**
	 * The CTR bit is set by the 3DS (CFL) and by the
	 * Cafe Face Library (Wii U, Miitomo, also Switch)
	 * It is to be ignored on Mii data from the Wii.
	 */
	static constexpr int bitCtr = 16;
};

class CharDatabaseDesc
{
public:
	CharDatabaseDesc() = default;
	/**
	 * Size of the entire database file.
	 */
	int fileSize;
	/**
	 * Offset of the CRC-16 checksum for the file.
	 */
	int crcOffset;
	/**
	 * Amount of characters in the database.
	 */
	int charDataNum;
	/**
	 * Size of the data for each character.
	 */
	int charDataSize;
	/**
	 * Offset of the character data array.
	 */
	int charDataOffset;
};

class CharDatabaseAccessor
{
public:
	CharDatabaseAccessor() = default;
	CharDatabaseDesc desc;
	uint8_t const * data;
	bool initialize(uint8_t const * data);
	bool isValid() const;
	int getCharDataOffset(int i) const;
	std::shared_ptr<uint8_t[]> getCharData(int i) const;
	bool isEmpty(int i) const;
	int findNextEmpty(int start = 0) const;
	int getNonEmptyNum() const;
	void copyAllNonEmptyCharData(uint8_t * dst, int count) const;
	std::shared_ptr<uint8_t[]> getCharDataAll() const;
	std::shared_ptr<uint8_t[]> charDataFromAll(uint8_t const * allCharData, int i) const;
private: // internal
	static bool detectDataType(CharDatabaseDesc * desc, uint8_t const * data);
	friend CharDatabaseWriter;
};

/**
 * Database header for 3DS/CTR Face Library (CFL_DB.dat).
 */
class CharDatabaseCtr
{
public:
	static void setDesc(CharDatabaseDesc * desc);
private:
	CharDatabaseCtr() = delete;
};

/**
 * Database header for Wii U/Cafe Face Library (FFL_ODB.dat).
 */
class CharDatabaseCafe
{
public:
	static void setDesc(CharDatabaseDesc * desc);
private:
	CharDatabaseCafe() = delete;
};

class Ver3DataHeuristic
{
public:
	static constexpr int dateOffsetCtrAvailable = 16545600;
	static constexpr int dateOffsetWiiuAvailable = 44971200;
	/**
	 * True if the date offset is before its respective console
	 * released. This reflects if the character was created before
	 * the console was publicly available, or if the date is overflown.
	 */
	static constexpr bool isDateBeforeConsoleAvailable(int dateOffset, BirthPlatform platform)
	{
		return dateOffset < (platform == BirthPlatform::wiiu ? 44971200 : 16545600);
	}
	/**
	 * Adjusts the 3DS/Wii U ID date in the case of overflows.
	 */
	static constexpr int overflowAdjust(int dateOffset, BirthPlatform platform)
	{
		return isDateBeforeConsoleAvailable(dateOffset, platform) ? dateOffset + 268435456 : dateOffset;
	}
	static bool isMiiVersionForWiiuCamera(uint8_t const * ver3StoreData);
	static bool isAuthorIdPotentiallyWiiu(uint8_t const * authorId, int offset = 0);
	static bool isDefinitelyWiiu(uint8_t const * ver3StoreData);
	static OriginTheory guessOriginVer3(uint8_t const * ver3StoreData);
	static OriginTheory getOriginRfl(uint8_t const * rflData);
private:
	Ver3DataHeuristic() = delete;
	static bool creatorNamePresent(uint8_t const * name, int offset);
	static bool nameContinuouslyTerminated(uint8_t const * name, int offset, int count = 10);
	static bool hasNonNxFeature(uint8_t const * data);
};

/**
 * Implements the CRC-16/CCITT checksum calculation.
 * This class provides a static method for computing
 * the CRC-16/CCITT checksum over an input byte array.
 * The algorithm uses the polynomial 0x1021, an initial
 * value of 0xFFFF, and a default context of 0x0000.
 */
class Crc16Ccitt
{
public:
	/**
	 * Calculates the CRC-16/CCITT checksum for the specified input data.
	 * Courtesy of Luciano Barcaro: https://stackoverflow.com/a/30357446
	 */
	static int calculate(uint8_t const * input, int size);
	static void updateBigEndian(uint8_t * data, int end, int start = 0);
private:
	Crc16Ccitt() = delete;
};
}
