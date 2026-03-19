#include "app/BatchProcessor.h"
#include "app/BatchProcessor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <unordered_map>

#include <fbxsdk.h>

namespace polyx::app
{
namespace
{
bool HasExtensionCaseInsensitive(const std::filesystem::path& path, const std::vector<std::string>& extensions)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const std::string& expected : extensions)
    {
        if (ext == expected)
        {
            return true;
        }
    }

    return false;
}

std::filesystem::path RelativeTo(const std::filesystem::path& path, const std::filesystem::path& base)
{
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, base, ec);
    if (!ec)
    {
        return relative;
    }

    return path.filename();
}

std::string ToGenericString(const std::filesystem::path& path)
{
    return path.generic_string();
}

} // namespace

BatchProcessor::BatchProcessor(const core::AppConfig& config, core::Logger& logger)
    : config_(config)
    , logger_(logger)
{
}

bool BatchProcessor::Run()
{
    if (!std::filesystem::exists(config_.inputDir))
    {
        logger_.Error("Input directory does not exist: " + config_.inputDir.string());
        return false;
    }

    std::filesystem::create_directories(config_.outputDir);

    std::vector<PackageInfo> packages;
    if (!DiscoverPackages(packages))
    {
        return false;
    }

    if (packages.empty())
    {
        logger_.Error("No package directories or FBX files were found.");
        return false;
    }

    uv::UVAnalyzer analyzer;
    std::vector<FilePlan> filePlans;
    atlas::AtlasBuilder atlasBuilder(static_cast<int>(config_.atlasWidth), static_cast<int>(config_.atlasHeight));
    bool hadFatalError = false;

    for (const PackageInfo& package : packages)
    {
        if (package.sourceTexture.Empty() || package.fbxFiles.empty())
        {
            continue;
        }

        for (const std::filesystem::path& fbxFile : package.fbxFiles)
        {
            if (config_.verbose)
            {
                logger_.Info("Analyzing FBX: " + fbxFile.string());
            }

            fbx::FbxLoader loader;
            std::string errorMessage;
            if (!loader.Load(fbxFile, &errorMessage))
            {
                logger_.Error("Failed to load FBX for analysis: " + fbxFile.string() + " | " + errorMessage);
                hadFatalError = true;
                continue;
            }

            uv::ScenePlan scenePlan = analyzer.AnalyzeScene(loader.Scene(), package.sourceTexture, logger_);
            for (const uv::TileCandidate& tile : scenePlan.uniqueTiles)
            {
                std::string tileError;
                if (!atlasBuilder.AddTile(tile.key, tile.image, tile.sourceRect, &tileError))
                {
                    logger_.Error("Failed to add atlas tile from " + fbxFile.string() + " | " + tileError);
                    hadFatalError = true;
                }
            }

            FilePlan filePlan;
            filePlan.inputFbx = fbxFile;
            filePlan.outputFbx = MakeOutputPath(fbxFile);
            filePlan.sourceTexturePath = package.sourceTexturePath;
            filePlan.sourceTexture = package.sourceTexture;
            filePlan.scenePlan = std::move(scenePlan);
            filePlans.push_back(std::move(filePlan));
        }
    }

    if (filePlans.empty())
    {
        logger_.Error("No FBX files could be analyzed successfully.");
        return false;
    }

    std::filesystem::path atlasOutputPath;
    if (!BuildAtlas(filePlans, atlasBuilder, atlasOutputPath))
    {
        return false;
    }

    if (!ExportScenes(filePlans, atlasBuilder, atlasOutputPath))
    {
        hadFatalError = true;
    }

    if (logger_.WarningCount() > 0)
    {
        logger_.Info("Warnings emitted: " + std::to_string(logger_.WarningCount()));
    }

    return !hadFatalError;
}

