#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "TcpGps.h"
#include "NmeaParser.h"
#include "../crs/CoordTransform.h"
#include "../terrain/Config.h"
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace gps {

// Verify that our uintptr_t sentinel matches INVALID_SOCKET.
static_assert(~uintptr_t(0) == static_cast<uintptr_t>(INVALID_SOCKET),
              "INVALID_SOCKET sentinel mismatch");

// ── Line accumulator ─────────────────────────────────────────────────────────

static std::vector<std::string> extractLines(std::string& buf,
                                              const char*  data,
                                              int          n)
{
    buf.append(data, static_cast<size_t>(n));
    std::vector<std::string> lines;
    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

// ── Constructor / Destructor ──────────────────────────────────────────────────

TcpGps::TcpGps(const std::string&         host,
                int                        port,
                const std::array<float,3>& sceneOrigin,
                int                        mgaZone,
                Datum                      datum)
    : m_host(host)
    , m_port(port)
    , m_sceneOrigin(sceneOrigin)
    , m_mgaZone(mgaZone)
    , m_datum(datum)
{
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    m_running = true;
    m_thread  = std::thread(&TcpGps::threadMain, this);
}

TcpGps::~TcpGps()
{
    m_running = false;

    // Force-close the socket so recv() unblocks in the thread.
    const uintptr_t s = m_socket.exchange(~uintptr_t(0));
    if (s != ~uintptr_t(0))
        closesocket(static_cast<SOCKET>(s));

    if (m_thread.joinable()) m_thread.join();

    WSACleanup();
}

ScenePosition TcpGps::poll()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_last;
}

bool TcpGps::isConnected()
{
    return m_connected.load();
}

// ── Background thread ─────────────────────────────────────────────────────────

void TcpGps::threadMain()
{
    float  smoothHeading = 0.f;
    bool   headingInit   = false;

    const crs::Datum crsDatum =
        (m_datum == Datum::GDA2020) ? crs::Datum::GDA2020 : crs::Datum::GDA94;

    while (m_running) {
        // ── Connect ──────────────────────────────────────────────────────
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            for (int i = 0; i < 10 && m_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // 500 ms receive timeout so the thread can check m_running.
        DWORD timeoutMs = 500;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

        // Resolve host.
        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string portStr = std::to_string(m_port);
        if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
            closesocket(sock);
            for (int i = 0; i < 10 && m_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        const int connRet = connect(sock, res->ai_addr,
                                    static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);

        if (connRet == SOCKET_ERROR) {
            closesocket(sock);
            for (int i = 0; i < 10 && m_running; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        m_socket.store(static_cast<uintptr_t>(sock));
        m_connected = true;

        // ── Receive loop ─────────────────────────────────────────────────
        std::string lineBuf;
        char rxBuf[512];

        while (m_running) {
            const int n = recv(sock, rxBuf, static_cast<int>(sizeof(rxBuf)), 0);

            if (n <= 0) {
                // Disconnect, error, or socket force-closed by destructor.
                break;
            }

            for (const auto& sentence : extractLines(lineBuf, rxBuf, n)) {
                nmea::Fix fix{};
                if (!nmea::parse(sentence, fix)) continue;

                try {
                    const crs::MgaPoint mga =
                        crs::wgs84ToMga(fix.lat_deg, fix.lon_deg,
                                        static_cast<double>(fix.alt_msl_m),
                                        m_mgaZone, crsDatum);

                    const float sx = static_cast<float>(mga.easting  - m_sceneOrigin[0]);
                    const float sy = static_cast<float>(mga.northing - m_sceneOrigin[1]);
                    const float sz = static_cast<float>(mga.elev     - m_sceneOrigin[2]);

                    if (!headingInit) {
                        smoothHeading = fix.course_deg;
                        headingInit   = true;
                    } else if (fix.speed_mps >= terrain::GPS_MIN_SPEED_MS) {
                        smoothHeading = smoothHeading
                            + terrain::GPS_HEADING_ALPHA * (fix.course_deg - smoothHeading);
                    }

                    ScenePosition pos{};
                    pos.x       = sx;
                    pos.y       = sy;
                    pos.z       = sz;
                    pos.heading = smoothHeading;
                    pos.valid   = true;

                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_last = pos;
                } catch (...) {
                    // PROJ error — leave m_last unchanged
                }
            }
        }

        // ── Disconnect ───────────────────────────────────────────────────
        const uintptr_t prev = m_socket.exchange(~uintptr_t(0));
        if (prev != ~uintptr_t(0))
            closesocket(static_cast<SOCKET>(prev));
        m_connected = false;

        if (!m_running) break;

        // Wait 5 s before reconnecting.
        for (int i = 0; i < 10 && m_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

} // namespace gps
