# Architecture Decision Records

Entries are added here as significant architectural decisions are made during development.

---

## ADR-001 — Index buffers are uint32

**Date:** 2026-03-01
**Status:** Accepted

**Decision:** All GPU index buffers use `uint32_t` (DXGI_FORMAT_R32_UINT).
**Reason:** Target geometry is up to 5 million triangles per surface — well beyond the 65,535 vertex limit of uint16.
This decision is non-negotiable and must never be reversed.

---

## ADR-002 — GPU vertex positions are origin-offset

**Date:** 2026-03-01
**Status:** Accepted

**Decision:** Raw MGA easting/northing coordinates (e.g. 436 589 m East) are never written directly to GPU vertex buffers. The mesh origin (e.g. `$EXTMIN` from the DXF) is subtracted before upload so that all vertex positions are relative to a local origin near (0, 0, 0).
**Reason:** Single-precision float has ~7 decimal digits of precision. At MGA scale, coordinates exceed 400 000 m; direct storage would produce precision gaps of ~0.03 m at the survey accuracy required.

---

## ADR-003 — DXF parser is a standalone static library

**Date:** 2026-03-01
**Status:** Accepted

**Decision:** `dxf_parser/` compiles as a static library with no dependency on DX11 headers.
**Reason:** Keeps parsing logic independently testable and reusable; prevents accidental coupling between parsing and rendering code.

---

## ADR-004 — Dear ImGui vendored at v1.91.6

**Date:** 2026-03-01
**Status:** Accepted

**Decision:** ImGui source is copied directly to `third_party/imgui/` rather than managed via vcpkg.
**Reason:** imgui's vcpkg port does not include the backend implementations (imgui_impl_win32 / imgui_impl_dx11) in a convenient form. Vendoring gives full control over which backends are compiled.
