#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace polyx::atlas
{
struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    bool Empty() const;
    std::size_t ByteSize() const;
};

struct Rect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

bool LoadImageFile(const std::filesystem::path& filePath, Image& image, std::string* errorMessage = nullptr);
bool SaveImagePng(const Image& image, const std::filesystem::path& filePath, std::string* errorMessage = nullptr);
Image ExtractSubImage(const Image& image, const Rect& rect, std::string* errorMessage = nullptr);
void Blit(Image& destination, const Image& source, int destinationX, int destinationY);

struct AtlasEntry
{
    std::string key;
    Rect sourceRect;
    Rect atlasRect;
};

class AtlasBuilder
{
public:
    explicit AtlasBuilder(int targetWidth = 1024, int targetHeight = 1024);

    void SetTargetSize(int targetWidth, int targetHeight);
    int TargetWidth() const;
    int TargetHeight() const;

    bool AddTile(const std::string& key, const Image& image, const Rect& sourceRect, std::string* errorMessage = nullptr);
    bool Build(std::string* errorMessage = nullptr);
    const AtlasEntry* FindEntry(const std::string& key) const;
    const Image& GetAtlasImage() const;
    const std::vector<AtlasEntry>& Entries() const;
    bool SaveAtlas(const std::filesystem::path& filePath, std::string* errorMessage = nullptr) const;

private:
    struct PendingTile
    {
        std::string key;
        Image image;
        Rect sourceRect;
    };

    int targetWidth_ = 1024;
    int targetHeight_ = 1024;
    bool built_ = false;
    std::vector<PendingTile> tiles_;
    std::unordered_map<std::string, std::size_t> tileIndexByKey_;
    std::vector<AtlasEntry> entries_;
    std::unordered_map<std::string, std::size_t> entryIndexByKey_;
    Image atlasImage_;
};
} // namespace polyx::atlas