bool BatchProcessor::DiscoverPackages(std::vector<PackageInfo>& packages)
{
    packages.clear();

    std::vector<std::filesystem::path> packageRoots;
    for (const auto& entry : std::filesystem::directory_iterator(config_.inputDir))
    {
        if (entry.is_directory())
        {
            packageRoots.push_back(entry.path());
        }
    }

    if (packageRoots.empty())
    {
        packageRoots.push_back(config_.inputDir);
    }

    std::sort(packageRoots.begin(), packageRoots.end());

    for (const std::filesystem::path& packageRoot : packageRoots)
    {
        PackageInfo package;
        package.packageRoot = packageRoot;

        if (!FindSourceTexture(packageRoot, package.sourceTexturePath))
        {
            logger_.Warn("Skipping package without a unique source texture: " + packageRoot.string());
            continue;
        }

        std::string textureError;
        if (!atlas::LoadImageFile(package.sourceTexturePath, package.sourceTexture, &textureError))
        {
            logger_.Error("Failed to load source texture: " + package.sourceTexturePath.string() + " | " + textureError);
            continue;
        }

        CollectFbxFiles(packageRoot, package.fbxFiles);
        if (package.fbxFiles.empty())
        {
            logger_.Warn("No FBX files found under package: " + packageRoot.string());
            continue;
        }

        packages.push_back(std::move(package));
    }

    return true;
}

bool BatchProcessor::FindSourceTexture(const std::filesystem::path& packageRoot, std::filesystem::path& texturePath) const
{
    static const std::vector<std::string> kSupportedTextureExtensions =
    {
        ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".tif", ".tiff"
    };

    std::vector<std::filesystem::path> textureFiles;
    for (const auto& entry : std::filesystem::directory_iterator(packageRoot))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        if (HasExtensionCaseInsensitive(entry.path(), kSupportedTextureExtensions))
        {
            textureFiles.push_back(entry.path());
        }
    }

    if (textureFiles.size() != 1U)
    {
        if (textureFiles.empty())
        {
            logger_.Warn("No root texture found in package: " + packageRoot.string());
        }
        else
        {
            logger_.Warn("Multiple root textures found in package: " + packageRoot.string());
        }
        return false;
    }

    texturePath = textureFiles.front();
    return true;
}

void BatchProcessor::CollectFbxFiles(const std::filesystem::path& packageRoot, std::vector<std::filesystem::path>& files) const
{
    files.clear();

    const auto options = std::filesystem::directory_options::skip_permission_denied;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(packageRoot, options))
    {
        if (!entry.is_regular_file())
        {
            continue;
        }

        if (HasExtensionCaseInsensitive(entry.path(), { ".fbx" }))
        {
            files.push_back(entry.path());
        }
    }

    std::sort(files.begin(), files.end());
}

std::filesystem::path BatchProcessor::MakeOutputPath(const std::filesystem::path& inputFbx) const
{
    const std::filesystem::path relativePath = RelativeTo(inputFbx, config_.inputDir);
    return config_.outputDir / relativePath;
}

bool BatchProcessor::BuildAtlas(const std::vector<FilePlan>& filePlans,
                                atlas::AtlasBuilder& atlasBuilder,
                                std::filesystem::path& atlasOutputPath)
{
    atlasOutputPath = config_.outputDir / "atlas.png";

    if (!std::filesystem::exists(config_.outputDir))
    {
        std::filesystem::create_directories(config_.outputDir);
    }

    std::string buildError;
    if (!atlasBuilder.Build(&buildError))
    {
        logger_.Error("Atlas build failed: " + buildError);
        return false;
    }

    std::string saveError;
    if (!atlasBuilder.SaveAtlas(atlasOutputPath, &saveError))
    {
        logger_.Error("Failed to save atlas PNG: " + saveError);
        return false;
    }

    logger_.Info("Atlas saved to: " + atlasOutputPath.string());
    return true;
}

