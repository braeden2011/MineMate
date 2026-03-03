#pragma once
// IMPORTANT: This header must NEVER be included outside src/gps/ or src/crs/.
// All coordinate translation stays internal to those two directories.

namespace crs {

enum class Datum { GDA94, GDA2020 };

struct MgaPoint {
    double easting;   // metres
    double northing;  // metres
    double elev;      // metres above ellipsoid
};

struct WgsPoint {
    double lat_deg;   // decimal degrees, positive = North
    double lon_deg;   // decimal degrees, positive = East
    double alt_msl_m; // metres above mean sea level
};

// Transform WGS84 geodetic coordinates to GDA94 or GDA2020 MGA Zone N.
// zone: MGA zone number (e.g. 55).
//   GDA94  → EPSG:28355   GDA2020 → EPSG:7855
// Uses PROJ 9 C API internally.
MgaPoint wgs84ToMga(double lat_deg, double lon_deg, double alt_msl_m,
                    int zone, Datum datum = Datum::GDA94);

// Inverse: MGA Zone N → WGS84 geodetic.
WgsPoint mgaToWgs84(double easting, double northing, double elev_m,
                    int zone, Datum datum = Datum::GDA94);

} // namespace crs
