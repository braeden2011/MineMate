#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "SerialGps.h"
#include "NmeaParser.h"
#include "../crs/CoordTransform.h"
#include "../terrain/Config.h"
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace gps {

// ── Line accumulator ─────────────────────────────────────────────────────────
// Append raw bytes to buf; extract all complete NMEA lines (terminated by '\n').
static std::vector<std::string> extractLines(std::string& buf,
                                              const char*  data,
                                              DWORD        n)
{
    buf.append(data, n);
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

SerialGps::SerialGps(const std::string&         portName,
                     int                        baudRate,
                     const std::array<float,3>& sceneOrigin,
                     int                        mgaZone,
                     Datum                      datum)
    : m_portName(portName)
    , m_baudRate(baudRate)
    , m_sceneOrigin(sceneOrigin)
    , m_mgaZone(mgaZone)
    , m_datum(datum)
{
    m_running = true;
    m_thread  = std::thread(&SerialGps::threadMain, this);
}

SerialGps::~SerialGps()
{
    m_running = false;
    // Close the handle so ReadFile unblocks immediately.
    closePort();
    if (m_thread.joinable()) m_thread.join();
}

ScenePosition SerialGps::poll()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_last;
}

bool SerialGps::isConnected()
{
    return m_connected.load();
}

// ── Port open/close ───────────────────────────────────────────────────────────

bool SerialGps::openPort()
{
    const std::string fullName = "\\\\.\\" + m_portName;
    HANDLE h = CreateFileA(
        fullName.c_str(),
        GENERIC_READ,
        0,          // exclusive access
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    // Configure baud rate and 8-N-1 framing.
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        return false;
    }
    dcb.BaudRate = static_cast<DWORD>(m_baudRate);
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        return false;
    }

    // 500 ms read timeout so the thread can check m_running between retries.
    COMMTIMEOUTS ct{};
    ct.ReadTotalTimeoutConstant = 500;
    SetCommTimeouts(h, &ct);

    m_handle = static_cast<void*>(h);
    m_connected = true;
    return true;
}

void SerialGps::closePort()
{
    if (m_handle) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle    = nullptr;
        m_connected = false;
    }
}

// ── Background thread ─────────────────────────────────────────────────────────

void SerialGps::threadMain()
{
    float  smoothHeading = 0.f;
    bool   headingInit   = false;
    std::string lineBuf;

    const crs::Datum crsDatum =
        (m_datum == Datum::GDA2020) ? crs::Datum::GDA2020 : crs::Datum::GDA94;

    while (m_running) {
        // ── Connect (or reconnect) ───────────────────────────────────────
        if (!m_handle) {
            if (!openPort()) {
                // Port not available — wait 5 s then retry.
                for (int i = 0; i < 10 && m_running; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            lineBuf.clear();
        }

        // ── Read a chunk ────────────────────────────────────────────────
        char   rxBuf[256];
        DWORD  bytesRead = 0;
        BOOL   ok = ReadFile(static_cast<HANDLE>(m_handle),
                             rxBuf, sizeof(rxBuf), &bytesRead, nullptr);

        if (!ok || (bytesRead == 0 && !m_running)) {
            // Read error or shutdown.
            closePort();
            continue;
        }

        if (bytesRead == 0) continue;  // timeout, no data — loop to check m_running

        // ── Parse complete NMEA lines ────────────────────────────────────
        for (const auto& sentence : extractLines(lineBuf, rxBuf, bytesRead)) {
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

    closePort();
}

} // namespace gps
