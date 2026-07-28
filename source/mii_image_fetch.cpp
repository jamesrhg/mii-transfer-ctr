#include "mii_image_fetch.h"

#include "base64.h"

#include <3ds.h>

#include <atomic>

namespace MiiImageFetcher {

namespace {

bool g_started = false;

std::string UrlEncode(const std::string &in) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string BuildImageUrl(const std::array<uint8_t, 96> &storeData, int widthPx,
                           const std::string &expression = "") {
    std::string data = UrlEncode(Base64Encode(storeData.data(), storeData.size()));
    // Plain http://, not https:// - see this file's own header comment for
    // why httpc can get away with that here (nothing to TLS-handshake at
    // all) after being ruled out for HTTPS use entirely.
    std::string url = "http://mii-unsecure.ariankordi.net/miis/image.png"
                       "?data=" +
                       data + "&type=face&width=" + std::to_string(widthPx) +
                       "&texResolution=128&resourceType=low&shaderType=wiiu_blinn&scale=1";
    if (!expression.empty()) url += "&expression=" + UrlEncode(expression);
    return url;
}

// Polled between download chunks (httpcDownloadData() only ever fills one
// buffer's worth per call, so there's a natural point to check between
// calls) - lets FetchImageBlocking()'s caller abort an in-progress request
// from another thread instead of waiting for it to finish or time out.
bool IsCancelled(const std::atomic<bool> *cancel) { return cancel && cancel->load(); }

constexpr int kMaxRedirects = 5;
constexpr u64 kStatusTimeoutNs = 20LL * 1000 * 1000 * 1000;
constexpr u32 kDownloadChunkSize = 4096;

// Follows up to kMaxRedirects 30x redirects manually (httpc, unlike
// libcurl, doesn't do this itself), matching the pattern in devkitPro's own
// http example.
bool PerformGet(const std::string &url, std::vector<uint8_t> *outBody, const std::atomic<bool> *cancel = nullptr) {
    std::string currentUrl = url;

    for (int redirect = 0; redirect < kMaxRedirects; redirect++) {
        httpcContext context;
        if (R_FAILED(httpcOpenContext(&context, HTTPC_METHOD_GET, currentUrl.c_str(), 0))) return false;
        httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_DISABLED);
        httpcAddRequestHeaderField(&context, "User-Agent", "mii-transfer-3ds");

        if (R_FAILED(httpcBeginRequest(&context))) {
            httpcCloseContext(&context);
            return false;
        }

        u32 statusCode = 0;
        if (R_FAILED(httpcGetResponseStatusCodeTimeout(&context, &statusCode, kStatusTimeoutNs))) {
            httpcCloseContext(&context);
            return false;
        }

        if (statusCode >= 301 && statusCode <= 308 && statusCode != 304 && statusCode != 305 &&
            statusCode != 306) {
            char location[1024];
            // ::Result (global, from 3ds.h - a plain long), not this file's
            // own MiiImageFetcher::Result: unqualified `Result` inside this
            // namespace resolves to the latter, which isn't compatible with
            // R_FAILED()/R_SUCCEEDED() or httpc's own return values.
            ::Result headerResult = httpcGetResponseHeader(&context, "Location", location, sizeof(location));
            httpcCloseContext(&context);
            if (R_FAILED(headerResult)) return false;
            currentUrl = location;
            continue;
        }

        if (statusCode != 200) {
            httpcCloseContext(&context);
            return false;
        }

        u8 buffer[kDownloadChunkSize];
        ::Result downloadResult;
        do {
            if (IsCancelled(cancel)) {
                httpcCancelConnection(&context);
                httpcCloseContext(&context);
                return false;
            }
            u32 downloadedSize = 0;
            downloadResult = httpcDownloadData(&context, buffer, sizeof(buffer), &downloadedSize);
            outBody->insert(outBody->end(), buffer, buffer + downloadedSize);
        } while (downloadResult == static_cast<::Result>(HTTPC_RESULTCODE_DOWNLOADPENDING));

        // httpcCloseContext() hangs forever unless the transfer either
        // completed or was explicitly cancelled first (see httpc.h's own
        // doc comment on httpcDownloadData(), and
        // https://github.com/devkitPro/libctru/issues/82 - the exact bug
        // that issue was originally about, well before the TLS-unusability
        // reason it was eventually closed for) - a genuine mid-transfer
        // error here (not the DOWNLOADPENDING loop exit above, not the
        // cancel-token path above, but e.g. the connection dropping) would
        // leave the transfer incomplete, so it needs the same
        // cancel-before-close treatment.
        if (R_FAILED(downloadResult)) httpcCancelConnection(&context);
        httpcCloseContext(&context);
        return R_SUCCEEDED(downloadResult) && !outBody->empty();
    }

    return false; // too many redirects
}

} // namespace

void Start() {
    if (g_started) return;
    g_started = true;
    // 0 sharedmem_size: this app only ever does HTTP GET (no POST bodies to
    // stage into httpc's shared memory) - see httpc.h's own doc on
    // httpcInit() for why GET-only callers can pass 0.
    httpcInit(0);
}

void Stop() {
    if (!g_started) return;
    httpcExit();
    g_started = false;
}

Result FetchImageBlocking(const std::array<uint8_t, 96> &storeData, int widthPx,
                           const std::string &expression, const std::atomic<bool> *cancel) {
    Result result;

    std::string url = BuildImageUrl(storeData, widthPx, expression);
    std::vector<uint8_t> body;
    result.success = PerformGet(url, &body, cancel);
    if (result.success) result.pngBytes = std::move(body);

    return result;
}

} // namespace MiiImageFetcher
