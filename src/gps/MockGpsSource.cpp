#include "MockGpsSource.h"
#include "NmeaParser.h"
#include "../crs/CoordTransform.h"
#include "../terrain/Config.h"
#include <chrono>
#include <fstream>
#include <cmath>

namespace gps {

// Fallback NMEA used when no file is available.
// Represents a static position at approx. 33°49.75'S 151°05.50'E (Sydney area).
static const char* k_fallbackNmea[] = {
    "$GPGGA,000000.00,3349.7500,S,15105.5000,E,1,08,0.9,42.3,M,-33.5,M,,*67",
    "$GPRMC,000000.00,A,3349.7500,S,15105.5000,E,0.0,0.0,020326,,,*0D",
    nullptr
};

MockGpsSource::MockGpsSource(const std::filesystem::path& nmeaPath,
                             const std::array<float, 3>&  sceneOrigin,
                             int                          mgaZone,
                             Datum                        datum)
    : m_sceneOrigin(sceneOrigin)
    , m_mgaZone(mgaZone)
    , m_datum(datum)
{
    std::ifstream f(nmeaPath);
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) m_lines.push_back(std::move(line));
        }
    }
    if (m_lines.empty()) {
        for (const char** p = k_fallbackNmea; *p; ++p)
            m_lines.push_back(*p);
    }

    m_running = true;
    m_thread  = std::thread(&MockGpsSource::threadMain, this);
}

MockGpsSource::~MockGpsSource()
{
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

ScenePosition MockGpsSource::poll()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_last;
}

bool MockGpsSource::isConnected()
{
    return m_running.load();
}

void MockGpsSource::threadMain()
{
    size_t idx           = 0;
    float  smoothHeading = 0.f;
    bool   headingInit   = false;

    const crs::Datum crsDatum =
        (m_datum == Datum::GDA2020) ? crs::Datum::GDA2020 : crs::Datum::GDA94;

    while (m_running) {
        const std::string& line = m_lines[idx % m_lines.size()];
        ++idx;

        nmea::Fix fix{};
        if (nmea::parse(line, fix)) {
            try {
                const crs::MgaPoint mga =
                    crs::wgs84ToMga(fix.lat_deg, fix.lon_deg,
                                    static_cast<double>(fix.alt_msl_m),
                                    m_mgaZone, crsDatum);

                const float sx = static_cast<float>(mga.easting  - m_sceneOrigin[0]);
                const float sy = static_cast<float>(mga.northing - m_sceneOrigin[1]);
                const float sz = static_cast<float>(mga.elev     - m_sceneOrigin[2]);

                float heading = fix.course_deg;
                if (!headingInit) {
                    smoothHeading = heading;
                    headingInit   = true;
                } else if (fix.speed_mps >= terrain::GPS_MIN_SPEED_MS) {
                    smoothHeading = smoothHeading
                        + terrain::GPS_HEADING_ALPHA * (heading - smoothHeading);
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

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

} // namespace gps
