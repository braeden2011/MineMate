#pragma once

namespace gps {

// Datum for GPS coordinate transformation.
// Mirrors crs::Datum but lives in gps/ so main.cpp never includes crs/CoordTransform.h.
enum class Datum { GDA94, GDA2020 };

struct ScenePosition {
    float x, y, z;   // scene-space, origin-offset applied
    float heading;   // degrees from north, smoothed
    bool  valid;
};

class IGpsSource {
public:
    virtual ScenePosition poll() = 0;
    virtual bool isConnected() = 0;
    virtual ~IGpsSource() = default;
};

} // namespace gps
