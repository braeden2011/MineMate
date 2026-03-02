#pragma once
// src/app/Camera.h
// Spherical orbit camera — Z-up, right-handed world space.
//
// Coordinate conventions:
//   azimuth  : horizontal angle from +X axis, degrees, CCW when viewed from +Z, wraps freely.
//   elevation: angle above the XY plane, degrees, clamped to [-5, +89].
//   radius   : eye-to-pivot distance in scene metres, clamped to [1, 100000].
//
// Mouse mapping (call from WndProc, skip if ImGui::GetIO().WantCaptureMouse):
//   LMB drag → OrbitDelta(dAzPx, dElPx)
//   RMB/MMB drag → PanDelta(dxPx, dyPx)
//   Scroll wheel → ZoomDelta(notches)   positive = zoom in

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <DirectXMath.h>

class Camera
{
public:
    // ── Initialisation ────────────────────────────────────────────────────

    // Move the orbit pivot point in scene space.
    void SetPivot(float x, float y, float z);

    // Set spherical parameters directly.
    void SetSpherical(float radius, float azimuthDeg, float elevDeg);

    // ── Per-frame delta inputs ────────────────────────────────────────────

    // Orbit: pixel counts; dAzPx > 0 rotates right, dElPx > 0 tilts down.
    void OrbitDelta(float dAzPx, float dElPx);

    // Pan: pixel counts; moves the pivot in camera-right / camera-up directions.
    // Drag right → terrain slides right.  Drag down → terrain slides down.
    void PanDelta(float dxPx, float dyPx);

    // Zoom: notch count (wheel_delta / WHEEL_DELTA); positive = zoom in (smaller radius).
    void ZoomDelta(float notches);

    // ── Per-frame matrix output ───────────────────────────────────────────

    DirectX::XMMATRIX ViewMatrix()          const;
    DirectX::XMMATRIX ProjMatrix(float aspect) const;

    // ── Debug read-back ───────────────────────────────────────────────────

    float                     Radius()    const { return m_radius; }
    float                     Azimuth()   const { return m_azimuth; }
    float                     Elevation() const { return m_elevation; }
    DirectX::XMFLOAT3         Pivot()     const { return m_pivot; }

private:
    DirectX::XMFLOAT3 m_pivot     = {  0.0f,   0.0f,  0.0f };
    float             m_radius    = 360.6f;
    float             m_azimuth   = 270.0f;  // degrees
    float             m_elevation =  33.7f;  // degrees

    static constexpr float kElevMin    =   -5.0f;
    static constexpr float kElevMax    =   89.0f;
    static constexpr float kFovDeg     =   60.0f;
    static constexpr float kNear       =    1.0f;
    static constexpr float kFar        = 20000.0f;
    static constexpr float kOrbitSens  =    0.3f;    // degrees per pixel
    static constexpr float kPanSens    =    0.001f;  // fraction of radius per pixel
    static constexpr float kZoomFactor =    0.85f;   // radius multiplier per notch (< 1)
};
