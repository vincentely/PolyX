#include "uv/UVAnalyzer.h"
#include "uv/UVAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
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

std::string MakeContext(const fbxsdk::FbxMesh* mesh, int polygonIndex, int uvSetIndex)
{
    std::ostringstream stream;
    stream << "mesh='" << (mesh != nullptr ? mesh->GetName() : "<null>")
           << "' polygon=" << polygonIndex
           << " uvSet=" << uvSetIndex;
    return stream.str();
}

std::pair<int, int> QuantizeToBlockOrigin(const UvPoint& uv, int textureWidth, int textureHeight)
{
    const int texelX = std::clamp(static_cast<int>(std::floor(uv.u * static_cast<double>(textureWidth))),
                                  0,
                                  std::max(0, textureWidth - 1));
    const int texelY = std::clamp(static_cast<int>(std::floor((1.0 - uv.v) * static_cast<double>(textureHeight))),
                                  0,
                                  std::max(0, textureHeight - 1));

    return { (texelX / 8) * 8, (texelY / 8) * 8 };
}

atlas::Rect MakeTileRect(int originX, int originY, int width, int height, int textureWidth, int textureHeight)
{
    atlas::Rect rect{};
    rect.width = width;
    rect.height = height;
    rect.x = std::clamp(originX, 0, std::max(0, textureWidth - width));
    rect.y = std::clamp(originY, 0, std::max(0, textureHeight - height));
    return rect;
}

atlas::Rect BuildSampleRect(const std::vector<UvPoint>& uvs,
                            int textureWidth,
                            int textureHeight,
                            core::Logger& logger,
                            const std::string& context)
{
    atlas::Rect rect{};
    if (uvs.empty())
    {
        logger.Warn("Polygon has no UV vertices for " + context);
        return rect;
    }

    double sumU = 0.0;
    double sumV = 0.0;
    for (const UvPoint& uv : uvs)
    {
        sumU += uv.u;
        sumV += uv.v;
    }

    const UvPoint centroid
    {
        sumU / static_cast<double>(uvs.size()),
        sumV / static_cast<double>(uvs.size())
    };

    struct CellSample
    {
        std::pair<int, int> origin{};
        std::size_t count = 0;
    };

    std::vector<CellSample> sampledCells;
    sampledCells.reserve(uvs.size() + 1U);

    const auto addCell = [&sampledCells](const std::pair<int, int>& origin)
    {
        const auto it = std::find_if(sampledCells.begin(), sampledCells.end(), [&](const CellSample& sample)
        {
            return sample.origin == origin;
        });

        if (it == sampledCells.end())
        {
            sampledCells.push_back(CellSample{ origin, 1U });
        }
        else
        {
            ++it->count;
        }
    };

    for (const UvPoint& uv : uvs)
    {
        addCell(QuantizeToBlockOrigin(uv, textureWidth, textureHeight));
    }
    addCell(QuantizeToBlockOrigin(centroid, textureWidth, textureHeight));

    const std::pair<int, int> centroidCell = QuantizeToBlockOrigin(centroid, textureWidth, textureHeight);
    std::pair<int, int> chosenCell = centroidCell;
    std::size_t chosenCount = 0U;
    for (const CellSample& sample : sampledCells)
    {
        if (sample.count > chosenCount || (sample.count == chosenCount && sample.origin == centroidCell))
        {
            chosenCell = sample.origin;
            chosenCount = sample.count;
        }
    }

    const bool sameColumn = std::all_of(sampledCells.begin(), sampledCells.end(), [&](const CellSample& sample)
    {
        return sample.origin.first == chosenCell.first;
    });

    const bool sameRow = std::all_of(sampledCells.begin(), sampledCells.end(), [&](const CellSample& sample)
    {
        return sample.origin.second == chosenCell.second;
    });

    if (sameColumn && sampledCells.size() > 1U)
    {
        const int height = textureHeight >= 32 ? 32 : 8;
        rect = MakeTileRect(chosenCell.first, chosenCell.second, 8, height, textureWidth, textureHeight);
        return rect;
    }

    if (sameRow && sampledCells.size() > 1U)
    {
        const int width = textureWidth >= 32 ? 32 : 8;
        rect = MakeTileRect(chosenCell.first, chosenCell.second, width, 8, textureWidth, textureHeight);
        return rect;
    }

    rect = MakeTileRect(chosenCell.first, chosenCell.second, 8, 8, textureWidth, textureHeight);
    return rect;
}

