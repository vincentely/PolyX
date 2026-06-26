#include "atlas/AtlasBuilder.h"
#include "atlas/ImageBackend.h"
#include "atlas/TgaLoader.h"

#include "core/Constants.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

namespace polyx::atlas
{
namespace
{
bool RoundUpToPowerOfTwoInt(std::uint64_t value, int& result)
{
    constexpr int kMaxPowerOfTwoInt = 1 << 30;
    if (value <= 1U)
    {
        result = 1;
        return true;
    }

    std::uint64_t size = 1U;
    while (size < value)
    {
        size <<= 1U;
        if (size > static_cast<std::uint64_t>(kMaxPowerOfTwoInt))
        {
            return false;
        }
    }

    result = static_cast<int>(size);
    return true;
}

} // namespace

bool Image::Empty() const
{
    return width <= 0 || height <= 0 || pixels.empty();
}

std::size_t Image::ByteSize() const
{
    return pixels.size();
}

bool LoadImageFile(const std::filesystem::path& filePath, Image& image, std::string* errorMessage)
{
    image = Image{};

    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".tga")
    {
        return LoadTgaImageFile(filePath, image, errorMessage);
    }

    return DecodeImageOS(filePath, image, errorMessage);
}

bool SaveImagePng(const Image& image, const std::filesystem::path& filePath, std::string* errorMessage)
{
    return EncodePngOS(image, filePath, errorMessage);
}

Image ExtractSubImage(const Image& image, const Rect& rect, std::string* errorMessage)
{
    Image result;

    if (image.Empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Source image is empty.";
        }
        return result;
    }

    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        rect.x + rect.width > image.width || rect.y + rect.height > image.height)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Requested sub-image is out of bounds.";
        }
        return result;
    }

    result.width = rect.width;
    result.height = rect.height;
    result.pixels.resize(static_cast<std::size_t>(rect.width) * static_cast<std::size_t>(rect.height) * 4U);

    const std::size_t srcRowBytes = static_cast<std::size_t>(image.width) * 4U;
    const std::size_t dstRowBytes = static_cast<std::size_t>(rect.width) * 4U;
    for (int y = 0; y < rect.height; ++y)
    {
        const std::uint8_t* srcRow = image.pixels.data() + static_cast<std::size_t>(rect.y + y) * srcRowBytes + static_cast<std::size_t>(rect.x) * 4U;
        std::uint8_t* dstRow = result.pixels.data() + static_cast<std::size_t>(y) * dstRowBytes;
        std::memcpy(dstRow, srcRow, dstRowBytes);
    }

    return result;
}

void Blit(Image& destination, const Image& source, int destinationX, int destinationY)
{
    if (destination.Empty() || source.Empty())
    {
        return;
    }

    const std::size_t dstRowBytes = static_cast<std::size_t>(destination.width) * 4U;
    const std::size_t srcRowBytes = static_cast<std::size_t>(source.width) * 4U;
    for (int y = 0; y < source.height; ++y)
    {
        std::uint8_t* dstRow = destination.pixels.data() + static_cast<std::size_t>(destinationY + y) * dstRowBytes + static_cast<std::size_t>(destinationX) * 4U;
        const std::uint8_t* srcRow = source.pixels.data() + static_cast<std::size_t>(y) * srcRowBytes;
        std::memcpy(dstRow, srcRow, srcRowBytes);
    }
}

AtlasBuilder::AtlasBuilder(int targetWidth, int targetHeight)
    : targetWidth_(targetWidth)
    , targetHeight_(targetHeight)
{
}

void AtlasBuilder::SetTargetSize(int targetWidth, int targetHeight)
{
    targetWidth_ = targetWidth;
    targetHeight_ = targetHeight;
    autoSize_ = false;
    built_ = false;
}

int AtlasBuilder::TargetWidth() const
{
    return targetWidth_;
}

int AtlasBuilder::TargetHeight() const
{
    return targetHeight_;
}

void AtlasBuilder::SetAutoSize(bool autoSize)
{
    autoSize_ = autoSize;
    built_ = false;
}

bool AtlasBuilder::AutoSize() const
{
    return autoSize_;
}

