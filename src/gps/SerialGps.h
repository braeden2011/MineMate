#pragma once
#include "IGpsSource.h"
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace gps {

// Reads NMEA sentences from a Windows COM port in a background thread.
// Pipeline per sentence: NmeaParser → CoordTransform → subtract sceneOrigin → ScenePosition.
// On read error: closes the port and retries after 5 seconds.
// HANDLE stored as void* to avoid including windows.h in this header.
class SerialGps : public IGpsSource {
public:
    // portName: COM port name, e.g. "COM3" (without \\.\\ prefix — added internally).
    // baudRate: e.g. 9600, 4800, 38400.
    // sceneOrigin: MGA easting/northing/elev of the scene coordinate origin.
    // mgaZone: MGA zone number (e.g. 55).
    // datum: GDA94 or GDA2020.
    SerialGps(const std::string&         portName,
              int                        baudRate,
              const std::array<float,3>& sceneOrigin,
              int                        mgaZone,
              Datum                      datum = Datum::GDA94);

    ~SerialGps() override;

    ScenePosition poll() override;
    bool isConnected() override;

private:
    std::string        m_portName;
    int                m_baudRate;
    std::array<float,3> m_sceneOrigin;
    int                m_mgaZone;
    Datum              m_datum;

    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_connected{false};
    std::thread        m_thread;
    mutable std::mutex m_mutex;
    ScenePosition      m_last{};

    // HANDLE to the open COM port; nullptr = closed.
    // Stored as void* to keep windows.h out of this header.
    void* m_handle = nullptr;

    void threadMain();
    bool openPort();
    void closePort();
};

} // namespace gps
