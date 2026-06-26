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
    // meshTextures[i] is the source texture for the i-th mesh (scene traversal
    // order), or nullptr/empty to leave that mesh unprocessed.
    ScenePlan AnalyzeScene(fbxsdk::FbxScene* scene,
                           const std::vector<const atlas::Image*>& meshTextures,
                           core::Logger& logger) const;
};
} // namespace polyx::uv
