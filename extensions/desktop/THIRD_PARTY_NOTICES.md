# Third-Party Notices (`extensions/desktop`)

The Liquid Glass optical shader and parameter formulas in
`host/render/shaders/liquid_glass_optics.hlsli`,
`host/render/shaders/liquid_glass.hlsl`, and
`host/render/liquid_glass_params.h` were written from scratch in HLSL/C++20 for
Direct2D 1.1 (`ps_4_0`), referencing mathematical formulations from the
following open-source projects:

---

## 1. Kyant0/AndroidLiquidGlass (Apache License 2.0)

- Repository: https://github.com/Kyant0/AndroidLiquidGlass
- Copyright (c) Kyant0
- Referenced concepts: rounded-rectangle signed distance field (`sdRoundRect`),
  circular meniscus surface profile, Snell's law normal refraction factor,
  dispersive RGB channel index offsets, and dual-lobe directional glare geometry.
- Licensed under the Apache License, Version 2.0 (the "License"); you may not
  use the referenced concepts except in compliance with the License. You may
  obtain a copy of the License at:
  http://www.apache.org/licenses/LICENSE-2.0

---

## 2. Liquid Glass Studio (MIT License)

Reference source supplied locally: `liquid-glass-studio-main`.
Ported defaults from `src/Controls.tsx`; STEP 9 Snell refraction, spectral
sampling, Fresnel/glare/LCH formulas from `src/shaders/fragment-main.glsl`
and `src/shaders/lib/color.glsl`; radius/sigma convention from `src/utils/index.ts`.
The color helper retains the upstream attribution to Rachmanin0xFF /
GLSL-Color-Functions (as recorded in the reference color.glsl).
Pulse adaptations use Direct2D screen-pixel/Y-down coordinates, retain existing
card shape/fusion, add persistent continuous controls and finite-input checks.
Default radius 1 uses the reference normalized truncated Gaussian kernel;
other radii use Direct2D Gaussian with sigma=radius/3, not an identical finite kernel.

MIT License

Copyright (c) 2024 Charles Yin

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
