#pragma once
#include "IGpsSource.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace gps {

// Reads NMEA sentences from a TCP host:port in a background thread.
// Pipeline per sentence: NmeaParser → CoordTransform → subtract sceneOrigin → ScenePosition.
// On disconnect / connect error: waits 5 seconds then reconnects.
// SOCKET stored as uintptr_t to avoid including winsock2.h in this header.
class TcpGps : public IGpsSource {
public:
    // host: IP address or hostname, e.g. "192.168.1.10".
    // port: TCP port, e.g. 4001 (common for NMEA-over-TCP devices).
    // sceneOrigin: MGA easting/northing/elev of the scene coordinate origin.
    // mgaZone: MGA zone number (e.g. 55).
    // datum: GDA94 or GDA2020.
    TcpGps(const std::string&         host,
           int                        port,
           const std::array<float,3>& sceneOrigin,
           int                        mgaZone,
           Datum                      datum = Datum::GDA94);

    ~TcpGps() override;

    ScenePosition poll() override;
    bool isConnected() override;

private:
    std::string         m_host;
    int                 m_port;
    std::array<float,3> m_sceneOrigin;
    int                 m_mgaZone;
    Datum               m_datum;

    std::atomic<bool>   m_running{false};
    std::atomic<bool>   m_connected{false};
    std::thread         m_thread;
    mutable std::mutex  m_mutex;
    ScenePosition       m_last{};

    // SOCKET stored as uintptr_t (~0 = INVALID_SOCKET) to keep winsock2.h out of header.
    std::atomic<uintptr_t> m_socket{~uintptr_t(0)};

    void threadMain();
};

} // namespace gps