bool AtlasBuilder::AddTile(const std::string& key, const Image& image, const Rect& sourceRect, std::string* errorMessage)
{
    if (key.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Tile key is empty.";
        }
        return false;
    }

    if (image.Empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Tile image is empty.";
        }
        return false;
    }

    const auto found = tileIndexByKey_.find(key);
    if (found != tileIndexByKey_.end())
    {
        const PendingTile& existing = tiles_[found->second];
        if (existing.image.width != image.width || existing.image.height != image.height || existing.image.pixels != image.pixels)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Tile key collision with different image content.";
            }
            return false;
        }
        return true;
    }

    PendingTile tile;
    tile.key = key;
    tile.image = image;
    tile.sourceRect = sourceRect;
    tileIndexByKey_.emplace(tile.key, tiles_.size());
    tiles_.push_back(std::move(tile));
    built_ = false;
    return true;
}

bool AtlasBuilder::CalculateAutoTargetSize(const std::vector<std::size_t>& order, int& targetWidth, int& targetHeight, std::string* errorMessage) const
{
    int maxTileDimension = 1;
    for (const PendingTile& tile : tiles_)
    {
        maxTileDimension = std::max(maxTileDimension, std::max(tile.image.width, tile.image.height));
    }

    // Start at the smallest power-of-two square that fits the largest tile, then
    // grow one dimension at a time -- double the smaller side (ties grow height) --
    // e.g. 1024x1024 -> 1024x2048 -> 2048x2048, instead of jumping to the next
    // square. Downstream UV remap uses width/height independently, so a rectangular
    // atlas is fine and wastes less space.
    int startSize = 0;
    if (!RoundUpToPowerOfTwoInt(static_cast<std::uint64_t>(maxTileDimension), startSize))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Auto atlas size exceeds supported dimensions.";
        }
        return false;
    }

    int width = startSize;
    int height = startSize;
    constexpr int kMaxAtlasDimension = 1 << 14; // 16384

    while (true)
    {
        if (PackTiles(order, width, height, nullptr, nullptr))
        {
            targetWidth = width;
            targetHeight = height;
            return true;
        }

        if (width >= kMaxAtlasDimension && height >= kMaxAtlasDimension)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Auto atlas size exceeds supported dimensions.";
            }
            return false;
        }

        if (height <= width && height < kMaxAtlasDimension)
        {
            height *= 2;
        }
        else if (width < kMaxAtlasDimension)
        {
            width *= 2;
        }
        else
        {
            height *= 2;
        }
    }
}

bool AtlasBuilder::PackTiles(const std::vector<std::size_t>& order,
                             int targetWidth,
                             int targetHeight,
                             std::vector<AtlasEntry>* packedEntries,
                             std::string* errorMessage) const
{
    if (targetWidth <= 0 || targetHeight <= 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Invalid atlas target size.";
        }
        return false;
    }

    // Skyline bottom-left packing: better space utilization than shelf packing,
    // while still guaranteeing non-overlapping, in-bounds placement. Each skyline
    // node is a horizontal segment {x, y, width} describing the current top profile;
    // the segments always tile [0, targetWidth) with no gaps.
    const int gutter = core::kAtlasGutter; // zero by project rule (see core/Constants.h)

    struct Node
    {
        int x;
        int y;
        int width;
    };

    std::vector<Node> skyline;
    skyline.push_back(Node{ 0, 0, targetWidth });

    // Lowest (then leftmost) y at which a w-wide, h-tall rect fits; false if none.
    const auto fit = [&](int w, int h, int& outX, int& outY) -> bool
    {
        int bestY = 0;
        int bestX = 0;
        bool found = false;
        for (std::size_t i = 0; i < skyline.size(); ++i)
        {
            const int x = skyline[i].x;
            if (x + w > targetWidth)
            {
                continue;
            }

            int y = 0;
            int remaining = w;
            for (std::size_t j = i; j < skyline.size() && remaining > 0; ++j)
            {
                y = std::max(y, skyline[j].y);
                remaining -= skyline[j].width;
            }
            if (remaining > 0)
            {
                continue; // not enough width to the right edge
            }
            if (y + h > targetHeight)
            {
                continue;
            }
            if (!found || y < bestY || (y == bestY && x < bestX))
            {
                found = true;
                bestY = y;
                bestX = x;
            }
        }
        if (!found)
        {
            return false;
        }
        outX = bestX;
        outY = bestY;
        return true;
    };

    // Raise the skyline over [x, x+width) to `height`, then merge equal-height runs.
    const auto raiseSkyline = [&](int x, int width, int height)
    {
        std::vector<Node> updated;
        updated.push_back(Node{ x, height, width });
        const int cx0 = x;
        const int cx1 = x + width;
        for (const Node& n : skyline)
        {
            const int nx0 = n.x;
            const int nx1 = n.x + n.width;
            if (nx1 <= cx0 || nx0 >= cx1)
            {
                updated.push_back(n);
                continue;
            }
            if (nx0 < cx0)
            {
                updated.push_back(Node{ nx0, n.y, cx0 - nx0 });
            }
            if (nx1 > cx1)
            {
                updated.push_back(Node{ cx1, n.y, nx1 - cx1 });
            }
        }

        std::sort(updated.begin(), updated.end(), [](const Node& a, const Node& b) { return a.x < b.x; });

        skyline.clear();
        for (const Node& n : updated)
        {
            if (!skyline.empty() && skyline.back().y == n.y && skyline.back().x + skyline.back().width == n.x)
            {
                skyline.back().width += n.width;
            }
            else
            {
                skyline.push_back(n);
            }
        }
    };

    for (const std::size_t index : order)
    {
        const PendingTile& tile = tiles_[index];
        const int w = tile.image.width;
        const int h = tile.image.height;
        if (w > targetWidth)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Tile wider than atlas target width: " + tile.key;
            }
            return false;
        }

        int px = 0;
        int py = 0;
        if (!fit(w, h, px, py))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Atlas target is too small for tile: " + tile.key;
            }
            return false;
        }

        AtlasEntry entry;
        entry.key = tile.key;
        entry.sourceRect = tile.sourceRect;
        entry.atlasRect = Rect{ px, py, w, h };
        if (packedEntries != nullptr)
        {
            packedEntries->push_back(std::move(entry));
        }

        // Reserve the tile footprint plus gutter, clamped to the atlas width.
        const int reservedWidth = std::min(w + gutter, targetWidth - px);
        raiseSkyline(px, reservedWidth, py + h + gutter);
    }

    return true;
}

