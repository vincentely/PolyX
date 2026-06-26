#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "atlas/AtlasBuilder.h"
#include "core/Logger.h"

namespace fbxsdk
{
class FbxScene;
class FbxNode;
class FbxMesh;
}

namespace polyx::uv
{
struct UvPoint
{
    double u = 0.0;
    double v = 0.0;
};

struct RegionMapping
{
    std::string tileKey;
    int originX = 0;
    int originY = 0;
    int width = 0;
    int height = 0;
    int textureWidth = 0;  // dims of the source texture this region was sampled from
    int textureHeight = 0; // (per material slot / submesh)
};

struct MeshPlan
{
    std::vector<RegionMapping> regions;
    std::vector<int> polyToRegion;
};

struct TileCandidate
{
    std::string key;
    atlas::Rect sourceRect;
    atlas::Image image;
};

struct ScenePlan
{
    std::vector<MeshPlan> meshes;
    std::vector<TileCandidate> uniqueTiles;
    std::string primaryUvSetName;
    std::size_t triangleCount = 0;
};

class UVAnalyzer
{
public:
    // meshMaterialTextures[i][m] is the source texture for the i-th mesh's
    // material slot m (submesh m), or nullptr/empty to skip that slot. A mesh
    // with no usable slot texture is left unprocessed.
    ScenePlan AnalyzeScene(fbxsdk::FbxScene* scene,
                           const std::vector<std::vector<const atlas::Image*>>& meshMaterialTextures,
                           core::Logger& logger) const;
};
} // namespace polyx::uv
