#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace fbxsdk
{
class FbxManager;
class FbxScene;
class FbxNode;
} // namespace fbxsdk

namespace polyx::fbx
{
class FbxLoader
{
public:
    FbxLoader();
    ~FbxLoader();

    FbxLoader(const FbxLoader&) = delete;
    FbxLoader& operator=(const FbxLoader&) = delete;

    bool Load(const std::filesystem::path& filePath, std::string* errorMessage = nullptr);
    bool Save(const std::filesystem::path& filePath, std::string* errorMessage = nullptr) const;
    void Unload();

    bool IsLoaded() const;
    std::size_t MeshCount() const;
    void PrintBasicInfo(std::ostream& os) const;

    fbxsdk::FbxManager* Manager() const;
    fbxsdk::FbxScene* Scene() const;
    const std::filesystem::path& LoadedFile() const;

private:
    fbxsdk::FbxManager* manager_ = nullptr;
    fbxsdk::FbxScene* scene_ = nullptr;
    std::filesystem::path loadedFile_;

    std::size_t CountMeshesRecursive(fbxsdk::FbxNode* node) const;
};
} // namespace polyx::fbx