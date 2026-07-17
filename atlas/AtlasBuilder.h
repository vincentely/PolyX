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

// True if every pixel in the block is black (RGB 0). Alpha is ignored because
// source PNGs commonly encode unused black space as opaque black.
bool IsBlockEmpty(const Image& image, int blockX, int blockY, int blockSize);

// Append point for a left-to-right, top-to-bottom atlas: scan occupied blocks
// in reverse (bottom-to-top, right-to-left), then return the next block in
// forward row-major order. Empty atlas -> (0,0); full atlas -> false.
bool FindAppendStart(const Image& image, int blockSize, int& outX, int& outY);

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
    void SetAutoSize(bool autoSize);
    bool AutoSize() const;

    bool AddTile(const std::string& key, const Image& image, const Rect& sourceRect, std::string* errorMessage = nullptr);
    bool Build(std::string* errorMessage = nullptr);

    // Incremental: seed from an existing atlas. startX/startY must be the append
    // point computed by FindAppendStart. Existing non-zero pixels are preserved.
    bool LoadBase(const Image& atlas, int startX, int startY, std::string* errorMessage = nullptr);

    // Exact pixel match of `tile` inside the base/current atlas image (8-aligned).
    bool FindTileInAtlas(const Image& tile, Rect& outAtlasRect) const;

    // Register a tile that already exists in the base atlas (content reuse).
    bool RegisterReusedTile(const std::string& key, const Rect& atlasRect, const Rect& sourceRect, std::string* errorMessage = nullptr);

    // Pack only newly AddTile'd tiles into empty regions; blit onto base image.
    bool BuildIncremental(std::string* errorMessage = nullptr);

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

    bool CalculateAutoTargetSize(const std::vector<std::size_t>& order, int& targetWidth, int& targetHeight, std::string* errorMessage) const;
    bool PackTiles(const std::vector<std::size_t>& order,
                   int targetWidth,
                   int targetHeight,
                   std::vector<AtlasEntry>* packedEntries,
                   std::string* errorMessage) const;

    bool RegionIsEmpty(int x, int y, int width, int height) const;
    bool PlaceInEmptySpace(int width, int height, int& outX, int& outY) const;
    void ReserveRegion(int x, int y, int width, int height);

    int targetWidth_ = 1024;
    int targetHeight_ = 1024;
    bool autoSize_ = false;
    bool built_ = false;
    bool incremental_ = false;
    int startX_ = 0;
    int startY_ = 0;
    std::vector<PendingTile> tiles_;
    std::unordered_map<std::string, std::size_t> tileIndexByKey_;
    std::vector<AtlasEntry> entries_;
    std::unordered_map<std::string, std::size_t> entryIndexByKey_;
    Image atlasImage_;
    int occupancyColumns_ = 0;
    int occupancyRows_ = 0;
    std::vector<std::uint8_t> occupancy_;
};
} // namespace polyx::atlas
