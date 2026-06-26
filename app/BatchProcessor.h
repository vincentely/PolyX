#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/Manifest.h"
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

    // Folder mode: discover input/ packages and write to output/.
    bool Run();

    // Manifest mode: process the request, writing FBXs + atlas under outputDir
    // (FBX paths mirror their manifest-relative layout) and filling `result`.
    bool RunManifest(const manifest::Request& request,
                     const std::filesystem::path& jsonDir,
                     const std::filesystem::path& outputDir,
                     manifest::Result& result);

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
        // Per-mesh (scene order) -> per material slot (submesh) source textures.
        std::vector<std::vector<atlas::Image>> meshMaterialTextures;
        uv::ScenePlan scenePlan;
        // Scene loaded during analysis, kept alive (pristine) to reuse as the
        // export-time reference instead of re-loading the input file.
        std::unique_ptr<fbx::FbxLoader> originalLoader;
    };

    bool DiscoverPackages(std::vector<PackageInfo>& packages);
    bool FindSourceTexture(const std::filesystem::path& packageRoot, std::filesystem::path& texturePath) const;
    void CollectFbxFiles(const std::filesystem::path& packageRoot, std::vector<std::filesystem::path>& files) const;
    std::filesystem::path MakeOutputPath(const std::filesystem::path& inputFbx) const;

    bool BuildAtlas(const std::vector<FilePlan>& filePlans,
                    atlas::AtlasBuilder& atlasBuilder,
                    const std::filesystem::path& atlasOutputPath);

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
                             const std::string& uvSetName,
                             const std::vector<fbxsdk::FbxMesh*>& atlasedMeshes);

    void CollectMeshes(fbxsdk::FbxNode* node, std::vector<fbxsdk::FbxMesh*>& meshes) const;

    const core::AppConfig config_;
    core::Logger& logger_;
};
} // namespace polyx::app