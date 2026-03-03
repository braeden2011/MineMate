#pragma once
// Helpers for displaying scene-space coordinates as MGA and WGS84 in the UI.
// main.cpp may include this header; it does NOT expose any crs/ types.
#include "IGpsSource.h"
#include <array>

namespace gps {

struct MgaCoord {
    double easting;   // metres
    double northing;  // metres
    double elev;      // metres
};

struct WgsCoord {
    double lat_deg;   // decimal degrees, positive = North
    double lon_deg;   // decimal degrees, positive = East
    double alt_m;     // metres
};

// Convert scene-space XYZ (origin-offset) to MGA by adding the scene origin.
MgaCoord sceneToMga(float sx, float sy, float sz,
                    const std::array<float,3>& sceneOrigin);

// Convert MGA coordinates to WGS84 using PROJ.
// Throws std::runtime_error on PROJ failure.
WgsCoord mgaToWgs(double easting, double northing, double elev,
                  int zone, Datum datum);

} // namespace gps
