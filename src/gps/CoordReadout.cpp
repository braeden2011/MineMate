#include "CoordReadout.h"
#include "../crs/CoordTransform.h"

namespace gps {

MgaCoord sceneToMga(float sx, float sy, float sz,
                    const std::array<float,3>& sceneOrigin)
{
    return MgaCoord{
        static_cast<double>(sx) + static_cast<double>(sceneOrigin[0]),
        static_cast<double>(sy) + static_cast<double>(sceneOrigin[1]),
        static_cast<double>(sz) + static_cast<double>(sceneOrigin[2])
    };
}

WgsCoord mgaToWgs(double easting, double northing, double elev,
                  int zone, Datum datum)
{
    const crs::Datum cd =
        (datum == Datum::GDA2020) ? crs::Datum::GDA2020 : crs::Datum::GDA94;
    const crs::WgsPoint w = crs::mgaToWgs84(easting, northing, elev, zone, cd);
    return WgsCoord{ w.lat_deg, w.lon_deg, w.alt_msl_m };
}

} // namespace gps
