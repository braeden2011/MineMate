// src/app/Camera.cpp
#include "app/Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

// ── Initialisation ────────────────────────────────────────────────────────────

void Camera::SetPivot(float x, float y, float z)
{
    m_pivot = {x, y, z};
}

void Camera::SetSpherical(float radius, float azimuthDeg, float elevDeg)
{
    m_radius    = std::max(1.0f, radius);
    m_azimuth   = azimuthDeg;
    m_elevation = std::clamp(elevDeg, kElevMin, kElevMax);
}

// ── Per-frame delta inputs ────────────────────────────────────────────────────

void Camera::OrbitDelta(float dAzPx, float dElPx)
{
    m_azimuth   += dAzPx * kOrbitSens;
    m_elevation -= dElPx * kOrbitSens;   // screen Y inverted relative to world Z
    m_elevation  = std::clamp(m_elevation, kElevMin, kElevMax);
}

void Camera::ZoomDelta(float notches)
{
    // Each notch multiplies radius by kZoomFactor (< 1 = zoom in, > 1 = zoom out).
    const float next  = m_radius * powf(kZoomFactor, notches);
    // Enforce a minimum absolute step so close-range zooming stays responsive.
    constexpr float kMinStep = 1.0f;   // metres
    const float     step     = m_radius - next;  // positive = zoom in
    m_radius = (fabsf(step) >= kMinStep) ? next
             : m_radius - std::copysign(kMinStep, step);
    m_radius  = std::clamp(m_radius, 1.0f, 100000.0f);
}

void Camera::PanDelta(float dxPx, float dyPx)
{
    const float az = XMConvertToRadians(m_azimuth);
    const float el = XMConvertToRadians(m_elevation);

    // Eye-offset direction from pivot (unit vector).
    const XMVECTOR eyeDir = XMVectorSet(
        cosf(el) * cosf(az),
        cosf(el) * sinf(az),
        sinf(el), 0.0f);

    const XMVECTOR lookDir = XMVector3Normalize(XMVectorNegate(eyeDir));
    const XMVECTOR worldUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    // Camera right: horizontal, lies in the XY plane.
    const XMVECTOR right  = XMVector3Normalize(XMVector3Cross(lookDir, worldUp));
    // Camera up in world space (tilted toward camera).
    const XMVECTOR upVec  = XMVector3Normalize(XMVector3Cross(right, lookDir));

    // Scale pan speed proportional to radius so feel is consistent at every zoom level.
    const float scale = m_radius * kPanSens;

    XMVECTOR pivot = XMLoadFloat3(&m_pivot);
    pivot = pivot
        - XMVectorScale(right,  dxPx * scale)   // drag right → pivot left → terrain slides right
        + XMVectorScale(upVec,  dyPx * scale);  // drag down  → pivot up   → terrain slides down
    XMStoreFloat3(&m_pivot, pivot);
}

// ── Per-frame matrix output ───────────────────────────────────────────────────

XMMATRIX Camera::ViewMatrix() const
{
    const float az = XMConvertToRadians(m_azimuth);
    const float el = XMConvertToRadians(m_elevation);

    const XMVECTOR pivot = XMLoadFloat3(&m_pivot);
    const XMVECTOR eye   = pivot + XMVectorScale(
        XMVectorSet(cosf(el) * cosf(az),
                    cosf(el) * sinf(az),
                    sinf(el), 0.0f),
        m_radius);

    const XMVECTOR up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    return XMMatrixLookAtRH(eye, pivot, up);
}

XMFLOAT3 Camera::Position() const
{
    const float az = XMConvertToRadians(m_azimuth);
    const float el = XMConvertToRadians(m_elevation);
    return {
        m_pivot.x + m_radius * cosf(el) * cosf(az),
        m_pivot.y + m_radius * cosf(el) * sinf(az),
        m_pivot.z + m_radius * sinf(el)
    };
}

XMMATRIX Camera::ProjMatrix(float aspect) const
{
    return XMMatrixPerspectiveFovRH(
        XMConvertToRadians(kFovDeg), aspect, kNear, kFar);
}
