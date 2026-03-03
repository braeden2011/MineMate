#include "CoordTransform.h"
#include <proj.h>
#include <stdexcept>
#include <string>

namespace crs {

// Returns the PROJ EPSG string for the given zone and datum.
//   GDA94  MGA Zone N: EPSG:28300+N   (e.g. zone 55 → EPSG:28355)
//   GDA2020 MGA Zone N: EPSG:7800+N   (e.g. zone 55 → EPSG:7855)
static std::string epsgForZone(int zone, Datum datum)
{
    if (datum == Datum::GDA94)
        return "EPSG:" + std::to_string(28300 + zone);
    else
        return "EPSG:" + std::to_string(7800 + zone);
}

// Shared helper: create a PROJ normalised transform between two EPSG CRS strings.
// Caller is responsible for calling proj_destroy(P) and proj_context_destroy(ctx).
static PJ* makeProjTransform(const char* srcEpsg, const char* dstEpsg,
                              PJ_CONTEXT*& ctxOut)
{
    ctxOut = proj_context_create();
    if (!ctxOut)
        throw std::runtime_error("CoordTransform: proj_context_create failed");

    PJ* P = proj_create_crs_to_crs(ctxOut, srcEpsg, dstEpsg, nullptr);
    if (!P) {
        proj_context_destroy(ctxOut);
        ctxOut = nullptr;
        throw std::runtime_error(
            std::string("CoordTransform: proj_create_crs_to_crs failed (")
            + srcEpsg + " → " + dstEpsg + ")");
    }

    PJ* Pn = proj_normalize_for_visualization(ctxOut, P);
    proj_destroy(P);
    if (!Pn) {
        proj_context_destroy(ctxOut);
        ctxOut = nullptr;
        throw std::runtime_error("CoordTransform: proj_normalize_for_visualization failed");
    }
    return Pn;
}

MgaPoint wgs84ToMga(double lat_deg, double lon_deg, double alt_msl_m,
                    int zone, Datum datum)
{
    const std::string dstEpsg = epsgForZone(zone, datum);
    PJ_CONTEXT* ctx = nullptr;
    PJ* Pn = makeProjTransform("EPSG:4326", dstEpsg.c_str(), ctx);

    // After normalize_for_visualization: WGS84 expects (lon, lat), MGA outputs (E, N).
    PJ_COORD in  = proj_coord(lon_deg, lat_deg, alt_msl_m, 0.0);
    PJ_COORD out = proj_trans(Pn, PJ_FWD, in);
    proj_destroy(Pn);
    proj_context_destroy(ctx);

    if (out.xyzt.x == HUGE_VAL || out.xyzt.y == HUGE_VAL)
        throw std::runtime_error("CoordTransform: wgs84ToMga returned HUGE_VAL — bad input?");

    return MgaPoint{ out.xyzt.x, out.xyzt.y, out.xyzt.z };
}

WgsPoint mgaToWgs84(double easting, double northing, double elev_m,
                    int zone, Datum datum)
{
    const std::string srcEpsg = epsgForZone(zone, datum);
    PJ_CONTEXT* ctx = nullptr;
    PJ* Pn = makeProjTransform(srcEpsg.c_str(), "EPSG:4326", ctx);

    // After normalize_for_visualization: MGA input is (E, N), WGS84 output is (lon, lat).
    PJ_COORD in  = proj_coord(easting, northing, elev_m, 0.0);
    PJ_COORD out = proj_trans(Pn, PJ_FWD, in);
    proj_destroy(Pn);
    proj_context_destroy(ctx);

    if (out.xyzt.x == HUGE_VAL || out.xyzt.y == HUGE_VAL)
        throw std::runtime_error("CoordTransform: mgaToWgs84 returned HUGE_VAL — bad input?");

    // out.xyzt.x = lon, out.xyzt.y = lat
    return WgsPoint{ out.xyzt.y, out.xyzt.x, out.xyzt.z };
}

} // namespace crs
