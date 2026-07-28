#include "mii_http_server.h"

#include "ctr_log.h"
#include "ctr_network.h"
#include "ctr_thread.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace MiiHttpServer {

namespace {

constexpr int kMaxPortAttempts = 6;

int g_listenSocket = -1;
Thread g_thread = nullptr;
std::atomic<bool> g_stopRequested{false};
uint32_t g_assignedIp = 0;
uint16_t g_boundPort = 0;

// State/error are written only by the background thread (SetupAndServe(),
// just once, before it moves on to accepting connections) and read only by
// GetState()/GetError() on the caller's thread, always under g_stateMutex -
// see Start()'s own comment for why setup must never run on the caller's
// thread at all.
std::mutex g_stateMutex;
State g_state = State::Starting;
std::string g_error;

std::mutex g_dataMutex;
std::vector<uint8_t> g_cflDbData;
std::vector<uint8_t> g_friendMiiData;

bool ReadFileFully(const char *path, std::vector<uint8_t> *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return false;
    }
    out->resize(static_cast<size_t>(size));
    size_t bytesRead = fread(out->data(), 1, out->size(), f);
    fclose(f);
    return bytesRead == out->size();
}

void SendAll(int fd, const void *data, size_t size) {
    const char *p = static_cast<const char *>(data);
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, p + sent, size - sent, 0);
        if (n <= 0) return; // Peer gone - nothing more we can do.
        sent += static_cast<size_t>(n);
    }
}

void SendResponse(int fd, int statusCode, const char *statusText, const char *contentType,
                   const char *extraHeaders, const void *body, size_t bodySize) {
    char header[512];
    int headerLen = std::snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        statusCode, statusText, contentType, bodySize, extraHeaders ? extraHeaders : "");
    if (headerLen > 0) SendAll(fd, header, static_cast<size_t>(headerLen));
    if (bodySize) SendAll(fd, body, bodySize);
}

std::string ReadRequestLine(int fd) {
    std::string buffer;
    while (buffer.size() < 8192) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;
        buffer += c;
        if (buffer.size() >= 4 && buffer.compare(buffer.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    size_t lineEnd = buffer.find("\r\n");
    return lineEnd == std::string::npos ? buffer : buffer.substr(0, lineEnd);
}

constexpr const char kFallbackIndexHtml[] =
    "<!DOCTYPE html><html><head><title>Mii Transfer</title></head>"
    "<body><h1>Mii Transfer is running</h1>"
    "<p><a href=\"/data/CFL_DB.dat\">Download CFL_DB.dat</a></p>"
    "</body></html>";

void HandleConnection(int fd) {
    std::string requestLine = ReadRequestLine(fd);

    size_t firstSpace = requestLine.find(' ');
    size_t secondSpace =
        firstSpace == std::string::npos ? std::string::npos : requestLine.find(' ', firstSpace + 1);
    std::string method = firstSpace == std::string::npos ? "" : requestLine.substr(0, firstSpace);
    std::string path = (firstSpace == std::string::npos || secondSpace == std::string::npos)
                            ? "/"
                            : requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    if (!method.empty() && method != "GET" && method != "HEAD") {
        static const char body[] = "Method Not Allowed";
        SendResponse(fd, 405, "Method Not Allowed", "text/plain", nullptr, body, sizeof(body) - 1);
        return;
    }

    if (path == "/" || path == "/index.html") {
        std::vector<uint8_t> html;
        if (!ReadFileFully("romfs:/index.html", &html)) {
            html.assign(kFallbackIndexHtml, kFallbackIndexHtml + sizeof(kFallbackIndexHtml) - 1);
        }
        SendResponse(fd, 200, "OK", "text/html", nullptr, html.data(), html.size());
    } else if (path == "/mii-parser.js") {
        std::vector<uint8_t> js;
        if (ReadFileFully("romfs:/mii-parser.js", &js)) {
            SendResponse(fd, 200, "OK", "application/javascript", nullptr, js.data(), js.size());
        } else {
            static const char body[] = "Could not read mii-parser.js";
            SendResponse(fd, 404, "Not Found", "text/plain", nullptr, body, sizeof(body) - 1);
        }
    } else if (path == "/bg.png") {
        std::vector<uint8_t> png;
        if (ReadFileFully("romfs:/bg.png", &png)) {
            SendResponse(fd, 200, "OK", "image/png", nullptr, png.data(), png.size());
        } else {
            static const char body[] = "Could not read bg.png";
            SendResponse(fd, 404, "Not Found", "text/plain", nullptr, body, sizeof(body) - 1);
        }
    } else if (path == "/data/CFL_DB.dat" || path == "/data/fp_db.dat") {
        std::vector<uint8_t> data;
        const char *fileName;
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            if (path == "/data/CFL_DB.dat") {
                data = g_cflDbData;
                fileName = "CFL_DB.dat";
            } else {
                data = g_friendMiiData;
                fileName = "fp_db.dat";
            }
        }
        if (!data.empty()) {
            std::string disposition = "Content-Disposition: attachment; filename=\"" + std::string(fileName) + "\"\r\n";
            SendResponse(fd, 200, "OK", "application/octet-stream", disposition.c_str(), data.data(), data.size());
        } else {
            static const char body[] = "No data available yet";
            SendResponse(fd, 404, "Not Found", "text/plain", nullptr, body, sizeof(body) - 1);
        }
    } else {
        static const char body[] = "Not Found";
        SendResponse(fd, 404, "Not Found", "text/plain", nullptr, body, sizeof(body) - 1);
    }
}

void Fail(const std::string &error) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_error = error;
    g_state = State::Failed;
}

