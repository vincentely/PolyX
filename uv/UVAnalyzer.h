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

struct TrianglePlan
{
    std::vector<UvPoint> sourceUvs;
    atlas::Rect sourceRect{};
    std::string tileKey;
    bool valid = false;
    std::size_t vertexCount = 0;
};

struct LayerPlan
{
    std::string uvSetName;
    std::vector<TrianglePlan> triangles;
};

struct MeshPlan
{
    std::vector<LayerPlan> layers;
};

struct TileCandidate
{
    std::string key;
    atlas::Rect sourceRect;
    atlas::Image image;
};

struct CellCoord
{
    int x = 0;
    int y = 0;
    bool operator==(const CellCoord& o) const { return x == o.x && y == o.y; }
};

struct CellCoordHash
{
    std::size_t operator()(const CellCoord& c) const
    {
        return std::hash<long long>{}((static_cast<long long>(c.x) << 32) | static_cast<unsigned>(c.y));
    }
};

struct CellMapping
{
    std::string tileKey;
    int tileOriginX = 0;
    int tileOriginY = 0;
    int tileWidth = 8;
    int tileHeight = 8;
};

struct PolyKey
{
    int meshIndex = 0;
    int polyIndex = 0;
    bool operator==(const PolyKey& o) const { return meshIndex == o.meshIndex && polyIndex == o.polyIndex; }
};

struct PolyKeyHash
{
    std::size_t operator()(const PolyKey& k) const
    {
        return std::hash<long long>{}((static_cast<long long>(k.meshIndex) << 32) | static_cast<unsigned>(k.polyIndex));
    }
};

struct PolyStripMapping
{
    std::string tileKey;
    int stripOriginX = 0;
    int stripOriginY = 0;
    int stripWidth = 0;
    int stripHeight = 0;
};

struct ScenePlan
{
    std::vector<MeshPlan> meshes;
    std::vector<TileCandidate> uniqueTiles;
    std::unordered_map<CellCoord, CellMapping, CellCoordHash> cellMap;
    std::unordered_map<PolyKey, PolyStripMapping, PolyKeyHash> polyStripMap;
    std::string primaryUvSetName;
    std::size_t triangleCount = 0;
};

class UVAnalyzer
{
public:
    ScenePlan AnalyzeScene(fbxsdk::FbxScene* scene,
                           const atlas::Image& sourceTexture,
                           core::Logger& logger) const;
};
} // namespace polyx::uv
