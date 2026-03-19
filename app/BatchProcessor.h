#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "atlas/AtlasBuilder.h"
#include "core/Config.h"
#include "core/Logger.h"
#include "fbx/FbxLoader.h"
#include "uv/UVAnalyzer.h"

namespace polyx::app
{
class BatchProcessor
{
public:
    BatchProcessor(const core::AppConfig& config, core::Logger& logger);

    bool Run();

private:
    struct PackageInfo
    {
        std::filesystem::path packageRoot;
        std::filesystem::path sourceTexturePath;
        atlas::Image sourceTexture;
        std::vector<std::filesystem::path> fbxFiles;
    };

    struct FilePlan
    {
        std::filesystem::path inputFbx;
        std::filesystem::path outputFbx;
        std::filesystem::path sourceTexturePath;
        atlas::Image sourceTexture;
        uv::ScenePlan scenePlan;
    };

    bool DiscoverPackages(std::vector<PackageInfo>& packages);
    bool FindSourceTexture(const std::filesystem::path& packageRoot, std::filesystem::path& texturePath) const;
    void CollectFbxFiles(const std::filesystem::path& packageRoot, std::vector<std::filesystem::path>& files) const;
    std::filesystem::path MakeOutputPath(const std::filesystem::path& inputFbx) const;

    bool BuildAtlas(const std::vector<FilePlan>& filePlans,
                    atlas::AtlasBuilder& atlasBuilder,
                    std::filesystem::path& atlasOutputPath);

    bool ExportScenes(const std::vector<FilePlan>& filePlans,
                      const atlas::AtlasBuilder& atlasBuilder,
                      const std::filesystem::path& atlasOutputPath);

    bool ApplyScenePlan(fbxsdk::FbxScene* scene,
                        fbxsdk::FbxScene* originalScene,
                        const FilePlan& filePlan,
                        const atlas::AtlasBuilder& atlasBuilder,
                        const std::filesystem::path& atlasOutputPath);

    void VerifyExportedMesh(const FilePlan& filePlan, fbxsdk::FbxScene* originalScene, const atlas::AtlasBuilder& atlasBuilder);

    void ApplySceneMaterials(fbxsdk::FbxScene* scene,
                             const std::filesystem::path& atlasRelativePath,
                             const std::string& uvSetName);

    void CollectMeshes(fbxsdk::FbxNode* node, std::vector<fbxsdk::FbxMesh*>& meshes) const;

    const core::AppConfig config_;
    core::Logger& logger_;
};
} // namespace polyx::app