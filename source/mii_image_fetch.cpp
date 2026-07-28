#include "mii_image_fetch.h"

#include "base64.h"
#include "ctr_log.h"

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

// Cancel-then-close, not just close - see PerformGet()'s own top comment
// (https://github.com/devkitPro/libctru/issues/82: the *original* report
// there, before the issue got closed for the separate TLS-unusability
// reason, was "my 3DS freezes whenever a httpc context is closed with
// httpcCloseContext" - a general statement, not scoped to only a
// mid-download close) - applied to every httpcCloseContext() call in this
// file now, not just the one on the mid-download-failure path an earlier
// version of this function singled out. httpcCancelConnection() on a
// context that was never actively downloading is expected to be a
// harmless no-op.
void CancelAndClose(httpcContext *context) {
    httpcCancelConnection(context);
    httpcCloseContext(context);
}

// Follows up to kMaxRedirects 30x redirects manually (httpc, unlike
// libcurl, doesn't do this itself), matching the pattern in devkitPro's own
// http example.
bool PerformGet(const std::string &url, std::vector<uint8_t> *outBody, const std::atomic<bool> *cancel = nullptr) {
    std::string currentUrl = url;

    for (int redirect = 0; redirect < kMaxRedirects; redirect++) {
        httpcContext context;
        ::Result openResult = httpcOpenContext(&context, HTTPC_METHOD_GET, currentUrl.c_str(), 0);
        CtrLog::Printf("MiiImageFetcher: httpcOpenContext redirect=%d result=0x%08lX", redirect,
                        static_cast<unsigned long>(openResult));
        if (R_FAILED(openResult)) return false;
        httpcSetKeepAlive(&context, HTTPC_KEEPALIVE_DISABLED);
        httpcAddRequestHeaderField(&context, "User-Agent", "mii-transfer-3ds");

        ::Result beginResult = httpcBeginRequest(&context);
        CtrLog::Printf("MiiImageFetcher: httpcBeginRequest result=0x%08lX", static_cast<unsigned long>(beginResult));
        if (R_FAILED(beginResult)) {
            CancelAndClose(&context);
            return false;
        }

        u32 statusCode = 0;
        ::Result statusResult = httpcGetResponseStatusCodeTimeout(&context, &statusCode, kStatusTimeoutNs);
        CtrLog::Printf("MiiImageFetcher: httpcGetResponseStatusCodeTimeout result=0x%08lX statusCode=%lu",
                        static_cast<unsigned long>(statusResult), static_cast<unsigned long>(statusCode));
        if (R_FAILED(statusResult)) {
            CancelAndClose(&context);
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
            CtrLog::Printf("MiiImageFetcher: redirect statusCode=%lu headerResult=0x%08lX",
                            static_cast<unsigned long>(statusCode), static_cast<unsigned long>(headerResult));
            CancelAndClose(&context);
            if (R_FAILED(headerResult)) return false;
            currentUrl = location;
            continue;
        }

        if (statusCode != 200) {
            CancelAndClose(&context);
            return false;
        }

        u8 buffer[kDownloadChunkSize];
        ::Result downloadResult;
        do {
            if (IsCancelled(cancel)) {
                CtrLog::Printf("MiiImageFetcher: cancelled mid-download");
                CancelAndClose(&context);
                return false;
            }
            u32 downloadedSize = 0;
            downloadResult = httpcDownloadData(&context, buffer, sizeof(buffer), &downloadedSize);
            outBody->insert(outBody->end(), buffer, buffer + downloadedSize);
        } while (downloadResult == static_cast<::Result>(HTTPC_RESULTCODE_DOWNLOADPENDING));

        CtrLog::Printf("MiiImageFetcher: download done result=0x%08lX bytes=%zu",
                        static_cast<unsigned long>(downloadResult), outBody->size());
        CancelAndClose(&context);
        return R_SUCCEEDED(downloadResult) && !outBody->empty();
    }

    CtrLog::Printf("MiiImageFetcher: too many redirects");
    return false;
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