bool AtlasBuilder::Build(std::string* errorMessage)
{
    entries_.clear();
    entryIndexByKey_.clear();
    atlasImage_ = Image{};

    if (tiles_.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "No tiles were added to the atlas builder.";
        }
        return false;
    }

    std::vector<std::size_t> order(tiles_.size());
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }

    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs)
    {
        const PendingTile& a = tiles_[lhs];
        const PendingTile& b = tiles_[rhs];
        if (a.image.height != b.image.height)
        {
            return a.image.height > b.image.height;
        }
        if (a.image.width != b.image.width)
        {
            return a.image.width > b.image.width;
        }
        return a.key < b.key;
    });

    if (autoSize_)
    {
        int autoWidth = 0;
        int autoHeight = 0;
        if (!CalculateAutoTargetSize(order, autoWidth, autoHeight, errorMessage))
        {
            return false;
        }
        targetWidth_ = autoWidth;
        targetHeight_ = autoHeight;
    }

    if (!PackTiles(order, targetWidth_, targetHeight_, &entries_, errorMessage))
    {
        return false;
    }

    for (std::size_t i = 0; i < entries_.size(); ++i)
    {
        entryIndexByKey_.emplace(entries_[i].key, i);
    }

    atlasImage_.width = targetWidth_;
    atlasImage_.height = targetHeight_;
    atlasImage_.pixels.assign(static_cast<std::size_t>(targetWidth_) * static_cast<std::size_t>(targetHeight_) * 4U, 0);

    for (const AtlasEntry& entry : entries_)
    {
        const PendingTile& tile = tiles_[tileIndexByKey_.at(entry.key)];
        Blit(atlasImage_, tile.image, entry.atlasRect.x, entry.atlasRect.y);
    }

    built_ = true;
    return true;
}

const AtlasEntry* AtlasBuilder::FindEntry(const std::string& key) const
{
    const auto it = entryIndexByKey_.find(key);
    if (it == entryIndexByKey_.end())
    {
        return nullptr;
    }

    return &entries_[it->second];
}

const Image& AtlasBuilder::GetAtlasImage() const
{
    return atlasImage_;
}

const std::vector<AtlasEntry>& AtlasBuilder::Entries() const
{
    return entries_;
}

bool AtlasBuilder::SaveAtlas(const std::filesystem::path& filePath, std::string* errorMessage) const
{
    if (!built_)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Atlas has not been built yet.";
        }
        return false;
    }

    return SaveImagePng(atlasImage_, filePath, errorMessage);
}
} // namespace polyx::atlas
