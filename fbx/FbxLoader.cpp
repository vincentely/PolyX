#include "fbx/FbxLoader.h"

#include <fbxsdk.h>

#include <memory>

namespace polyx::fbx
{
namespace
{
struct ImporterDeleter
{
    void operator()(fbxsdk::FbxImporter* importer) const
    {
        if (importer != nullptr)
        {
            importer->Destroy();
        }
    }
};

struct ExporterDeleter
{
    void operator()(fbxsdk::FbxExporter* exporter) const
    {
        if (exporter != nullptr)
        {
            exporter->Destroy();
        }
    }
};

using ImporterPtr = std::unique_ptr<fbxsdk::FbxImporter, ImporterDeleter>;
using ExporterPtr = std::unique_ptr<fbxsdk::FbxExporter, ExporterDeleter>;
} // namespace

FbxLoader::FbxLoader() = default;

FbxLoader::~FbxLoader()
{
    Unload();
}

bool FbxLoader::Load(const std::filesystem::path& filePath, std::string* errorMessage)
{
    Unload();

    manager_ = fbxsdk::FbxManager::Create();
    if (manager_ == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to create FBX manager.";
        }
        return false;
    }

    fbxsdk::FbxIOSettings* ioSettings = fbxsdk::FbxIOSettings::Create(manager_, IOSROOT);
    manager_->SetIOSettings(ioSettings);

    scene_ = fbxsdk::FbxScene::Create(manager_, "PolyXScene");
    if (scene_ == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to create FBX scene.";
        }
        Unload();
        return false;
    }

    ImporterPtr importer(fbxsdk::FbxImporter::Create(manager_, ""));
    const std::string filePathString = filePath.string();
    if (!importer || !importer->Initialize(filePathString.c_str(), -1, manager_->GetIOSettings()))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = importer ? importer->GetStatus().GetErrorString() : "Failed to create FBX importer.";
        }

        importer.reset();
        Unload();
        return false;
    }

    if (!importer->Import(scene_))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = importer->GetStatus().GetErrorString();
        }

        importer.reset();
        Unload();
        return false;
    }

    loadedFile_ = filePath;
    return true;
}

bool FbxLoader::Save(const std::filesystem::path& filePath, std::string* errorMessage) const
{
    if (manager_ == nullptr || scene_ == nullptr)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "No FBX scene is loaded.";
        }
        return false;
    }

    ExporterPtr exporter(fbxsdk::FbxExporter::Create(manager_, ""));
    if (!exporter)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to create FBX exporter.";
        }
        return false;
    }

    const std::string filePathString = filePath.string();
    if (!exporter->Initialize(filePathString.c_str(), -1, manager_->GetIOSettings()))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = exporter->GetStatus().GetErrorString();
        }
        return false;
    }

    if (!exporter->Export(scene_))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = exporter->GetStatus().GetErrorString();
        }
        return false;
    }

    return true;
}

void FbxLoader::Unload()
{
    if (scene_ != nullptr)
    {
        scene_->Destroy();
        scene_ = nullptr;
    }

    if (manager_ != nullptr)
    {
        manager_->Destroy();
        manager_ = nullptr;
    }

    loadedFile_.clear();
}

bool FbxLoader::IsLoaded() const
{
    return scene_ != nullptr;
}

std::size_t FbxLoader::MeshCount() const
{
    if (scene_ == nullptr)
    {
        return 0;
    }

    return CountMeshesRecursive(scene_->GetRootNode());
}

std::size_t FbxLoader::CountMeshesRecursive(fbxsdk::FbxNode* node) const
{
    if (node == nullptr)
    {
        return 0;
    }

    std::size_t meshCount = node->GetMesh() != nullptr ? 1U : 0U;
    for (int i = 0; i < node->GetChildCount(); ++i)
    {
        meshCount += CountMeshesRecursive(node->GetChild(i));
    }
    return meshCount;
}

void FbxLoader::PrintBasicInfo(std::ostream& os) const
{
    if (scene_ == nullptr)
    {
        os << "No FBX scene loaded.\n";
        return;
    }

    os << "File: " << loadedFile_.string() << '\n';
    os << "Scene: " << scene_->GetName() << '\n';
    os << "Mesh Count: " << MeshCount() << '\n';

    const auto printNode = [&](fbxsdk::FbxNode* node, const auto& self) -> void
    {
        if (node == nullptr)
        {
            return;
        }

        if (const fbxsdk::FbxMesh* mesh = node->GetMesh(); mesh != nullptr)
        {
            os << "  Mesh Node: " << node->GetName()
               << ", Control Points: " << mesh->GetControlPointsCount()
               << ", Polygons: " << mesh->GetPolygonCount() << '\n';
        }

        for (int i = 0; i < node->GetChildCount(); ++i)
        {
            self(node->GetChild(i), self);
        }
    };

    printNode(scene_->GetRootNode(), printNode);
}

fbxsdk::FbxManager* FbxLoader::Manager() const
{
    return manager_;
}

fbxsdk::FbxScene* FbxLoader::Scene() const
{
    return scene_;
}

const std::filesystem::path& FbxLoader::LoadedFile() const
{
    return loadedFile_;
}
} // namespace polyx::fbx