// Runs entirely on the background thread Start() spawns: socInit() through
// listen(), then (on success) falls straight into the accept loop without
// ever handing control back to the caller - see Start()'s own comment for
// why none of this may run on the caller's thread.
void SetupAndServe(void *portArg) {
    uint16_t port = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(portArg));
    CtrLog::Printf("MiiHttpServer::SetupAndServe: thread entered, waiting on CtrNetwork::WaitUntilDone()");

    // SOC is initialized centrally in main.cpp now (shared with CtrLog's UDP
    // debug logger, which needs it up before anything else even starts) -
    // this used to call socInit() itself here, but two socInit() calls in
    // the same process don't coexist.

    // gethostid() needs a real assigned IP, which needs the AC connection
    // CtrNetwork::BeginConnect() started to have actually come up first -
    // waiting here (this background thread, never the caller's) is what
    // makes that safe to assume below.
    if (!CtrNetwork::WaitUntilDone()) {
        CtrLog::Printf("MiiHttpServer::SetupAndServe: CtrNetwork::WaitUntilDone() returned false");
        Fail("No network connection available.");
        return;
    }
    CtrLog::Printf("MiiHttpServer::SetupAndServe: CtrNetwork::WaitUntilDone() returned true, calling gethostid()");

    long hostId = gethostid();
    CtrLog::Printf("MiiHttpServer::SetupAndServe: gethostid() returned %ld", hostId);
    g_assignedIp = static_cast<uint32_t>(hostId);
    if (g_assignedIp == 0) {
        Fail("Could not determine this console's IP address.");
        return;
    }

    CtrLog::Printf("MiiHttpServer::SetupAndServe: calling socket()");
    g_listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    CtrLog::Printf("MiiHttpServer::SetupAndServe: socket() returned %d", g_listenSocket);
    if (g_listenSocket < 0) {
        Fail("socket() failed.");
        return;
    }

    int reuse = 1;
    setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    CtrLog::Printf("MiiHttpServer::SetupAndServe: calling bind()");
    g_boundPort = 0;
    for (int attempt = 0; attempt < kMaxPortAttempts; attempt++) {
        uint16_t tryPort = static_cast<uint16_t>(port + attempt);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(tryPort);
        if (bind(g_listenSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
            g_boundPort = tryPort;
            break;
        }
    }
    CtrLog::Printf("MiiHttpServer::SetupAndServe: bind() loop done, boundPort=%u", g_boundPort);
    if (g_boundPort == 0) {
        Fail("bind() failed on every port tried.");
        close(g_listenSocket);
        g_listenSocket = -1;
        return;
    }

    CtrLog::Printf("MiiHttpServer::SetupAndServe: calling listen()");
    if (listen(g_listenSocket, 5) != 0) {
        CtrLog::Printf("MiiHttpServer::SetupAndServe: listen() failed");
        Fail("listen() failed.");
        close(g_listenSocket);
        g_listenSocket = -1;
        g_boundPort = 0;
        return;
    }
    CtrLog::Printf("MiiHttpServer::SetupAndServe: listen() succeeded, entering accept loop");

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state = State::Ready;
    }

    while (!g_stopRequested) {
        // Polled with a short timeout rather than calling accept() directly
        // and blocking indefinitely: libctru's soc:u BSD socket shim doesn't
        // reliably unblock a thread already parked in accept() just because
        // another thread closed the fd (see Stop()'s own comment) - the
        // interrupt has to round-trip through the SOC service rather than
        // being a real kernel wakeup, so a closed-during-accept() thread can
        // sit there for a noticeable stretch. Re-checking g_stopRequested
        // every 200ms instead keeps Stop()'s threadJoin() from stalling the
        // main thread (and therefore rendering) for more than that long.
        pollfd pfd{};
        pfd.fd = g_listenSocket;
        pfd.events = POLLIN;
        int pollResult = poll(&pfd, 1, 200);
        if (pollResult <= 0) continue; // timeout or error - loop back and re-check g_stopRequested
        if (g_stopRequested) break;

        sockaddr_in clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);
        int clientFd = accept(g_listenSocket, reinterpret_cast<sockaddr *>(&clientAddr), &clientAddrLen);
        if (clientFd < 0) {
            if (g_stopRequested) break;
            continue;
        }
        HandleConnection(clientFd);
        close(clientFd);
    }
}

} // namespace

