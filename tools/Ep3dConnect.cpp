#include "Ep3dConnect.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#endif

namespace ep3d {

namespace {

// Winsock needs starting and stopping; the BSD sockets do not. Wrapped so the
// two platforms differ in ONE place rather than at every call.
struct SocketLibrary {
    bool ok{true};
    SocketLibrary() {
#ifdef _WIN32
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }
    ~SocketLibrary() {
#ifdef _WIN32
        if (ok) WSACleanup();
#endif
    }
};

void CloseSocket(SocketHandle handle) {
#ifdef _WIN32
    closesocket(handle);
#else
    ::close(handle);
#endif
}

bool SendAll(SocketHandle handle, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const int wrote = static_cast<int>(
            ::send(handle, text.data() + sent, static_cast<int>(text.size() - sent), 0));
        if (wrote <= 0) return false;
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

// Reads until a line that is not a log line -- the verdict. The protocol
// promises exactly one OK or ERR per exchange, which is what lets a client know
// when to stop reading rather than guessing with a timeout.
bool ReadVerdict(SocketHandle handle, std::string& pending, std::string* verdict,
                 std::string* log) {
    for (;;) {
        const std::size_t newline = pending.find('\n');
        if (newline != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind(". ", 0) == 0) {
                *log += line.substr(2) + "\n";
                continue;
            }
            *verdict = line;
            return true;
        }
        char buffer[4096];
        const int got = static_cast<int>(::recv(handle, buffer, sizeof(buffer), 0));
        if (got <= 0) return false;
        pending.append(buffer, static_cast<std::size_t>(got));
    }
}

} // namespace

int SendScript(unsigned short port, const std::string& script, bool quiet) {
    SocketLibrary library;
    if (!library.ok) {
        std::fprintf(stderr, "could not start the socket library\n");
        return 2;
    }

    const SocketHandle handle = ::socket(AF_INET, SOCK_STREAM, 0);
    if (handle == kNoSocket) {
        std::fprintf(stderr, "could not create a socket\n");
        return 2;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    // LOOPBACK, matching the server. There is no host to pass because there is
    // nothing else to connect to.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::fprintf(stderr,
                     "could not connect to 127.0.0.1:%u -- is the viewer running with "
                     "--listen?\n",
                     static_cast<unsigned>(port));
        CloseSocket(handle);
        return 2;
    }

    std::string pending;
    std::string verdict;
    std::string log;
    // The banner the server sends on connect.
    if (!ReadVerdict(handle, pending, &verdict, &log)) {
        std::fprintf(stderr, "the connection closed before the server said anything\n");
        CloseSocket(handle);
        return 2;
    }
    if (!quiet && !verdict.empty()) std::printf("%s\n", verdict.c_str());

    int status = 0;
    std::istringstream lines(script);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!SendAll(handle, line + "\n")) {
            std::fprintf(stderr, "the connection closed while sending\n");
            status = 2;
            break;
        }
        log.clear();
        if (!ReadVerdict(handle, pending, &verdict, &log)) {
            std::fprintf(stderr, "the connection closed while waiting for a reply\n");
            status = 2;
            break;
        }
        if (!quiet && !log.empty()) std::fputs(log.c_str(), stdout);
        if (verdict.rfind("ERR", 0) == 0) {
            std::fflush(stdout);
            std::fprintf(stderr, "%s\n", verdict.c_str());
            // STOPS HERE. The remaining lines are not sent, for the reason the
            // file interpreter stops: what follows a refused command was
            // written for a document that does not exist.
            status = 1;
            break;
        }
    }

    CloseSocket(handle);
    return status;
}

} // namespace ep3d