int ScoreSolidBlock(const atlas::Image& image)
{
    if (image.Empty() || image.width != 8 || image.height != 8)
    {
        return -1;
    }

    const std::uint8_t* firstPixel = image.pixels.data();
    for (std::size_t offset = 4U; offset < image.pixels.size(); offset += 4U)
    {
        if (std::memcmp(firstPixel, image.pixels.data() + offset, 4U) != 0)
        {
            return -1;
        }
    }

    return 1000;
}

int ScoreVerticalGradientStrip(const atlas::Image& image)
{
    if (image.Empty() || image.width != 8 || image.height != 32)
    {
        return -1;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;
    const std::uint8_t* firstRow = image.pixels.data();
    bool hasRowDifference = false;

    for (int y = 0; y < image.height; ++y)
    {
        const std::uint8_t* row = image.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
        for (std::size_t x = 4U; x < rowBytes; x += 4U)
        {
            if (std::memcmp(row, row + x, 4U) != 0)
            {
                return -1;
            }
        }

        if (y > 0 && std::memcmp(firstRow, row, rowBytes) != 0)
        {
            hasRowDifference = true;
        }
    }

    return hasRowDifference ? 900 : -1;
}

int ScoreHorizontalGradientStrip(const atlas::Image& image)
{
    if (image.Empty() || image.width != 32 || image.height != 8)
    {
        return -1;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;
    bool hasColumnDifference = false;
    std::array<std::uint8_t, 4U> firstColumn{};

    for (int x = 0; x < image.width; ++x)
    {
        const std::uint8_t* topPixel = image.pixels.data() + static_cast<std::size_t>(x) * 4U;
        for (int y = 1; y < image.height; ++y)
        {
            const std::uint8_t* pixel = image.pixels.data() + static_cast<std::size_t>(y) * rowBytes + static_cast<std::size_t>(x) * 4U;
            if (std::memcmp(topPixel, pixel, 4U) != 0)
            {
                return -1;
            }
        }

        if (x == 0)
        {
            std::memcpy(firstColumn.data(), topPixel, 4U);
        }
        else if (std::memcmp(firstColumn.data(), topPixel, 4U) != 0)
        {
            hasColumnDifference = true;
        }
    }

    return hasColumnDifference ? 900 : -1;
}

atlas::Rect ChooseTileRect(const atlas::Image& sourceTexture,
                           const std::vector<UvPoint>& uvs,
                           core::Logger& logger,
                           const std::string& context)
{
    const atlas::Rect sampleRect = BuildSampleRect(uvs, sourceTexture.width, sourceTexture.height, logger, context);
    if (sampleRect.width <= 0 || sampleRect.height <= 0)
    {
        return sampleRect;
    }

    struct Candidate
    {
        atlas::Rect rect;
        int score = -1;
    };

    std::vector<Candidate> candidates;
    const int aligned8X = (sampleRect.x / 8) * 8;
    const int aligned8Y = (sampleRect.y / 8) * 8;

    candidates.push_back(Candidate{ MakeTileRect(aligned8X, aligned8Y, 8, 8, sourceTexture.width, sourceTexture.height), -1 });
    const int stripY = (aligned8Y / 32) * 32;
    if (stripY + 32 <= sourceTexture.height)
    {
        candidates.push_back(Candidate{ MakeTileRect(aligned8X, stripY, 8, 32, sourceTexture.width, sourceTexture.height), -1 });
    }
    const int stripX = (aligned8X / 32) * 32;
    if (stripX + 32 <= sourceTexture.width)
    {
        candidates.push_back(Candidate{ MakeTileRect(stripX, aligned8Y, 32, 8, sourceTexture.width, sourceTexture.height), -1 });
    }

    int bestScore = -1;
    atlas::Rect bestRect = sampleRect;
    for (Candidate& candidate : candidates)
    {
        std::string candidateError;
        atlas::Image tile = atlas::ExtractSubImage(sourceTexture, candidate.rect, &candidateError);
        if (tile.Empty())
        {
            continue;
        }

        const int solidScore = ScoreSolidBlock(tile);
        const int verticalScore = ScoreVerticalGradientStrip(tile);
        const int horizontalScore = ScoreHorizontalGradientStrip(tile);
        candidate.score = std::max({ solidScore, verticalScore, horizontalScore });
        if (candidate.score > bestScore)
        {
            bestScore = candidate.score;
            bestRect = candidate.rect;
        }
    }

    if (bestScore >= 0)
    {
        return bestRect;
    }

    return sampleRect;
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
                                   const atlas::Image& sourceTexture,
                                   core::Logger& logger) const
{
    ScenePlan result;

    if (scene == nullptr)
    {
        logger.Warn("Scene is null during UV analysis.");
        return result;
    }

    if (sourceTexture.Empty())
    {
        logger.Warn("Source texture is empty during UV analysis.");
        return result;
    }

    std::vector<fbxsdk::FbxMesh*> meshes;
    CollectMeshes(scene->GetRootNode(), meshes);

    std::unordered_map<std::string, std::size_t> uniqueTileIndex;
    result.meshes.reserve(meshes.size());

    const auto addTile = [&](const atlas::Rect& rect) -> std::string
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

    const auto ensureCell = [&](int cellX, int cellY)
    {
        const CellCoord coord{ cellX, cellY };
        if (result.cellMap.find(coord) != result.cellMap.end())
        {
            return;
        }

        const int texW = sourceTexture.width;
        const int texH = sourceTexture.height;

        const int vStripY = (cellY / 32) * 32;
        if (vStripY + 32 <= texH)
        {
            const atlas::Rect vRect = MakeTileRect(cellX, vStripY, 8, 32, texW, texH);
            const atlas::Image vImg = atlas::ExtractSubImage(sourceTexture, vRect, nullptr);
            if (!vImg.Empty() && ScoreVerticalGradientStrip(vImg) > 0)
            {
                const std::string key = addTile(vRect);
                if (!key.empty())
                {
                    for (int sy = vStripY; sy < vStripY + 32; sy += 8)
                    {
                        const CellCoord sc{ cellX, sy };
                        if (result.cellMap.find(sc) == result.cellMap.end())
                        {
                            result.cellMap.emplace(sc, CellMapping{ key, vRect.x, vRect.y, 8, 32 });
                        }
                    }
                    return;
                }
            }
        }

        const int hStripX = (cellX / 32) * 32;
        if (hStripX + 32 <= texW)
        {
            const atlas::Rect hRect = MakeTileRect(hStripX, cellY, 32, 8, texW, texH);
            const atlas::Image hImg = atlas::ExtractSubImage(sourceTexture, hRect, nullptr);
            if (!hImg.Empty() && ScoreHorizontalGradientStrip(hImg) > 0)
            {
                const std::string key = addTile(hRect);
                if (!key.empty())
                {
                    for (int sx = hStripX; sx < hStripX + 32; sx += 8)
                    {
                        const CellCoord sc{ sx, cellY };
                        if (result.cellMap.find(sc) == result.cellMap.end())
                        {
                            result.cellMap.emplace(sc, CellMapping{ key, hRect.x, hRect.y, 32, 8 });
                        }
                    }
                    return;
                }
            }
        }

        const atlas::Rect cellRect = MakeTileRect(cellX, cellY, 8, 8, texW, texH);
        const std::string key = addTile(cellRect);
        if (!key.empty())
        {
            result.cellMap.emplace(coord, CellMapping{ key, cellRect.x, cellRect.y, 8, 8 });
        }
    };

    int meshIdx = 0;
    for (fbxsdk::FbxMesh* mesh : meshes)
    {
        if (mesh == nullptr)
        {
            ++meshIdx;
            continue;
        }

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

            const auto& directArray = uvElement->GetDirectArray();
            const auto& indexArray = uvElement->GetIndexArray();
            const bool indexed = uvElement->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect;

            for (int i = 0; i < directArray.GetCount(); ++i)
            {
                const fbxsdk::FbxVector2 uv = directArray.GetAt(i);
                const UvPoint pt{ uv[0], uv[1] };
                const auto [cellX, cellY] = QuantizeToBlockOrigin(pt, sourceTexture.width, sourceTexture.height);
                ensureCell(cellX, cellY);
            }

            const int texW = sourceTexture.width;
            const int texH = sourceTexture.height;

            int pvIdx = 0;
            for (int poly = 0; poly < mesh->GetPolygonCount(); ++poly)
            {
                const int polySize = mesh->GetPolygonSize(poly);

                int minCellX = std::numeric_limits<int>::max();
                int minCellY = std::numeric_limits<int>::max();
                int maxCellX = std::numeric_limits<int>::min();
                int maxCellY = std::numeric_limits<int>::min();

                std::vector<std::pair<int, int>> vertCells;
                vertCells.reserve(polySize);

                for (int v = 0; v < polySize; ++v)
                {
                    int dIdx = pvIdx;
                    if (indexed && pvIdx < indexArray.GetCount())
                    {
                        dIdx = indexArray.GetAt(pvIdx);
                    }
                    ++pvIdx;

                    if (dIdx < 0 || dIdx >= directArray.GetCount())
                    {
                        continue;
                    }

                    const fbxsdk::FbxVector2 uv = directArray.GetAt(dIdx);
                    const UvPoint pt{ uv[0], uv[1] };
                    const auto [cx, cy] = QuantizeToBlockOrigin(pt, texW, texH);
                    vertCells.emplace_back(cx, cy);

                    minCellX = std::min(minCellX, cx);
                    minCellY = std::min(minCellY, cy);
                    maxCellX = std::max(maxCellX, cx);
                    maxCellY = std::max(maxCellY, cy);
                }

                if (vertCells.empty())
                {
                    continue;
                }

                const bool sameCell = (minCellX == maxCellX && minCellY == maxCellY);
                if (sameCell)
                {
                    continue;
                }

                const int stripX = minCellX;
                const int stripY = minCellY;
                const int stripW = maxCellX + 8 - minCellX;
                const int stripH = maxCellY + 8 - minCellY;

                const int clampedX = std::clamp(stripX, 0, std::max(0, texW - stripW));
                const int clampedY = std::clamp(stripY, 0, std::max(0, texH - stripH));
                const int clampedW = std::min(stripW, texW - clampedX);
                const int clampedH = std::min(stripH, texH - clampedY);

                if (clampedW <= 0 || clampedH <= 0)
                {
                    continue;
                }

                const atlas::Rect stripRect{ clampedX, clampedY, clampedW, clampedH };
                const std::string stripKey = addTile(stripRect);
                if (!stripKey.empty())
                {
                    PolyStripMapping psm;
                    psm.tileKey = stripKey;
                    psm.stripOriginX = clampedX;
                    psm.stripOriginY = clampedY;
                    psm.stripWidth = clampedW;
                    psm.stripHeight = clampedH;
                    result.polyStripMap.emplace(PolyKey{ meshIdx, poly }, std::move(psm));
                }
            }

            LayerPlan layerPlan;
            layerPlan.uvSetName = uvSetName;
            meshPlan.layers.push_back(std::move(layerPlan));
        }

        result.meshes.push_back(std::move(meshPlan));
        ++meshIdx;
    }

    return result;
}
} // namespace polyx::uv


