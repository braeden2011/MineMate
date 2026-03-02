#pragma once

namespace gps {

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