void Start(uint16_t port) {
    g_stopRequested = false;
    g_assignedIp = 0;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state = State::Starting;
        g_error.clear();
    }
    g_thread = CtrThread::SpawnJoinable(SetupAndServe, reinterpret_cast<void *>(static_cast<uintptr_t>(port)));
}

void Stop() {
    if (!g_thread) return;

    // Just the flag here - no shutdown()/close() on g_listenSocket yet. The
    // accept loop's poll() now has its own 200ms timeout (see
    // SetupAndServe()) and will notice g_stopRequested and exit on its own
    // within that window, so forcing the socket closed to unblock it isn't
    // needed anymore. Closing it here, before threadJoin(), used to race
    // with the background thread possibly still being mid-poll()/accept()
    // on that exact fd - on 3DS, every socket call is an IPC round-trip to
    // the soc:u sysmodule rather than a purely local kernel op, so a close()
    // arriving mid-call there could leave that socket's IPC session wedged
    // instead of just erroring out cleanly - manifesting as a hang on the
    // *next* Start()/Stop() cycle rather than the current one.
    g_stopRequested = true;
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = nullptr;

    // Safe to close now - the background thread that owned this fd has
    // already exited.
    if (g_listenSocket >= 0) {
        close(g_listenSocket);
        g_listenSocket = -1;
    }
    g_boundPort = 0;
}

State GetState() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_state;
}

std::string GetError() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_error;
}

void SetCflDbData(std::vector<uint8_t> rawDbBytes) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    g_cflDbData = std::move(rawDbBytes);
}

void SetFriendMiiData(std::vector<uint8_t> rawStoreData) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    g_friendMiiData = std::move(rawStoreData);
}

std::string GetLocalIpString() {
    if (g_assignedIp == 0) return "";
    char buf[16];
    // gethostid() (in SetupAndServe(), where g_assignedIp is set) hands back
    // the address in host byte order (little-endian on the 3DS's ARM11),
    // not network byte order - so the first octet is the *low* byte, not
    // the high one. Formatting high-to-low here (as if it were a
    // network-order value straight off the wire) prints every console's
    // real address fully reversed, e.g. an actual 192.168.100.13 comes out
    // "13.100.168.192".
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", static_cast<unsigned>(g_assignedIp) & 0xFFu,
                  static_cast<unsigned>(g_assignedIp >> 8) & 0xFFu, static_cast<unsigned>(g_assignedIp >> 16) & 0xFFu,
                  static_cast<unsigned>(g_assignedIp >> 24) & 0xFFu);
    return buf;
}

uint16_t GetPort() {
    return g_boundPort;
}

} // namespace MiiHttpServer
