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

std::string ToUtf8(const std::filesystem::path& path)
{
    return path.u8string();
}

void ParseAtlasSize(const std::string& text, bool& autoSize, int& width, int& height)
{
    autoSize = true;
    width = 1024;
    height = 1024;
    if (text.empty() || text == "auto")
    {
        return;
    }

    const auto toInt = [](const std::string& token, int fallback) -> int
    {
        try
        {
            return std::max(1, std::stoi(token));
        }
        catch (...)
        {
            return fallback;
        }
    };

    const std::size_t xPos = text.find('x');
    if (xPos != std::string::npos)
    {
        width = toInt(text.substr(0, xPos), 1024);
        height = toInt(text.substr(xPos + 1), 1024);
    }
    else
    {
        width = height = toInt(text, 1024);
    }
    autoSize = false;
}

// Build "/Name/Name/..." from the FBX root's child down to this node, matching
// the transform path the Unity exporter writes.
std::string NodePath(fbxsdk::FbxNode* node)
{
    if (node == nullptr)
    {
        return "";
    }
    fbxsdk::FbxNode* root = node->GetScene() != nullptr ? node->GetScene()->GetRootNode() : nullptr;
    std::vector<std::string> names;
    for (fbxsdk::FbxNode* current = node; current != nullptr && current != root; current = current->GetParent())
    {
        names.push_back(current->GetName() != nullptr ? current->GetName() : "");
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it)
    {
        path += '/';
        path += *it;
    }
    return path.empty() ? "/" : path;
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
    atlasBuilder.SetAutoSize(config_.autoAtlasSize);
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

            auto loaderPtr = std::make_unique<fbx::FbxLoader>();
            std::string errorMessage;
            if (!loaderPtr->Load(fbxFile, &errorMessage))
            {
                logger_.Error("Failed to load FBX for analysis: " + fbxFile.string() + " | " + errorMessage);
                hadFatalError = true;
                continue;
            }

            std::vector<fbxsdk::FbxMesh*> sceneMeshes;
            CollectMeshes(loaderPtr->Scene()->GetRootNode(), sceneMeshes);
            const std::vector<const atlas::Image*> meshTextures(sceneMeshes.size(), &package.sourceTexture);
            uv::ScenePlan scenePlan = analyzer.AnalyzeScene(loaderPtr->Scene(), meshTextures, logger_);
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
            filePlan.originalLoader = std::move(loaderPtr);
            filePlans.push_back(std::move(filePlan));
        }
    }

    if (filePlans.empty())
    {
        logger_.Error("No FBX files could be analyzed successfully.");
        return false;
    }

    const std::filesystem::path atlasOutputPath = config_.outputDir / "atlas.png";
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

