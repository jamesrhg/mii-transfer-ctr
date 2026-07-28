// Mii Transfer (3DS) - client-side parser for this console's three Mii data
// sources, ported field-for-field from this app's own C++ decode logic
// (source/ctr_mii_db.cpp, source/lib/DbAndCreateId.cpp), not guessed at.
// Ported from the Wii U build's mii-parser.js - see this file's own history
// for that version, which parsed FFL_ODB.dat (Wii U) instead of CFL_DB.dat
// (3DS).
//
// Two genuinely different architectures, per their different origins:
//
// - /data/act_db.dat and /data/fp_db.dat (ACT/FRD) are just raw,
//   concatenated CFLStoreData records - 96 bytes each, back to back, no
//   header, no separators. Already in the exact byte layout everything
//   else (this app's own face-image requests, mii-unsecure.ariankordi.net)
//   expects - no decoding needed beyond slicing and reading the name
//   fields directly.
//
// - /data/CFL_DB.dat is the actual on-console Mii Library database file (3DS
//   Face Library format): a fixed header/index container of up to 100
//   *92*-byte "core" records (not 96 - no padding/CRC of their own), each of
//   which needs an empty-slot check before use. Unlike the Wii U's
//   FFL_ODB.dat, CFL_DB.dat's records need **no endian swap** - they're
//   already in the same little-endian layout as everything else (see
//   ctr_mii_db.cpp's own comment on why) - so this just zero-pads and
//   computes a real CRC-16 to reconstruct a proper 96-byte StoreData for
//   each one, without the swapVer3Data() step the FFL_ODB.dat path needed.
//   This same file also has a second, embedded array at a different offset -
//   the "CFRA" section (Miis recently picked in *other* titles, e.g.
//   friend-request/NPC pickers - shown here as "Recent Miis") - parsed by
//   parseCflDbRecent() below, off the exact same downloaded bytes, rather
//   than the server exposing a second endpoint for it (it's not a separate
//   file on the console either - see source/ctr_mii_db.cpp's own comment on
//   the CFRA offsets, confirmed against 3dbrew's own CFL_DB.dat layout docs).
//
// Everything here operates on plain, generic "byte arrays" (anything with a
// numeric .length and numeric-indexed access) rather than requiring typed
// arrays specifically - see loadBinary().
//
// Usage:
//   MiiParser.loadBinary('/data/fp_db.dat', function (bytes) {
//     var miis = MiiParser.parseActFpDb(bytes);
//   }, function (err) { ... });
//   MiiParser.loadBinary('/data/CFL_DB.dat', function (bytes) {
//     var libraryMiis = MiiParser.parseCflDb(bytes);
//     var recentMiis = MiiParser.parseCflDbRecent(bytes); // same bytes, no extra request
//   }, function (err) { ... });
//   // Each entry: { nickname, creatorName, raw: <byte array, length 96> }
//   // `raw` is a valid, ready-to-use CFLStoreData/Ver3StoreData blob for
//   // all three sources - e.g. MiiParser.faceImageUrl(mii). creatorName is
//   // always "" for parseCflDbRecent()'s entries - the CFRA section uses
//   // Nintendo's more compact "Core" Mii format, which has no creator-name
//   // field at all (matches this app's own C++ decode: GetVer3MiiCreatorName()
//   // on these always returns empty too).
window.MiiParser = (function () {
  "use strict";

  var STORE_DATA_SIZE = 96;

  // --- Shared byte-array helpers ---------------------------------------

  function sliceBytes(bytes, start, end) {
    var out = new Array(end - start);
    for (var i = start; i < end; i++) out[i - start] = bytes[i];
    return out;
  }

  function toByteArray(bytes) {
    if (typeof ArrayBuffer !== "undefined" && bytes instanceof ArrayBuffer) {
      return new Uint8Array(bytes);
    }
    return bytes;
  }

  function utf16Field(rec, offset) {
    var out = "";
    for (var i = 0; i < 10; i++) {
      var code = rec[offset + i * 2] | (rec[offset + i * 2 + 1] << 8);
      if (code === 0) break;
      out += String.fromCharCode(code);
    }
    return out;
  }

  var BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  function base64EncodeBytes(bytes) {
    var i, len = bytes.length;
    if (typeof window.btoa === "function") {
      var binary = "";
      for (i = 0; i < len; i++) binary += String.fromCharCode(bytes[i]);
      return window.btoa(binary);
    }
    var out = "";
    for (i = 0; i < len; i += 3) {
      var b0 = bytes[i];
      var b1 = (i + 1 < len) ? bytes[i + 1] : 0;
      var b2 = (i + 2 < len) ? bytes[i + 2] : 0;
      var triplet = (b0 << 16) | (b1 << 8) | b2;
      out += BASE64_CHARS.charAt((triplet >> 18) & 0x3f);
      out += BASE64_CHARS.charAt((triplet >> 12) & 0x3f);
      out += (i + 1 < len) ? BASE64_CHARS.charAt((triplet >> 6) & 0x3f) : "=";
      out += (i + 2 < len) ? BASE64_CHARS.charAt(triplet & 0x3f) : "=";
    }
    return out;
  }

  function faceImageUrl(mii, opts) {
    opts = opts || {};
    var width = opts.width || 128;
    var expression = opts.expression || "";
    var url = "https://mii-unsecure.ariankordi.net/miis/image.png?type=face&shaderType=wiiu_blinn&resourceType=low&texResolution=96&width=" + width +
              "&data=" + encodeURIComponent(base64EncodeBytes(mii.raw));
    if (expression) url += "&expression=" + encodeURIComponent(expression);
    return url;
  }

  // --- Binary loading (XMLHttpRequest, with an ActiveX + text/binary
  //     fallback path for browsers that predate it) ---------------------

  function createXHR() {
    if (typeof window.XMLHttpRequest !== "undefined") {
      return new XMLHttpRequest();
    }
    if (typeof window.ActiveXObject !== "undefined") {
      var progIds = ["Msxml2.XMLHTTP.6.0", "Msxml2.XMLHTTP.3.0", "Msxml2.XMLHTTP", "Microsoft.XMLHTTP"];
      for (var i = 0; i < progIds.length; i++) {
        try {
          return new ActiveXObject(progIds[i]);
        } catch (e) {
          // try the next ProgID
        }
      }
    }
    return null;
  }

  function xhrResponseToByteArray(xhr, useArrayBuffer) {
    var i, len, out, view;

    if (useArrayBuffer) {
      view = new Uint8Array(xhr.response);
      len = view.length;
      out = new Array(len);
      for (i = 0; i < len; i++) out[i] = view[i];
      return out;
    }

    if (xhr.responseBody && typeof VBArray !== "undefined") {
      return new VBArray(xhr.responseBody).toArray();
    }

    var text = xhr.responseText || "";
    len = text.length;
    out = new Array(len);
    for (i = 0; i < len; i++) out[i] = text.charCodeAt(i) & 0xff;
    return out;
  }

  function loadBinary(url, onSuccess, onError) {
    var xhr = createXHR();
    if (!xhr) {
      if (onError) onError("XMLHttpRequest is not supported in this browser.");
      return;
    }

    var useArrayBuffer = ("responseType" in xhr) && (typeof ArrayBuffer !== "undefined");

    xhr.open("GET", url, true);

    if (useArrayBuffer) {
      xhr.responseType = "arraybuffer";
    } else if (xhr.overrideMimeType) {
      xhr.overrideMimeType("text/plain; charset=x-user-defined");
    }

    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4) return;
      if (xhr.status !== 200 && xhr.status !== 0) {
        if (onError) onError("HTTP " + xhr.status);
        return;
      }
      var bytes;
      try {
        bytes = xhrResponseToByteArray(xhr, useArrayBuffer);
      } catch (e) {
        if (onError) onError("Could not read response: " + e.message);
        return;
      }
      if (onSuccess) onSuccess(bytes);
    };

    xhr.send(null);
  }

  // --- /data/act_db.dat, /data/fp_db.dat -------------------------------
  // Raw concatenated CFLStoreData - see the file header comment.

  function parseActFpDb(bytes) {
    bytes = toByteArray(bytes);
    var count = Math.floor(bytes.length / STORE_DATA_SIZE);
    var out = [];
    for (var i = 0; i < count; i++) {
      var rec = sliceBytes(bytes, i * STORE_DATA_SIZE, (i + 1) * STORE_DATA_SIZE);
      out.push({
        nickname: utf16Field(rec, 26),
        creatorName: utf16Field(rec, 72),
        raw: rec
      });
    }
    return out;
  }

  // --- /data/CFL_DB.dat --------------------------------------------------
  // 3DS Mii Maker Face Library database container - fixed layout, matching
  // DbAndCreateId::CharDatabaseCtr::setDesc() exactly (not read from the
  // file itself).
  var CFL_DB = {
    fileSize: 310560,
    crcOffset: 51230,
    charDataNum: 100,
    charDataSize: 92, // "core" record - no pad/CRC of its own, unlike StoreData
    charDataOffset: 8,
    // Where a record's CreateID lives, used only to detect empty slots (an
    // all-zero CreateID) before this record is otherwise touched.
    createIdOffset: 12,
    createIdLength: 10
  };

  // The CFRA "recent Miis" section, embedded inside the same CFL_DB.dat file
  // - starts at file offset 0xC820 ("CFRA" magic + a u32 count + a 100-byte
  // order-index array), with its own 100-entry Mii array right after that at
  // 0xC88C, each entry 0x48 (72) bytes - Nintendo's more compact "Core" Mii
  // format (no creator-name field) - matches source/ctr_mii_db.cpp's own
  // kCfraCharDataOffset/kCfraCharDataSize/kCfraCharDataNum exactly.
  // createIdOffset/createIdLength are unchanged from the main array above -
  // DbAndCreateId.hpp's own idOffset/idLength tables give the CreateID the
  // same offset (12) and length (10) for both the 92-byte "Official" and
  // 72-byte "Core" ver3 record shapes.
  var CFL_DB_RECENT = {
    charDataNum: 100,
    charDataSize: 72, // "Core" record - no creator-name field at all
    charDataOffset: 0xC88C,
    createIdOffset: 12,
    createIdLength: 10
  };

  // Crc16Ccitt::calculate() (DbAndCreateId.cpp) - a custom bit-shuffled
  // CRC-16, not a standard table-based CCITT variant - ported literally.
  function crc16Calculate(bytes, size) {
    var msb = 0, lsb = 0;
    for (var i = 0; i < size; i++) {
      var x = (bytes[i] ^ msb) & 0xff;
      x ^= x >> 4;
      msb = (lsb ^ (x >> 3) ^ ((x << 4) & 0xff)) & 0xff;
      lsb = (x ^ ((x << 5) & 0xff)) & 0xff;
    }
    return (msb << 8) | lsb;
  }

  // Crc16Ccitt::updateBigEndian(data, end, start=0) - writes a 2-byte CRC at
  // data[start+end-2 .. start+end), computed over data[start .. start+end-2)
  // (which must already have those 2 bytes zeroed).
  function crc16UpdateBigEndian(data, end, start) {
    start = start || 0;
    var offset = start + end - 2;
    data[offset] = 0;
    data[offset + 1] = 0;
    for (var i = 0; i < end - 2; i++) {
      var x = (data[start + i] ^ data[offset]) & 0xff;
      x ^= x >> 4;
      data[offset] = (data[offset + 1] ^ (x >> 3) ^ ((x << 4) & 0xff)) & 0xff;
      data[offset + 1] = (x ^ ((x << 5) & 0xff)) & 0xff;
    }
  }

  function isAllZero(bytes, offset, length) {
    for (var i = 0; i < length; i++) if (bytes[offset + i] !== 0) return false;
    return true;
  }

  // True if `bytes` (the whole file) passes CFL_DB.dat's own CRC-16 -
  // CharDatabaseAccessor::isValid(). Not required to read Miis out of the
  // file, but a useful sanity check that the download wasn't truncated or
  // corrupted.
  function isCflDbValid(bytes) {
    bytes = toByteArray(bytes);
    return crc16Calculate(bytes, CFL_DB.crcOffset + 2) === 0;
  }

  // Shared by parseCflDb() (the main 92-byte-record array) and
  // parseCflDbRecent() (the embedded 72-byte-record CFRA array) - only the
  // offsets/sizes differ, per `desc` (CFL_DB or CFL_DB_RECENT above).
  function parseCflDbSection(bytes, desc) {
    bytes = toByteArray(bytes);

    var out = [];
    for (var n = 0; n < desc.charDataNum; n++) {
      var recordOffset = desc.charDataOffset + desc.charDataSize * n;
      if (isAllZero(bytes, recordOffset + desc.createIdOffset, desc.createIdLength)) continue; // empty slot

      // core record -> 96-byte StoreData: zero-pad, then compute a real
      // CRC-16 - **no endian swap** here (see the file header comment for
      // why CFL_DB.dat differs from the Wii U build's FFL_ODB.dat in
      // exactly this one respect). For the 72-byte CFRA case this also
      // zero-pads out the creator-name field entirely, since that record
      // shape doesn't have one - utf16Field() below then naturally reads it
      // back as "".
      var storeData = new Array(STORE_DATA_SIZE);
      for (var z = 0; z < STORE_DATA_SIZE; z++) storeData[z] = 0;
      var core = sliceBytes(bytes, recordOffset, recordOffset + desc.charDataSize);
      for (var k = 0; k < core.length; k++) storeData[k] = core[k];
      crc16UpdateBigEndian(storeData, STORE_DATA_SIZE);

      out.push({
        nickname: utf16Field(storeData, 26),
        creatorName: utf16Field(storeData, 72),
        raw: storeData
      });
    }
    return out;
  }

  function parseCflDb(bytes) {
    return parseCflDbSection(bytes, CFL_DB);
  }

  // The "Recent Miis" tab's data - see this file's own header comment and
  // CFL_DB_RECENT's own comment above. Takes the *same* CFL_DB.dat bytes
  // parseCflDb() does, not a separate download.
  function parseCflDbRecent(bytes) {
    return parseCflDbSection(bytes, CFL_DB_RECENT);
  }

  return {
    parseActFpDb: parseActFpDb,
    parseCflDb: parseCflDb,
    parseCflDbRecent: parseCflDbRecent,
    faceImageUrl: faceImageUrl,
    isCflDbValid: isCflDbValid,
    loadBinary: loadBinary
  };
})();