bool BatchProcessor::ExportScenes(const std::vector<FilePlan>& filePlans,
                                 const atlas::AtlasBuilder& atlasBuilder,
                                 const std::filesystem::path& atlasOutputPath)
{
    bool hadFatalError = false;
    for (const FilePlan& filePlan : filePlans)
    {
        if (config_.verbose)
        {
            logger_.Info("Exporting FBX: " + filePlan.inputFbx.string());
        }

        fbx::FbxLoader inputLoader;
        {
            std::string errorMessage;
            if (!inputLoader.Load(filePlan.inputFbx, &errorMessage))
            {
                logger_.Error("Failed to load original FBX for reference: " + filePlan.inputFbx.string() + " | " + errorMessage);
                hadFatalError = true;
                continue;
            }
        }

        fbx::FbxLoader loader;
        {
            std::string errorMessage;
            if (!loader.Load(filePlan.inputFbx, &errorMessage))
            {
                logger_.Error("Failed to reload FBX for export: " + filePlan.inputFbx.string() + " | " + errorMessage);
                hadFatalError = true;
                continue;
            }
        }

        if (!ApplyScenePlan(loader.Scene(), inputLoader.Scene(), filePlan, atlasBuilder, atlasOutputPath))
        {
            hadFatalError = true;
            continue;
        }

        std::filesystem::create_directories(filePlan.outputFbx.parent_path());
        std::string saveError;
        if (!loader.Save(filePlan.outputFbx, &saveError))
        {
            logger_.Error("Failed to save FBX: " + filePlan.outputFbx.string() + " | " + saveError);
            hadFatalError = true;
            continue;
        }

        VerifyExportedMesh(filePlan, inputLoader.Scene(), atlasBuilder);
    }

    return !hadFatalError;
}

