#include "CoordTransform.h"
#include <proj.h>
#include <stdexcept>
#include <string>

namespace crs {

MgaPoint wgs84ToMga(double lat_deg, double lon_deg, double alt_msl_m, int zone)
{
    // GDA94 MGA Zone N = EPSG:28300 + zone  (e.g. zone 55 → EPSG:28355)
    const std::string targetEpsg = "EPSG:" + std::to_string(28300 + zone);

    PJ_CONTEXT* ctx = proj_context_create();
    if (!ctx)
        throw std::runtime_error("CoordTransform: proj_context_create failed");

    PJ* P = proj_create_crs_to_crs(ctx,
                                    "EPSG:4326",
                                    targetEpsg.c_str(),
                                    nullptr);
    if (!P) {
        proj_context_destroy(ctx);
        throw std::runtime_error("CoordTransform: proj_create_crs_to_crs failed for "
                                 + targetEpsg);
    }

    // Normalise axis order so input is always (lon, lat, alt) regardless of EPSG convention.
    PJ* Pn = proj_normalize_for_visualization(ctx, P);
    proj_destroy(P);
    if (!Pn) {
        proj_context_destroy(ctx);
        throw std::runtime_error("CoordTransform: proj_normalize_for_visualization failed");
    }

    PJ_COORD in = proj_coord(lon_deg, lat_deg, alt_msl_m, 0.0);
    PJ_COORD out = proj_trans(Pn, PJ_FWD, in);

    proj_destroy(Pn);
    proj_context_destroy(ctx);

    if (out.xyzt.x == HUGE_VAL || out.xyzt.y == HUGE_VAL)
        throw std::runtime_error("CoordTransform: proj_trans returned HUGE_VAL — bad input?");

    return MgaPoint{ out.xyzt.x, out.xyzt.y, out.xyzt.z };
}

} // namespace crs
