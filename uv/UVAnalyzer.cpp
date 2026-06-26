#include "uv/UVAnalyzer.h"

#include "core/Constants.h"
#include "uv/UVRegion.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>

#include <fbxsdk.h>

namespace polyx::uv
{
namespace
{
void CollectMeshes(fbxsdk::FbxNode* node, std::vector<fbxsdk::FbxMesh*>& meshes)
{
    if (node == nullptr)
    {
        return;
    }

    if (fbxsdk::FbxMesh* mesh = node->GetMesh(); mesh != nullptr)
    {
        meshes.push_back(mesh);
    }

    for (int i = 0; i < node->GetChildCount(); ++i)
    {
        CollectMeshes(node->GetChild(i), meshes);
    }
}

std::string BuildTileKey(const atlas::Image& image)
{
    std::string key;
    key.reserve(32U + image.pixels.size());
    key.append(std::to_string(image.width));
    key.push_back('x');
    key.append(std::to_string(image.height));
    key.push_back('|');
    key.append(reinterpret_cast<const char*>(image.pixels.data()), static_cast<std::size_t>(image.pixels.size()));
    return key;
}

} // namespace

ScenePlan UVAnalyzer::AnalyzeScene(fbxsdk::FbxScene* scene,
                                   const std::vector<const atlas::Image*>& meshTextures,
                                   core::Logger& logger) const
{
    ScenePlan result;

    if (scene == nullptr)
    {
        logger.Warn("Scene is null during UV analysis.");
        return result;
    }

    std::vector<fbxsdk::FbxMesh*> meshes;
    CollectMeshes(scene->GetRootNode(), meshes);

    std::unordered_map<std::string, std::size_t> uniqueTileIndex;
    result.meshes.reserve(meshes.size());

    const auto addTile = [&](const atlas::Image& sourceTexture, const atlas::Rect& rect) -> std::string
    {
        const atlas::Image img = atlas::ExtractSubImage(sourceTexture, rect, nullptr);
        if (img.Empty())
        {
            return {};
        }
        const std::string key = BuildTileKey(img);
        if (uniqueTileIndex.find(key) == uniqueTileIndex.end())
        {
            uniqueTileIndex.emplace(key, result.uniqueTiles.size());
            result.uniqueTiles.push_back(TileCandidate{ key, rect, img });
        }
        return key;
    };

    int meshIdx = 0;
    for (fbxsdk::FbxMesh* mesh : meshes)
    {
        const atlas::Image* meshTexture =
            (static_cast<std::size_t>(meshIdx) < meshTextures.size()) ? meshTextures[meshIdx] : nullptr;
        if (mesh == nullptr || meshTexture == nullptr || meshTexture->Empty())
        {
            result.meshes.push_back(MeshPlan{}); // unprocessed mesh keeps index alignment
            ++meshIdx;
            continue;
        }
        const atlas::Image& sourceTexture = *meshTexture;

        MeshPlan meshPlan;
        const int uvSetCount = mesh->GetElementUVCount();
        if (uvSetCount <= 0)
        {
            logger.Warn(std::string("Mesh has no UV sets: ") + mesh->GetName());
            result.meshes.push_back(std::move(meshPlan));
            ++meshIdx;
            continue;
        }

        for (int uvSetIndex = 0; uvSetIndex < uvSetCount; ++uvSetIndex)
        {
            const fbxsdk::FbxGeometryElementUV* uvElement = mesh->GetElementUV(uvSetIndex);
            std::string uvSetName = uvElement != nullptr ? uvElement->GetName() : "";
            if (uvSetName.empty())
            {
                uvSetName = "UVSet_" + std::to_string(uvSetIndex);
            }

            if (result.primaryUvSetName.empty())
            {
                result.primaryUvSetName = uvSetName;
            }
        }

        // Region analysis on the first UV set
        {
            const fbxsdk::FbxGeometryElementUV* uvElement = mesh->GetElementUV(0);
            if (uvElement == nullptr)
            {
                result.meshes.push_back(std::move(meshPlan));
                ++meshIdx;
                continue;
            }

            const auto& directArray = uvElement->GetDirectArray();
            const auto& indexArray = uvElement->GetIndexArray();
            const bool indexed = uvElement->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect;
            const int directCount = directArray.GetCount();
            const int polyCount = mesh->GetPolygonCount();

            // Phase 1: Collect dIdx per polygon and build dIdx -> polygon adjacency
            std::vector<std::vector<int>> polyDIdxSets(polyCount);
            std::unordered_map<int, std::vector<int>> dIdxToPolys;

            int pvIdx = 0;
            for (int poly = 0; poly < polyCount; ++poly)
            {
                const int polySize = mesh->GetPolygonSize(poly);
                for (int v = 0; v < polySize; ++v)
                {
                    int dIdx = pvIdx;
                    if (indexed && pvIdx < indexArray.GetCount())
                    {
                        dIdx = indexArray.GetAt(pvIdx);
                    }
                    ++pvIdx;

                    if (dIdx < 0 || dIdx >= directCount) continue;

                    polyDIdxSets[poly].push_back(dIdx);
                    dIdxToPolys[dIdx].push_back(poly);
                }
            }

            // Phase 2: Union-Find to group polygons sharing dIdx
            detail::UnionFind uf(polyCount);
            for (const auto& [dIdx, polys] : dIdxToPolys)
            {
                for (std::size_t i = 1; i < polys.size(); ++i)
                {
                    uf.Unite(polys[0], polys[i]);
                }
            }

            // Phase 3: Group polygons by region root
            std::unordered_map<int, std::vector<int>> regionPolys;
            for (int poly = 0; poly < polyCount; ++poly)
            {
                if (polyDIdxSets[poly].empty()) continue;
                const int root = uf.Find(poly);
                regionPolys[root].push_back(poly);
            }

            // Phase 4: For each region, compute bounding rect and create tile
            meshPlan.polyToRegion.assign(polyCount, -1);

            const int texW = sourceTexture.width;
            const int texH = sourceTexture.height;

            for (const auto& [root, polys] : regionPolys)
            {
                int minCellX = std::numeric_limits<int>::max();
                int minCellY = std::numeric_limits<int>::max();
                int maxCellX = std::numeric_limits<int>::min();
                int maxCellY = std::numeric_limits<int>::min();

                for (int poly : polys)
                {
                    for (int dIdx : polyDIdxSets[poly])
                    {
                        const fbxsdk::FbxVector2 uv = directArray.GetAt(dIdx);
                        const auto [cx, cy] = detail::QuantizeToBlockOrigin(uv[0], uv[1], texW, texH, core::kTextureBlockSize);
                        minCellX = std::min(minCellX, cx);
                        minCellY = std::min(minCellY, cy);
                        maxCellX = std::max(maxCellX, cx);
                        maxCellY = std::max(maxCellY, cy);
                    }
                }

                const int rectW = maxCellX + core::kTextureBlockSize - minCellX;
                const int rectH = maxCellY + core::kTextureBlockSize - minCellY;

                const int clampedX = std::clamp(minCellX, 0, std::max(0, texW - rectW));
                const int clampedY = std::clamp(minCellY, 0, std::max(0, texH - rectH));
                const int clampedW = std::min(rectW, texW - clampedX);
                const int clampedH = std::min(rectH, texH - clampedY);

                if (clampedW <= 0 || clampedH <= 0) continue;

                const atlas::Rect regionRect{ clampedX, clampedY, clampedW, clampedH };
                const std::string tileKey = addTile(sourceTexture, regionRect);
                if (tileKey.empty()) continue;

                const int regionId = static_cast<int>(meshPlan.regions.size());
                RegionMapping rm;
                rm.tileKey = tileKey;
                rm.originX = clampedX;
                rm.originY = clampedY;
                rm.width = clampedW;
                rm.height = clampedH;
                meshPlan.regions.push_back(std::move(rm));

                for (int poly : polys)
                {
                    meshPlan.polyToRegion[poly] = regionId;
                }
            }
        }

        result.meshes.push_back(std::move(meshPlan));
        ++meshIdx;
    }

    return result;
}
} // namespace polyx::uv
