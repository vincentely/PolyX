#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace polyx::uv::detail
{
// Quantize a UV coordinate to the origin (top-left) of its block-aligned cell in
// texture space. The V axis is flipped because textures are addressed top-down.
// `block` is the alignment unit in pixels (see core::kTextureBlockSize).
//
// Kept free of FBX/engine types so it can be unit-tested in isolation.
inline std::pair<int, int> QuantizeToBlockOrigin(double u, double v, int textureWidth, int textureHeight, int block)
{
    if (block <= 0)
    {
        block = 1;
    }

    const int texelX = std::clamp(static_cast<int>(std::floor(u * static_cast<double>(textureWidth))),
                                  0,
                                  std::max(0, textureWidth - 1));
    const int texelY = std::clamp(static_cast<int>(std::floor((1.0 - v) * static_cast<double>(textureHeight))),
                                  0,
                                  std::max(0, textureHeight - 1));

    return { (texelX / block) * block, (texelY / block) * block };
}

// Disjoint-set union (union by rank + path halving). Used to group polygons that
// share UV vertices into a single region.
class UnionFind
{
public:
    explicit UnionFind(int n)
        : parent_(n)
        , rank_(n, 0)
    {
        for (int i = 0; i < n; ++i)
        {
            parent_[i] = i;
        }
    }

    int Find(int x)
    {
        while (parent_[x] != x)
        {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }

    void Unite(int a, int b)
    {
        a = Find(a);
        b = Find(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
    }

private:
    std::vector<int> parent_;
    std::vector<int> rank_;
};
} // namespace polyx::uv::detail
