#pragma once
#include <string>
#include <cstdint>

namespace nmea {

struct Fix {
    double lat_deg;    // decimal degrees, positive = North
    double lon_deg;    // decimal degrees, positive = East
    float  alt_msl_m;  // altitude above mean sea level, metres
    float  course_deg; // true course, degrees from north (from RMC)
    float  speed_mps;  // speed over ground, metres per second (from RMC)
    bool   valid;      // false if fix quality 0 (GGA) or status V (RMC)
};

// Parse a $GPGGA or $GNGGA sentence into fix.
// Returns true and populates fix on success. Returns false if sentence is
// not a GGA type, has bad checksum, or has fix quality 0.
bool parseGGA(const std::string& sentence, Fix& fix);

// Parse a $GPRMC or $GNRMC sentence into fix.
// Returns true and populates fix on success. Returns false if sentence is
// not an RMC type, has bad checksum, or status is V (void).
bool parseRMC(const std::string& sentence, Fix& fix);

// Dispatch to parseGGA or parseRMC based on sentence type.
// Returns false if neither type matches or parsing fails.
bool parse(const std::string& sentence, Fix& fix);

// Compute XOR checksum of characters between '$' and '*' (exclusive).
// Returns 0 if delimiters not found.
uint8_t computeChecksum(const std::string& sentence);

} // namespace nmea
