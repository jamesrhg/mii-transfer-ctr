// Generated automatically with "fut". Do not edit.
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include "DbAndCreateId.hpp"

namespace DbAndCreateId
{

bool CharDataUtility::isAllZero(uint8_t const * bytes, int length, int offset)
{
	for (int i = 0; i < length; i++)
		if (bytes[offset + i] != 0)
			return false;
	return true;
}

CharDataTypeForId CharDataUtility::getTypeFromLength(int length)
{
	switch (length) {
	case 96:
	case 92:
	case 72:
		return CharDataTypeForId::ver3Core;
	case 76:
	case 74:
	case 64:
		return CharDataTypeForId::rflCore;
	case 88:
		return CharDataTypeForId::nxCharInfo;
	case 68:
		return CharDataTypeForId::nxStoreData;
	default:
		return CharDataTypeForId::unknown;
	}
}

bool CharDataUtility::dataTypeHasVer3Id(CharDataTypeForId type)
{
	return static_cast<int>(type) < 2;
}

void CharDataUtility::createStoreData(uint8_t * dst, int length, uint8_t const * src)
{
	std::copy_n(src, length, dst);
	updateCrc16(dst, length);
}

void CharDataUtility::updateCrc16(uint8_t * data, int end, int start)
{
	int offset = start + end - 2;
	data[offset] = data[offset + 1] = 0;
	for (int i = 0; i < end - 2; i++) {
		int x = data[start + i] ^ data[offset];
		x ^= x >> 4;
		data[offset] = static_cast<uint8_t>(data[offset + 1] ^ x >> 3 ^ x << 4);
		data[offset + 1] = static_cast<uint8_t>(x ^ x << 5);
	}
}

int CharDataUtility::getNameOffset(CharDataTypeForId type)
{
	return nameOffsetAndIsLittle[static_cast<int>(type)] & 127;
}

bool CharDataUtility::getNameIsLittleEndian(CharDataTypeForId type)
{
	return nameOffsetAndIsLittle[static_cast<int>(type)] >> 7 == 1;
}

bool Ver3CreateId::getNormal(uint8_t const * id)
{
	return (id[0] & 128) == 128;
}

void Ver3CreateId::setNormal(uint8_t * id, bool value)
{
	id[0] = static_cast<uint8_t>(value ? id[0] | 128 : id[0] & ~128);
}

bool Ver3CreateId::getTemporary(uint8_t const * id)
{
	return (id[0] & 32) == 32;
}

void Ver3CreateId::setTemporary(uint8_t * id, bool value)
{
	id[0] = static_cast<uint8_t>(value ? id[0] | 32 : id[0] & ~32);
}

BirthPlatform Ver3CreateId::getPlatform(uint8_t const * id)
{
	return getPlatformP(id[0]);
}

BirthPlatform Ver3CreateId::getPlatformP(int id0)
{
	switch (id0 & 80) {
	case 16:
		return BirthPlatform::ctr;
	case 64:
		return BirthPlatform::ds;
	case 80:
		return BirthPlatform::wiiu;
	default:
		return BirthPlatform::wii;
	}
}

void Ver3CreateId::setPlatform(uint8_t * id, BirthPlatform value)
{
	id[0] &= ~80;
	switch (value) {
	case BirthPlatform::ctr:
		id[0] |= 16;
		break;
	case BirthPlatform::ds:
		id[0] |= 64;
		break;
	case BirthPlatform::wiiu:
		id[0] |= 80;
		break;
	default:
		break;
	}
}

int Ver3CreateId::getDateOffset(uint8_t const * id)
{
	return (id[0] & 15) << 24 | id[1] << 16 | id[2] << 8 | id[3];
}

void Ver3CreateId::setDateOffset(uint8_t * id, int value)
{
	value &= 268435455;
	id[0] = static_cast<uint8_t>((id[0] & 240) | (value >> 24 & 15));
	id[1] = static_cast<uint8_t>(value >> 16);
	id[2] = static_cast<uint8_t>(value >> 8);
	id[3] = static_cast<uint8_t>(value);
}

int Ver3CreateId::getDateOffsetDs(uint8_t const * id)
{
	return (id[2] & 3) << 16 | id[3] << 8 | id[4];
}

int64_t Ver3CreateId::getDateVer3(uint8_t const * id)
{
	return getDateOffset(id) * 2 + 1262304000;
}

int64_t Ver3CreateId::getDateRfl(uint8_t const * id)
{
	return getDateOffset(id) * 4 + 1136073601;
}

int64_t Ver3CreateId::getDate(uint8_t const * id)
{
	BirthPlatform p = getPlatform(id);
	if (p == BirthPlatform::ds)
		return -1;
	else if (static_cast<int>(p) >= 3)
		return getDateVer3(id);
	else
		return getDateRfl(id);
}

int Ver3CreateId::getWiiAddrSum(uint8_t const * addr)
{
	int sum = (addr[0] + addr[1] + addr[2]) & 255;
	if (addr[0] != 0 || addr[1] != 23 || addr[2] != 171)
		sum &= 127;
	return sum;
}

bool CharDatabaseAccessor::initialize(uint8_t const * data)
{
	this->data = data;
	return detectDataType(&this->desc, data);
}

bool CharDatabaseAccessor::isValid() const
{
	return Crc16Ccitt::calculate(this->data, this->desc.crcOffset + 2) == 0;
}

int CharDatabaseAccessor::getCharDataOffset(int i) const
{
	return this->desc.charDataOffset + this->desc.charDataSize * i;
}

std::shared_ptr<uint8_t[]> CharDatabaseAccessor::getCharData(int i) const
{
	std::shared_ptr<uint8_t[]> out = std::make_shared<uint8_t[]>(this->desc.charDataSize);
	std::copy_n(this->data + getCharDataOffset(i), this->desc.charDataSize, out.get());
	return out;
}

bool CharDatabaseAccessor::isEmpty(int i) const
{
	CharDataTypeForId type = CharDataUtility::getTypeFromLength(this->desc.charDataSize);
	int idOffset = CharDataUtility::idOffset[static_cast<int>(type)];
	int offset = getCharDataOffset(i) + idOffset;
	int length = CharDataUtility::idLength[static_cast<int>(type)];
	return CharDataUtility::isAllZero(this->data, length, offset);
}

int CharDatabaseAccessor::findNextEmpty(int start) const
{
	for (int i = start; i < this->desc.charDataNum; i++)
		if (isEmpty(i))
			return i;
	return -1;
}

int CharDatabaseAccessor::getNonEmptyNum() const
{
	int num = 0;
	for (int i = 0; i < this->desc.charDataNum; i++)
		if (!isEmpty(i))
			num += 1;
	return num;
}

void CharDatabaseAccessor::copyAllNonEmptyCharData(uint8_t * dst, int count) const
{
	int outIndex = 0;
	for (int i = 0; i < count; i++) {
		if (isEmpty(i))
			continue;
		int outOffset = outIndex * this->desc.charDataSize;
		std::copy_n(this->data + getCharDataOffset(i), this->desc.charDataSize, dst + outOffset);
	}
}

std::shared_ptr<uint8_t[]> CharDatabaseAccessor::getCharDataAll() const
{
	int num = getNonEmptyNum();
	int size = num * this->desc.charDataSize;
	std::shared_ptr<uint8_t[]> out = std::make_shared<uint8_t[]>(size);
	copyAllNonEmptyCharData(out.get(), num);
	return out;
}

std::shared_ptr<uint8_t[]> CharDatabaseAccessor::charDataFromAll(uint8_t const * allCharData, int i) const
{
	std::shared_ptr<uint8_t[]> out = std::make_shared<uint8_t[]>(this->desc.charDataSize);
	int offset = i * this->desc.charDataSize;
	std::copy_n(this->data + offset, this->desc.charDataSize, out.get());
	return out;
}


void CharDatabaseCtr::setDesc(CharDatabaseDesc * desc)
{
	desc->fileSize = 310560;
	desc->crcOffset = 51230;
	desc->charDataNum = 100;
	desc->charDataSize = 92;
	desc->charDataOffset = 8;
}

void CharDatabaseCafe::setDesc(CharDatabaseDesc * desc)
{
	desc->fileSize = 276544;
	desc->crcOffset = 276542;
	desc->charDataNum = 3000;
	desc->charDataSize = 92;
	desc->charDataOffset = 8;
}

bool Ver3DataHeuristic::isMiiVersionForWiiuCamera(uint8_t const * ver3StoreData)
{
	return ver3StoreData[0] == 0;
}

bool Ver3DataHeuristic::isAuthorIdPotentiallyWiiu(uint8_t const * authorId, int offset)
{
	return (authorId[offset + 0] & 4) == 0 && (authorId[offset + 4] & 8) == 0 && (authorId[offset + 5] & 12) == 4 && (authorId[offset + 6] & 12) == 0 && (authorId[offset + 7] & 12) == 0;
}

bool Ver3DataHeuristic::isDefinitelyWiiu(uint8_t const * ver3StoreData)
{
	return creatorNamePresent(ver3StoreData, 72) || hasNonNxFeature(ver3StoreData) || !nameContinuouslyTerminated(ver3StoreData, 26);
}

bool Ver3DataHeuristic::creatorNamePresent(uint8_t const * name, int offset)
{
	for (int i = 0; i < 20; i++)
		if (name[offset + i] != 0)
			return true;
	return false;
}

bool Ver3DataHeuristic::nameContinuouslyTerminated(uint8_t const * name, int offset, int count)
{
	bool foundTerminator = false;
	for (int i = 0; i < count; i++) {
		int off = offset + i * 2;
		bool isNull = name[off] == 0 && name[off + 1] == 0;
		if (!foundTerminator && isNull)
			foundTerminator = true;
		else if (foundTerminator && !isNull)
			return false;
	}
	return true;
}

bool Ver3DataHeuristic::hasNonNxFeature(uint8_t const * data)
{
	return (data[1] & 1) != 0 || (data[1] >> 1 & 1) != 0 || (data[2] & 15) != 0 || data[2] >> 4 != 0 || (data[24] >> 1 & 15) != 0 || ((data[25] & 3) << 3 | data[24] >> 5) != 0 || (data[25] >> 6 & 1) != 0 || (data[48] & 1) != 0;
}

OriginTheory Ver3DataHeuristic::guessOriginVer3(uint8_t const * ver3StoreData)
{
	int birthPlatform = ver3StoreData[3] >> 4 & 7;
	BirthPlatform idPlatform = Ver3CreateId::getPlatformP(ver3StoreData[12]);
	switch (birthPlatform) {
	case 1:
		{
			switch (idPlatform) {
			case BirthPlatform::wii:
				{
					return OriginTheory::wiiToWiiu;
				}
			case BirthPlatform::ds:
				return OriginTheory::dsToWiiu;
			case BirthPlatform::ctr:
				return OriginTheory::wiiToCtr;
			default:
				return OriginTheory::abnormal;
			}
		}
	case 2:
		return idPlatform == BirthPlatform::ctr ? OriginTheory::dsToCtr : OriginTheory::abnormal;
	case 3:
		switch (idPlatform) {
		case BirthPlatform::wiiu:
			return OriginTheory::wiiuToCtrOrQr;
		case BirthPlatform::ctr:
			return OriginTheory::ctr;
		default:
			return OriginTheory::abnormal;
		}
	case 4:
		{
			if (idPlatform != BirthPlatform::wiiu)
				return OriginTheory::abnormal;
			if (isMiiVersionForWiiuCamera(ver3StoreData))
				return OriginTheory::wiiuCamera;
			else if (isDefinitelyWiiu(ver3StoreData))
				return OriginTheory::wiiu;
			else if (isAuthorIdPotentiallyWiiu(ver3StoreData, 4))
				return OriginTheory::wiiuProbably;
			else
				return OriginTheory::notWiiu;
		}
	case 5:
	case 6:
	case 7:
		return OriginTheory::birthPlatformFuture;
	default:
		return OriginTheory::abnormal;
	}
}

OriginTheory Ver3DataHeuristic::getOriginRfl(uint8_t const * rflData)
{
	BirthPlatform idPlatform = Ver3CreateId::getPlatformP(rflData[24]);
	return idPlatform == BirthPlatform::ds ? OriginTheory::ds : OriginTheory::wii;
}

int Crc16Ccitt::calculate(uint8_t const * input, int size)
{
	int msb = 0;
	int lsb = 0;
	for (int i = 0; i < size; i++) {
		int x = input[i] ^ msb;
		x ^= x >> 4;
		msb = (lsb ^ x >> 3 ^ x << 4) & 255;
		lsb = (x ^ x << 5) & 255;
	}
	return msb << 8 | lsb;
}

void Crc16Ccitt::updateBigEndian(uint8_t * data, int end, int start)
{
	int offset = start + end - 2;
	data[offset] = data[offset + 1] = 0;
	for (int i = 0; i < end - 2; i++) {
		int x = data[start + i] ^ data[offset];
		x ^= x >> 4;
		data[offset] = static_cast<uint8_t>(data[offset + 1] ^ x >> 3 ^ x << 4);
		data[offset + 1] = static_cast<uint8_t>(x ^ x << 5);
	}
}
}