bool BatchProcessor::ApplyScenePlan(fbxsdk::FbxScene* scene,
                                   fbxsdk::FbxScene* originalScene,
                                   const FilePlan& filePlan,
                                   const atlas::AtlasBuilder& atlasBuilder,
                                   const std::filesystem::path& atlasOutputPath)
{
    if (scene == nullptr || originalScene == nullptr)
    {
        logger_.Error("Cannot apply scene plan to a null scene.");
        return false;
    }

    std::vector<fbxsdk::FbxMesh*> meshes;
    CollectMeshes(scene->GetRootNode(), meshes);

    std::vector<fbxsdk::FbxMesh*> origMeshes;
    CollectMeshes(originalScene->GetRootNode(), origMeshes);

    const int texW = filePlan.sourceTexture.width;
    const int texH = filePlan.sourceTexture.height;
    const int atlasW = atlasBuilder.GetAtlasImage().width;
    const int atlasH = atlasBuilder.GetAtlasImage().height;

    const auto remapUvCell = [&](double srcU, double srcV,
                                  const uv::CellMapping& cm,
                                  const atlas::AtlasEntry* entry) -> fbxsdk::FbxVector2
    {
        const double pixU = srcU * static_cast<double>(texW);
        const double pixV = (1.0 - srcV) * static_cast<double>(texH);
        const double localU = (pixU - static_cast<double>(cm.tileOriginX)) / static_cast<double>(cm.tileWidth);
        const double localV = (pixV - static_cast<double>(cm.tileOriginY)) / static_cast<double>(cm.tileHeight);

        const double halfTexelU = 0.5 / static_cast<double>(atlasW);
        const double halfTexelV = 0.5 / static_cast<double>(atlasH);
        const double tileU0 = static_cast<double>(entry->atlasRect.x) / static_cast<double>(atlasW);
        const double tileU1 = static_cast<double>(entry->atlasRect.x + entry->atlasRect.width) / static_cast<double>(atlasW);
        const double tileV0 = static_cast<double>(entry->atlasRect.y) / static_cast<double>(atlasH);
        const double tileV1 = static_cast<double>(entry->atlasRect.y + entry->atlasRect.height) / static_cast<double>(atlasH);

        double newU = tileU0 + std::clamp(localU, 0.0, 1.0) * (tileU1 - tileU0);
        double newAtlasV = tileV0 + std::clamp(localV, 0.0, 1.0) * (tileV1 - tileV0);

        newU = std::clamp(newU, tileU0 + halfTexelU, tileU1 - halfTexelU);
        newAtlasV = std::clamp(newAtlasV, tileV0 + halfTexelV, tileV1 - halfTexelV);

        return fbxsdk::FbxVector2(newU, 1.0 - newAtlasV);
    };

    const auto remapUvStrip = [&](double srcU, double srcV,
                                   const uv::PolyStripMapping& psm,
                                   const atlas::AtlasEntry* entry) -> fbxsdk::FbxVector2
    {
        const double pixU = srcU * static_cast<double>(texW);
        const double pixV = (1.0 - srcV) * static_cast<double>(texH);
        const double localU = (pixU - static_cast<double>(psm.stripOriginX)) / static_cast<double>(psm.stripWidth);
        const double localV = (pixV - static_cast<double>(psm.stripOriginY)) / static_cast<double>(psm.stripHeight);

        const double halfTexelU = 0.5 / static_cast<double>(atlasW);
        const double halfTexelV = 0.5 / static_cast<double>(atlasH);
        const double tileU0 = static_cast<double>(entry->atlasRect.x) / static_cast<double>(atlasW);
        const double tileU1 = static_cast<double>(entry->atlasRect.x + entry->atlasRect.width) / static_cast<double>(atlasW);
        const double tileV0 = static_cast<double>(entry->atlasRect.y) / static_cast<double>(atlasH);
        const double tileV1 = static_cast<double>(entry->atlasRect.y + entry->atlasRect.height) / static_cast<double>(atlasH);

        double newU = tileU0 + std::clamp(localU, 0.0, 1.0) * (tileU1 - tileU0);
        double newAtlasV = tileV0 + std::clamp(localV, 0.0, 1.0) * (tileV1 - tileV0);

        newU = std::clamp(newU, tileU0 + halfTexelU, tileU1 - halfTexelU);
        newAtlasV = std::clamp(newAtlasV, tileV0 + halfTexelV, tileV1 - halfTexelV);

        return fbxsdk::FbxVector2(newU, 1.0 - newAtlasV);
    };

    const auto findMapping = [&](double srcU, double srcV)
        -> std::pair<const uv::CellMapping*, const atlas::AtlasEntry*>
    {
        const int px = std::clamp(static_cast<int>(std::floor(srcU * static_cast<double>(texW))), 0, std::max(0, texW - 1));
        const int py = std::clamp(static_cast<int>(std::floor((1.0 - srcV) * static_cast<double>(texH))), 0, std::max(0, texH - 1));
        const uv::CellCoord coord{ (px / 8) * 8, (py / 8) * 8 };
        const auto cellIt = filePlan.scenePlan.cellMap.find(coord);
        if (cellIt == filePlan.scenePlan.cellMap.end())
        {
            return { nullptr, nullptr };
        }
        const uv::CellMapping& cm = cellIt->second;
        const atlas::AtlasEntry* entry = atlasBuilder.FindEntry(cm.tileKey);
        return { &cm, entry };
    };

    const std::size_t meshCount = std::min(meshes.size(), origMeshes.size());
    for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
    {
        fbxsdk::FbxMesh* mesh = meshes[meshIndex];
        const fbxsdk::FbxMesh* origMesh = origMeshes[meshIndex];

        for (int uvSetIndex = 0; uvSetIndex < origMesh->GetElementUVCount(); ++uvSetIndex)
        {
            const fbxsdk::FbxGeometryElementUV* origUvElement = origMesh->GetElementUV(uvSetIndex);
            fbxsdk::FbxGeometryElementUV* uvElement = mesh->GetElementUV(uvSetIndex);
            if (origUvElement == nullptr || uvElement == nullptr)
            {
                continue;
            }

            const auto& origDirectArray = origUvElement->GetDirectArray();
            auto& directArray = uvElement->GetDirectArray();
            auto& indexArray = uvElement->GetIndexArray();
            const bool indexed = origUvElement->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect;
            const int directCount = origDirectArray.GetCount();

            std::vector<int> remappedWithTile(directCount, -1);

            int pvIdx = 0;
            for (int poly = 0; poly < origMesh->GetPolygonCount(); ++poly)
            {
                const int polySize = origMesh->GetPolygonSize(poly);

                struct PvInfo { int pvIndex; int dIdx; double u; double v; };
                std::vector<PvInfo> pvs;
                pvs.reserve(polySize);

                for (int v = 0; v < polySize; ++v)
                {
                    int dIdx = pvIdx;
                    if (indexed && pvIdx < origUvElement->GetIndexArray().GetCount())
                    {
                        dIdx = origUvElement->GetIndexArray().GetAt(pvIdx);
                    }
                    if (dIdx >= 0 && dIdx < directCount)
                    {
                        const fbxsdk::FbxVector2 uv = origDirectArray.GetAt(dIdx);
                        pvs.push_back({ pvIdx, dIdx, uv[0], uv[1] });
                    }
                    ++pvIdx;
                }

                if (pvs.empty()) continue;

                const uv::PolyKey polyKey{ static_cast<int>(meshIndex), poly };
                const auto stripIt = filePlan.scenePlan.polyStripMap.find(polyKey);

                if (stripIt != filePlan.scenePlan.polyStripMap.end())
                {
                    const uv::PolyStripMapping& psm = stripIt->second;
                    const atlas::AtlasEntry* entry = atlasBuilder.FindEntry(psm.tileKey);
                    if (entry != nullptr)
                    {
                        const int tileId = static_cast<int>(entry - atlasBuilder.Entries().data());
                        for (const PvInfo& pv : pvs)
                        {
                            const fbxsdk::FbxVector2 newUv = remapUvStrip(pv.u, pv.v, psm, entry);

                            const int newDIdx = directArray.GetCount();
                            directArray.Add(newUv);
                            remappedWithTile.push_back(tileId);
                            if (indexed) indexArray.SetAt(pv.pvIndex, newDIdx);
                        }
                    }
                }
                else
                {
                    for (const PvInfo& pv : pvs)
                    {
                        auto [cm, entry] = findMapping(pv.u, pv.v);
                        if (cm == nullptr || entry == nullptr) continue;
                        const int tileId = static_cast<int>(entry - atlasBuilder.Entries().data());

                        if (remappedWithTile[pv.dIdx] == tileId) continue;

                        const fbxsdk::FbxVector2 newUv = remapUvCell(pv.u, pv.v, *cm, entry);

                        if (remappedWithTile[pv.dIdx] < 0)
                        {
                            directArray.SetAt(pv.dIdx, newUv);
                            remappedWithTile[pv.dIdx] = tileId;
                        }
                        else
                        {
                            const int newDIdx = directArray.GetCount();
                            directArray.Add(newUv);
                            remappedWithTile.push_back(tileId);
                            if (indexed) indexArray.SetAt(pv.pvIndex, newDIdx);
                        }
                    }
                }
            }
        }
    }

    std::filesystem::path atlasRelativePath = RelativeTo(atlasOutputPath, filePlan.outputFbx.parent_path());
    ApplySceneMaterials(scene, atlasRelativePath, filePlan.scenePlan.primaryUvSetName);
    return true;
}

