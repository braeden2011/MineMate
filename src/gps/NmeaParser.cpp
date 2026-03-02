#include "NmeaParser.h"
#include <cmath>
#include <cstdlib>
#include <vector>
#include <sstream>

namespace nmea {

// ---- helpers ----------------------------------------------------------------

static uint8_t xorChecksum(const std::string& s)
{
    const auto start = s.find('$');
    const auto star  = s.find('*');
    if (start == std::string::npos || star == std::string::npos || star <= start)
        return 0;
    uint8_t cs = 0;
    for (size_t i = start + 1; i < star; ++i)
        cs ^= static_cast<uint8_t>(s[i]);
    return cs;
}

static bool validateChecksum(const std::string& sentence)
{
    const auto star = sentence.find('*');
    if (star == std::string::npos || star + 2 >= sentence.size())
        return false;
    const uint8_t expected = xorChecksum(sentence);
    const uint8_t given    = static_cast<uint8_t>(
        std::strtol(sentence.c_str() + star + 1, nullptr, 16));
    return expected == given;
}

static std::vector<std::string> splitComma(const std::string& s)
{
    std::vector<std::string> fields;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
        fields.push_back(tok);
    return fields;
}

// Convert NMEA DDmm.mmmm format to decimal degrees.
// dir: 'N'/'S' for latitude, 'E'/'W' for longitude.
static double ddmmToDecimal(const std::string& value, char dir)
{
    if (value.empty()) return 0.0;
    const double raw     = std::stod(value);
    const int    degrees = static_cast<int>(raw / 100.0);
    const double minutes = raw - degrees * 100.0;
    double decimal = degrees + minutes / 60.0;
    if (dir == 'S' || dir == 'W') decimal = -decimal;
    return decimal;
}

// ---- public -----------------------------------------------------------------

uint8_t computeChecksum(const std::string& sentence)
{
    return xorChecksum(sentence);
}

bool parseGGA(const std::string& sentence, Fix& fix)
{
    // Type check: $GPGGA or $GNGGA
    if (sentence.size() < 6) return false;
    const auto comma = sentence.find(',');
    if (comma == std::string::npos) return false;
    const std::string tag = sentence.substr(1, comma - 1); // strip leading $
    if (tag != "GPGGA" && tag != "GNGGA") return false;

    if (!validateChecksum(sentence)) return false;

    // Strip trailing *XX and CR/LF, then split
    const auto star = sentence.find('*');
    const std::string body = (star != std::string::npos)
        ? sentence.substr(0, star) : sentence;
    const auto fields = splitComma(body);

    // GGA fields: 0=tag,1=time,2=lat,3=N/S,4=lon,5=E/W,6=quality,7=sats,
    //             8=hdop,9=alt,10=M,11=geoid,12=M,13=dgps age,14=dgps id
    if (fields.size() < 10) return false;

    const int quality = fields[6].empty() ? 0 : std::stoi(fields[6]);
    if (quality == 0) { fix.valid = false; return false; }

    try {
        fix.lat_deg   = ddmmToDecimal(fields[2], fields[3].empty() ? 'N' : fields[3][0]);
        fix.lon_deg   = ddmmToDecimal(fields[4], fields[5].empty() ? 'E' : fields[5][0]);
        fix.alt_msl_m = fields[9].empty() ? 0.f : static_cast<float>(std::stod(fields[9]));
    } catch (...) { return false; }

    // GGA has no course/speed — leave at zero
    fix.course_deg = 0.f;
    fix.speed_mps  = 0.f;
    fix.valid      = true;
    return true;
}

bool parseRMC(const std::string& sentence, Fix& fix)
{
    // Type check: $GPRMC or $GNRMC
    if (sentence.size() < 6) return false;
    const auto comma = sentence.find(',');
    if (comma == std::string::npos) return false;
    const std::string tag = sentence.substr(1, comma - 1);
    if (tag != "GPRMC" && tag != "GNRMC") return false;

    if (!validateChecksum(sentence)) return false;

    const auto star = sentence.find('*');
    const std::string body = (star != std::string::npos)
        ? sentence.substr(0, star) : sentence;
    const auto fields = splitComma(body);

    // RMC fields: 0=tag,1=time,2=status,3=lat,4=N/S,5=lon,6=E/W,
    //             7=speed(knots),8=course,9=date,10=mag var,11=E/W
    if (fields.size() < 9) return false;

    if (fields[2].empty() || fields[2][0] != 'A') {
        fix.valid = false;
        return false;
    }

    try {
        fix.lat_deg    = ddmmToDecimal(fields[3], fields[4].empty() ? 'N' : fields[4][0]);
        fix.lon_deg    = ddmmToDecimal(fields[5], fields[6].empty() ? 'E' : fields[6][0]);
        const double knots = fields[7].empty() ? 0.0 : std::stod(fields[7]);
        fix.speed_mps  = static_cast<float>(knots * 0.514444);
        fix.course_deg = fields[8].empty() ? 0.f
                       : static_cast<float>(std::stod(fields[8]));
        fix.alt_msl_m  = 0.f; // RMC has no altitude
    } catch (...) { return false; }

    fix.valid = true;
    return true;
}

bool parse(const std::string& sentence, Fix& fix)
{
    if (parseGGA(sentence, fix)) return true;
    if (parseRMC(sentence, fix)) return true;
    return false;
}

} // namespace nmea
