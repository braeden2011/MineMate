#pragma once
// IMPORTANT: This header must NEVER be included outside src/gps/ or src/crs/.
// All coordinate translation stays internal to those two directories.

namespace crs {

struct MgaPoint {
    double easting;   // metres
    double northing;  // metres
    double elev;      // metres above ellipsoid
};

// Transform WGS84 geodetic coordinates to GDA94 MGA Zone N.
// zone: MGA zone number (e.g. 55 → EPSG:28355).
// Uses PROJ 9 C API internally.
MgaPoint wgs84ToMga(double lat_deg, double lon_deg, double alt_msl_m, int zone);

} // namespace crs