void BatchProcessor::ApplySceneMaterials(fbxsdk::FbxScene* scene,
                                        const std::filesystem::path& atlasRelativePath,
                                        const std::string& uvSetName)
{
    if (scene == nullptr)
    {
        return;
    }

    fbxsdk::FbxFileTexture* atlasTexture = fbxsdk::FbxFileTexture::Create(scene, "PolyXAtlasTexture");
    if (atlasTexture == nullptr)
    {
        logger_.Error("Failed to create FBX atlas texture object.");
        return;
    }

    const std::string atlasPathString = ToGenericString(atlasRelativePath);
    atlasTexture->SetFileName(atlasPathString.c_str());
    atlasTexture->SetRelativeFileName(atlasPathString.c_str());
    atlasTexture->SetTextureUse(fbxsdk::FbxTexture::eStandard);
    atlasTexture->SetMaterialUse(fbxsdk::FbxFileTexture::eModelMaterial);
    atlasTexture->UVSet.Set(uvSetName.empty() ? "map1" : uvSetName.c_str());

    std::vector<fbxsdk::FbxNode*> stack;
    stack.push_back(scene->GetRootNode());
    while (!stack.empty())
    {
        fbxsdk::FbxNode* node = stack.back();
        stack.pop_back();
        if (node == nullptr)
        {
            continue;
        }

        for (int materialIndex = 0; materialIndex < node->GetMaterialCount(); ++materialIndex)
        {
            fbxsdk::FbxSurfaceMaterial* material = node->GetMaterial(materialIndex);
            if (material == nullptr)
            {
                continue;
            }

            fbxsdk::FbxProperty diffuse = material->FindProperty(fbxsdk::FbxSurfaceMaterial::sDiffuse);
            if (diffuse.IsValid())
            {
                diffuse.DisconnectAllSrcObject();
                diffuse.ConnectSrcObject(atlasTexture);
            }
        }

        for (int childIndex = 0; childIndex < node->GetChildCount(); ++childIndex)
        {
            stack.push_back(node->GetChild(childIndex));
        }
    }
}

