#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Standard (RFC 4648) base64 encoding, `=`-padded. Used by
// mii_image_fetch.cpp for the face-render request's "data" query param.
std::string Base64Encode(const uint8_t *data, size_t size);
