#pragma once

namespace polyx::core
{
// ---------------------------------------------------------------------------
// Foundational constants / project-wide rules.
//
// Stable, project-level conventions that almost never change between runs.
// Centralized here as the single source of truth so the values are not
// scattered as magic numbers. Changing one is changing a global rule.
// (Keep this file ASCII: MSVC compiles sources as the system code page, so
//  non-ASCII comments can corrupt following declarations without /utf-8.)
// ---------------------------------------------------------------------------

// Texture block size, in pixels. The art team authors textures on an 8-pixel
// grid, so UV-region quantization and atlas tile sizing both use this as the
// base unit. Changing it affects all quantization granularity and tile
// splitting; under normal circumstances it should not be changed.
inline constexpr int kTextureBlockSize = 8;

// Gutter (padding) between atlas tiles, in pixels. This project uses a polygon
// (flat-shaded / low-poly) art style and does NOT use mipmapping, so tiles are
// packed edge-to-edge with no bleeding -- zero gutter is an intentional rule,
// not an oversight. If mipmapping is ever introduced, revisit this and extend
// the packing / UV half-texel inset logic accordingly.
inline constexpr int kAtlasGutter = 0;
} // namespace polyx::core
