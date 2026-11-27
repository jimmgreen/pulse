# Third-Party Notices (`extensions/desktop`)

The unified macOS Tahoe-style Liquid Glass single-pass pixel shader and renderer
in `host/render/shaders/tile_liquid_glass.hlsl` and `host/render/tile_liquid_glass.h/.cpp`
were written from scratch in HLSL/C++20 for Direct2D 1.1 (`ps_4_0`), referencing
convex superellipse bezel Snell refraction, RGB prism dispersion, Apple Vibrancy
saturation, and SDF directional specular rim concepts inspired by WacOS / macOS Tahoe
Liquid Glass.