void BatchProcessor::VerifyExportedMesh(const FilePlan& filePlan, fbxsdk::FbxScene* originalScene, const atlas::AtlasBuilder& atlasBuilder)
{
    fbx::FbxLoader outputLoader;
    std::string loadError;
    if (!outputLoader.Load(filePlan.outputFbx, &loadError))
    {
        logger_.Warn("Verify: failed to reload output FBX: " + filePlan.outputFbx.string() + " | " + loadError);
        return;
    }

    std::vector<fbxsdk::FbxMesh*> origMeshes;
    CollectMeshes(originalScene->GetRootNode(), origMeshes);

    std::vector<fbxsdk::FbxMesh*> outMeshes;
    CollectMeshes(outputLoader.Scene()->GetRootNode(), outMeshes);

    if (origMeshes.size() != outMeshes.size())
    {
        logger_.Warn("Verify: mesh count mismatch for " + filePlan.outputFbx.filename().string() +
                     " (input=" + std::to_string(origMeshes.size()) +
                     " output=" + std::to_string(outMeshes.size()) + ")");
    }

    const atlas::Image& atlasImg = atlasBuilder.GetAtlasImage();
    const atlas::Image& srcImg = filePlan.sourceTexture;

    const std::size_t meshCount = std::min(origMeshes.size(), outMeshes.size());
    for (std::size_t i = 0; i < meshCount; ++i)
    {
        const fbxsdk::FbxMesh* origMesh = origMeshes[i];
        const fbxsdk::FbxMesh* outMesh = outMeshes[i];
        const std::string meshName = origMesh->GetName() ? origMesh->GetName() : "?";

        const int origCP = origMesh->GetControlPointsCount();
        const int outCP = outMesh->GetControlPointsCount();
        const int origPoly = origMesh->GetPolygonCount();
        const int outPoly = outMesh->GetPolygonCount();

        if (origCP != outCP || origPoly != outPoly)
        {
            logger_.Warn("Verify[" + meshName + "]: CP=" + std::to_string(origCP) + "/" + std::to_string(outCP) +
                         " Poly=" + std::to_string(origPoly) + "/" + std::to_string(outPoly) +
                         " ** GEOMETRY CHANGED **");
        }

        if (!atlasImg.Empty() && !srcImg.Empty() &&
            origMesh->GetElementUVCount() > 0 && outMesh->GetElementUVCount() > 0)
        {
            const fbxsdk::FbxGeometryElementUV* origUvEl = origMesh->GetElementUV(0);
            const fbxsdk::FbxGeometryElementUV* outUvEl = outMesh->GetElementUV(0);
            const auto& origDA = origUvEl->GetDirectArray();
            const auto& outDA = outUvEl->GetDirectArray();
            const auto& origIA = origUvEl->GetIndexArray();
            const auto& outIA = outUvEl->GetIndexArray();
            const bool origIndexed = origUvEl->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect;
            const bool outIndexed = outUvEl->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect;

            int colorMismatch = 0;
            int totalChecked = 0;
            constexpr int kMaxDetails = 5;
            std::ostringstream details;

            int pvIdx = 0;
            for (int poly = 0; poly < origMesh->GetPolygonCount() && poly < outMesh->GetPolygonCount(); ++poly)
            {
                const int polySize = origMesh->GetPolygonSize(poly);
                for (int v = 0; v < polySize; ++v)
                {
                    const int curPv = pvIdx;
                    ++pvIdx;

                    int origDIdx = curPv;
                    if (origIndexed && curPv < origIA.GetCount())
                    {
                        origDIdx = origIA.GetAt(curPv);
                    }
                    int outDIdx = curPv;
                    if (outIndexed && curPv < outIA.GetCount())
                    {
                        outDIdx = outIA.GetAt(curPv);
                    }

                    if (origDIdx < 0 || origDIdx >= origDA.GetCount() ||
                        outDIdx < 0 || outDIdx >= outDA.GetCount())
                    {
                        continue;
                    }

                    const fbxsdk::FbxVector2 oUv = origDA.GetAt(origDIdx);
                    const int srcPx = std::clamp(static_cast<int>(std::floor(oUv[0] * srcImg.width)), 0, std::max(0, srcImg.width - 1));
                    const int srcPy = std::clamp(static_cast<int>(std::floor((1.0 - oUv[1]) * srcImg.height)), 0, std::max(0, srcImg.height - 1));
                    const std::size_t srcOff = (static_cast<std::size_t>(srcPy) * srcImg.width + srcPx) * 4;

                    const fbxsdk::FbxVector2 nUv = outDA.GetAt(outDIdx);
                    const int atPx = std::clamp(static_cast<int>(std::floor(nUv[0] * atlasImg.width)), 0, std::max(0, atlasImg.width - 1));
                    const int atPy = std::clamp(static_cast<int>(std::floor((1.0 - nUv[1]) * atlasImg.height)), 0, std::max(0, atlasImg.height - 1));
                    const std::size_t atOff = (static_cast<std::size_t>(atPy) * atlasImg.width + atPx) * 4;

                    ++totalChecked;

                    if (srcOff + 3 < srcImg.pixels.size() && atOff + 3 < atlasImg.pixels.size())
                    {
                        const int dr = std::abs(static_cast<int>(srcImg.pixels[srcOff]) - static_cast<int>(atlasImg.pixels[atOff]));
                        const int dg = std::abs(static_cast<int>(srcImg.pixels[srcOff + 1]) - static_cast<int>(atlasImg.pixels[atOff + 1]));
                        const int db = std::abs(static_cast<int>(srcImg.pixels[srcOff + 2]) - static_cast<int>(atlasImg.pixels[atOff + 2]));
                        const int da = std::abs(static_cast<int>(srcImg.pixels[srcOff + 3]) - static_cast<int>(atlasImg.pixels[atOff + 3]));
                        constexpr int kTolerance = 5;
                        if (dr > kTolerance || dg > kTolerance || db > kTolerance || da > kTolerance)
                        {
                            ++colorMismatch;
                            if (colorMismatch <= kMaxDetails)
                            {
                                details << "\n  PV[" << curPv << "] poly=" << poly << " v=" << v
                                        << " src(" << oUv[0] << "," << oUv[1]
                                        << ")¡úpx(" << srcPx << "," << srcPy
                                        << ") RGBA=(" << static_cast<int>(srcImg.pixels[srcOff])
                                        << "," << static_cast<int>(srcImg.pixels[srcOff + 1])
                                        << "," << static_cast<int>(srcImg.pixels[srcOff + 2])
                                        << "," << static_cast<int>(srcImg.pixels[srcOff + 3])
                                        << ") | out(" << nUv[0] << "," << nUv[1]
                                        << ")¡úpx(" << atPx << "," << atPy
                                        << ") RGBA=(" << static_cast<int>(atlasImg.pixels[atOff])
                                        << "," << static_cast<int>(atlasImg.pixels[atOff + 1])
                                        << "," << static_cast<int>(atlasImg.pixels[atOff + 2])
                                        << "," << static_cast<int>(atlasImg.pixels[atOff + 3])
                                        << ")";
                            }
                        }
                    }
                }
            }

            if (colorMismatch > 0)
            {
                logger_.Warn("Verify[" + meshName + "]: " + std::to_string(colorMismatch) + "/" +
                             std::to_string(totalChecked) + " polygon-vertices sample different colors ** COLOR MISMATCH **" +
                             details.str());
            }
        }
    }
}

void BatchProcessor::CollectMeshes(fbxsdk::FbxNode* node, std::vector<fbxsdk::FbxMesh*>& meshes) const
{
    if (node == nullptr)
    {
        return;
    }

    if (fbxsdk::FbxMesh* mesh = node->GetMesh(); mesh != nullptr)
    {
        meshes.push_back(mesh);
    }

    for (int childIndex = 0; childIndex < node->GetChildCount(); ++childIndex)
    {
        CollectMeshes(node->GetChild(childIndex), meshes);
    }
}
} // namespace polyx::app