bool BatchProcessor::RunManifest(const manifest::Request& request,
                                 const std::filesystem::path& jsonDir,
                                 const std::filesystem::path& outputDir,
                                 manifest::Result& result)
{
    bool autoSize = true;
    int atlasW = 1024;
    int atlasH = 1024;
    ParseAtlasSize(request.atlasSize, autoSize, atlasW, atlasH);

    std::filesystem::create_directories(outputDir);
    const std::filesystem::path atlasOutputPath = outputDir / "atlas.png";

    result = manifest::Result{};
    result.atlasOut = ToUtf8(atlasOutputPath);

    // One result entry per (fbx, mesh). resultIndex[i][j] -> result.items index.
    std::vector<std::vector<std::size_t>> resultIndex(request.items.size());
    for (std::size_t i = 0; i < request.items.size(); ++i)
    {
        const manifest::RequestItem& item = request.items[i];
        for (const manifest::MeshEntry& meshEntry : item.meshes)
        {
            manifest::ResultItem resultItem;
            resultItem.fbx = item.fbx;
            resultItem.nodePath = meshEntry.nodePath;
            resultItem.mesh = meshEntry.mesh;
            resultItem.status = "error";
            resultItem.detail = "unprocessed";
            resultIndex[i].push_back(result.items.size());
            result.items.push_back(std::move(resultItem));
        }
    }

    uv::UVAnalyzer analyzer;
    std::vector<FilePlan> filePlans;
    atlas::AtlasBuilder atlasBuilder(atlasW, atlasH);
    atlasBuilder.SetAutoSize(autoSize);
    bool hadFatalError = false;

    std::unordered_map<std::string, atlas::Image> textureCache;
    const auto loadTexture = [&](const std::filesystem::path& absTexture) -> const atlas::Image*
    {
        const std::string key = ToUtf8(absTexture);
        if (const auto it = textureCache.find(key); it != textureCache.end())
        {
            return it->second.Empty() ? nullptr : &it->second;
        }
        atlas::Image image;
        std::string textureError;
        if (!atlas::LoadImageFile(absTexture, image, &textureError))
        {
            logger_.Error("Failed to load texture: " + absTexture.string() + " | " + textureError);
            textureCache.emplace(key, atlas::Image{});
            return nullptr;
        }
        return &textureCache.emplace(key, std::move(image)).first->second;
    };

    for (std::size_t i = 0; i < request.items.size(); ++i)
    {
        const manifest::RequestItem& item = request.items[i];
        if (item.meshes.empty())
        {
            continue;
        }

        const std::filesystem::path absFbx = (jsonDir / std::filesystem::u8path(item.fbx)).lexically_normal();
        const std::filesystem::path outputFbx = outputDir / std::filesystem::u8path(item.fbx);

        if (config_.verbose)
        {
            logger_.Info("Analyzing FBX: " + absFbx.string());
        }

        auto loaderPtr = std::make_unique<fbx::FbxLoader>();
        std::string fbxError;
        if (!loaderPtr->Load(absFbx, &fbxError))
        {
            logger_.Error("Failed to load FBX: " + absFbx.string() + " | " + fbxError);
            for (const std::size_t idx : resultIndex[i])
            {
                result.items[idx].status = "error";
                result.items[idx].detail = "fbx-load";
            }
            hadFatalError = true;
            continue;
        }

        std::vector<fbxsdk::FbxMesh*> sceneMeshes;
        CollectMeshes(loaderPtr->Scene()->GetRootNode(), sceneMeshes);

        // Match each scene mesh to a manifest entry (nodePath first, then name)
        // and resolve its texture. meshTextures is aligned to scene-mesh order.
        std::vector<atlas::Image> meshTextures(sceneMeshes.size());
        std::vector<int> entryMaterialCount(item.meshes.size(), -1); // -1 = unmatched
        std::vector<bool> entryTextureFailed(item.meshes.size(), false);

        for (std::size_t k = 0; k < sceneMeshes.size(); ++k)
        {
            fbxsdk::FbxMesh* sceneMesh = sceneMeshes[k];
            if (sceneMesh == nullptr)
            {
                continue;
            }
            const std::string nodePath = NodePath(sceneMesh->GetNode());
            const std::string meshName = sceneMesh->GetName() != nullptr ? sceneMesh->GetName() : "";

            int matchJ = -1;
            for (std::size_t j = 0; j < item.meshes.size(); ++j)
            {
                if (!item.meshes[j].nodePath.empty() && item.meshes[j].nodePath == nodePath)
                {
                    matchJ = static_cast<int>(j);
                    break;
                }
            }
            if (matchJ < 0 && !meshName.empty())
            {
                for (std::size_t j = 0; j < item.meshes.size(); ++j)
                {
                    if (item.meshes[j].mesh == meshName)
                    {
                        matchJ = static_cast<int>(j);
                        break;
                    }
                }
            }
            if (matchJ < 0)
            {
                continue;
            }

            const std::filesystem::path absTexture =
                (jsonDir / std::filesystem::u8path(item.meshes[matchJ].texture)).lexically_normal();
            const atlas::Image* texture = loadTexture(absTexture);
            if (texture == nullptr)
            {
                entryTextureFailed[matchJ] = true;
                continue;
            }
            meshTextures[k] = *texture;
            fbxsdk::FbxNode* node = sceneMesh->GetNode();
            entryMaterialCount[matchJ] = node != nullptr ? node->GetMaterialCount() : 0;
        }

        std::vector<const atlas::Image*> texturePtrs(meshTextures.size());
        for (std::size_t k = 0; k < meshTextures.size(); ++k)
        {
            texturePtrs[k] = meshTextures[k].Empty() ? nullptr : &meshTextures[k];
        }

        uv::ScenePlan scenePlan = analyzer.AnalyzeScene(loaderPtr->Scene(), texturePtrs, logger_);
        for (const uv::TileCandidate& tile : scenePlan.uniqueTiles)
        {
            std::string tileError;
            if (!atlasBuilder.AddTile(tile.key, tile.image, tile.sourceRect, &tileError))
            {
                logger_.Error("Failed to add atlas tile from " + absFbx.string() + " | " + tileError);
                hadFatalError = true;
            }
        }
        const std::string uvSet = scenePlan.primaryUvSetName;

        FilePlan filePlan;
        filePlan.inputFbx = absFbx;
        filePlan.outputFbx = outputFbx;
        filePlan.meshTextures = std::move(meshTextures);
        filePlan.scenePlan = std::move(scenePlan);
        filePlan.originalLoader = std::move(loaderPtr);
        filePlans.push_back(std::move(filePlan));

        for (std::size_t j = 0; j < item.meshes.size(); ++j)
        {
            manifest::ResultItem& resultItem = result.items[resultIndex[i][j]];
            if (entryTextureFailed[j])
            {
                resultItem.status = "error";
                resultItem.detail = "texture-load";
                continue;
            }
            if (entryMaterialCount[j] < 0)
            {
                resultItem.status = "error";
                resultItem.detail = "mesh-not-found";
                result.warnings.push_back("Mesh '" + item.meshes[j].nodePath + "' (" + item.meshes[j].mesh +
                                          ") not found in " + item.fbx);
                continue;
            }
            resultItem.outputFbx = ToUtf8(outputFbx);
            resultItem.uvSet = uvSet;
            if (entryMaterialCount[j] > 1)
            {
                resultItem.status = "warn";
                resultItem.detail = "submesh:" + std::to_string(entryMaterialCount[j]);
            }
            else
            {
                resultItem.status = "ok";
                resultItem.detail.clear();
            }
        }
    }

    if (filePlans.empty())
    {
        logger_.Error("No FBX entries could be analyzed from the manifest.");
        return false;
    }

    if (!BuildAtlas(filePlans, atlasBuilder, atlasOutputPath))
    {
        return false;
    }

    result.atlasWidth = atlasBuilder.GetAtlasImage().width;
    result.atlasHeight = atlasBuilder.GetAtlasImage().height;

    if (!ExportScenes(filePlans, atlasBuilder, atlasOutputPath))
    {
        hadFatalError = true;
        result.warnings.push_back("Some FBX exports failed; see log.");
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
                                const std::filesystem::path& atlasOutputPath)
{
    (void)filePlans;
    std::filesystem::create_directories(atlasOutputPath.parent_path());

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
    logger_.Info("Atlas output size: " + std::to_string(atlasBuilder.GetAtlasImage().width) + "x" +
                 std::to_string(atlasBuilder.GetAtlasImage().height));
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

        // Reuse the scene parsed during analysis as the pristine reference (it is
        // never modified), avoiding a redundant re-parse of the input file here.
        fbxsdk::FbxScene* originalScene = filePlan.originalLoader ? filePlan.originalLoader->Scene() : nullptr;
        if (originalScene == nullptr)
        {
            logger_.Error("Missing analyzed scene for reference: " + filePlan.inputFbx.string());
            hadFatalError = true;
            continue;
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

        if (!ApplyScenePlan(loader.Scene(), originalScene, filePlan, atlasBuilder, atlasOutputPath))
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

        VerifyExportedMesh(filePlan, originalScene, atlasBuilder);
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

    int texW = 0;
    int texH = 0;
    const int atlasW = atlasBuilder.GetAtlasImage().width;
    const int atlasH = atlasBuilder.GetAtlasImage().height;

    std::vector<fbxsdk::FbxMesh*> atlasedMeshes;

    const auto remapUvRegion = [&](double srcU, double srcV,
                                    const uv::RegionMapping& rm,
                                    const atlas::AtlasEntry* entry) -> fbxsdk::FbxVector2
    {
        const double pixU = srcU * static_cast<double>(texW);
        const double pixV = (1.0 - srcV) * static_cast<double>(texH);
        const double localU = (pixU - static_cast<double>(rm.originX)) / static_cast<double>(rm.width);
        const double localV = (pixV - static_cast<double>(rm.originY)) / static_cast<double>(rm.height);

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

    const std::size_t meshCount = std::min(meshes.size(), origMeshes.size());
    const std::size_t planMeshCount = filePlan.scenePlan.meshes.size();
    for (std::size_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
    {
        fbxsdk::FbxMesh* mesh = meshes[meshIndex];
        const fbxsdk::FbxMesh* origMesh = origMeshes[meshIndex];

        const uv::MeshPlan* meshPlanPtr = (meshIndex < planMeshCount)
            ? &filePlan.scenePlan.meshes[meshIndex] : nullptr;

        const atlas::Image* meshTexture = nullptr;
        if (meshIndex < filePlan.meshTextures.size() && !filePlan.meshTextures[meshIndex].Empty())
        {
            meshTexture = &filePlan.meshTextures[meshIndex];
        }
        else if (!filePlan.sourceTexture.Empty())
        {
            meshTexture = &filePlan.sourceTexture;
        }
        if (meshTexture == nullptr)
        {
            continue; // no source texture for this mesh -> leave its UVs unchanged
        }
        texW = meshTexture->width;
        texH = meshTexture->height;
        if (meshPlanPtr != nullptr && !meshPlanPtr->regions.empty())
        {
            atlasedMeshes.push_back(mesh);
        }

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
            const bool indexed = origUvElement->GetReferenceMode() == fbxsdk::FbxLayerElement::eIndexToDirect;
            const int directCount = origDirectArray.GetCount();

            std::vector<bool> dIdxWritten(directCount, false);

            int pvIdx = 0;
            for (int poly = 0; poly < origMesh->GetPolygonCount(); ++poly)
            {
                const int polySize = origMesh->GetPolygonSize(poly);

                int regionId = -1;
                if (meshPlanPtr != nullptr &&
                    poly < static_cast<int>(meshPlanPtr->polyToRegion.size()))
                {
                    regionId = meshPlanPtr->polyToRegion[poly];
                }

                const uv::RegionMapping* region = nullptr;
                const atlas::AtlasEntry* entry = nullptr;
                if (regionId >= 0 &&
                    regionId < static_cast<int>(meshPlanPtr->regions.size()))
                {
                    region = &meshPlanPtr->regions[regionId];
                    entry = atlasBuilder.FindEntry(region->tileKey);
                }

                for (int v = 0; v < polySize; ++v)
                {
                    int dIdx = pvIdx;
                    if (indexed && pvIdx < origUvElement->GetIndexArray().GetCount())
                    {
                        dIdx = origUvElement->GetIndexArray().GetAt(pvIdx);
                    }
                    ++pvIdx;

                    if (dIdx < 0 || dIdx >= directCount) continue;
                    if (region == nullptr || entry == nullptr) continue;
                    if (dIdxWritten[dIdx]) continue;

                    const fbxsdk::FbxVector2 uv = origDirectArray.GetAt(dIdx);
                    const fbxsdk::FbxVector2 newUv = remapUvRegion(uv[0], uv[1], *region, entry);
                    directArray.SetAt(dIdx, newUv);
                    dIdxWritten[dIdx] = true;
                }
            }
        }
    }
    std::filesystem::path atlasRelativePath = RelativeTo(atlasOutputPath, filePlan.outputFbx.parent_path());
    ApplySceneMaterials(scene, atlasRelativePath, filePlan.scenePlan.primaryUvSetName, atlasedMeshes);
    return true;
}

void BatchProcessor::ApplySceneMaterials(fbxsdk::FbxScene* scene,
                                        const std::filesystem::path& atlasRelativePath,
                                        const std::string& uvSetName,
                                        const std::vector<fbxsdk::FbxMesh*>& atlasedMeshes)
{
    if (scene == nullptr || atlasedMeshes.empty())
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

    for (fbxsdk::FbxMesh* mesh : atlasedMeshes)
    {
        fbxsdk::FbxNode* node = mesh != nullptr ? mesh->GetNode() : nullptr;
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

        const atlas::Image* srcPtr = nullptr;
        if (i < filePlan.meshTextures.size() && !filePlan.meshTextures[i].Empty())
        {
            srcPtr = &filePlan.meshTextures[i];
        }
        else if (!filePlan.sourceTexture.Empty())
        {
            srcPtr = &filePlan.sourceTexture;
        }

        if (!atlasImg.Empty() && srcPtr != nullptr &&
            origMesh->GetElementUVCount() > 0 && outMesh->GetElementUVCount() > 0)
        {
            const atlas::Image& srcImg = *srcPtr;
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
                                        << ")->px(" << srcPx << "," << srcPy
                                        << ") RGBA=(" << static_cast<int>(srcImg.pixels[srcOff])
                                        << "," << static_cast<int>(srcImg.pixels[srcOff + 1])
                                        << "," << static_cast<int>(srcImg.pixels[srcOff + 2])
                                        << "," << static_cast<int>(srcImg.pixels[srcOff + 3])
                                        << ") | out(" << nUv[0] << "," << nUv[1]
                                        << ")->px(" << atPx << "," << atPy
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
