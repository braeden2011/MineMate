#pragma once
#include "IGpsSource.h"
#include <array>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gps {

// Replays an NMEA text file at approximately 1 Hz in a background thread.
// Pipeline per line: NmeaParser → CoordTransform → subtract sceneOrigin → ScenePosition.
// Loops at EOF. Falls back to a hardcoded NMEA string if file is unavailable.
// Heading is exponentially-smoothed with GPS_HEADING_ALPHA from Config.h.
class MockGpsSource : public IGpsSource {
public:
    // sceneOrigin: MGA easting/northing/elev of the scene coordinate origin.
    // mgaZone: MGA zone number (e.g. 55).
    // datum: GDA94 or GDA2020.
    MockGpsSource(const std::filesystem::path& nmeaPath,
                  const std::array<float, 3>&  sceneOrigin,
                  int                          mgaZone,
                  Datum                        datum = Datum::GDA94);

    ~MockGpsSource() override;

    ScenePosition poll() override;
    bool isConnected() override;

private:
    std::array<float, 3>      m_sceneOrigin;
    int                       m_mgaZone;
    Datum                     m_datum;
    std::vector<std::string>  m_lines;
    std::atomic<bool>         m_running{ false };
    std::thread               m_thread;
    mutable std::mutex        m_mutex;
    ScenePosition             m_last{};

    void threadMain();
};

} // namespace gps
