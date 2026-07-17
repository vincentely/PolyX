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

bool IsBlockEmpty(const Image& image, int blockX, int blockY, int blockSize)
{
    if (image.Empty() || blockSize <= 0 || blockX < 0 || blockY < 0 ||
        blockX >= image.width || blockY >= image.height)
    {
        return false;
    }

    const int x1 = std::min(blockX + blockSize, image.width);
    const int y1 = std::min(blockY + blockSize, image.height);
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;
    for (int y = blockY; y < y1; ++y)
    {
        const std::uint8_t* row = image.pixels.data() + static_cast<std::size_t>(y) * rowBytes + static_cast<std::size_t>(blockX) * 4U;
        for (int x = blockX; x < x1; ++x)
        {
            const std::size_t i = static_cast<std::size_t>(x - blockX) * 4U;
            if (row[i + 0U] != 0 || row[i + 1U] != 0 || row[i + 2U] != 0)
            {
                return false;
            }
        }
    }
    return true;
}

bool FindAppendStart(const Image& image, int blockSize, int& outX, int& outY)
{
    outX = 0;
    outY = 0;
    if (image.Empty() || blockSize <= 0 ||
        image.width % blockSize != 0 || image.height % blockSize != 0)
    {
        return false;
    }

    // Reverse of the packer's forward row order. The first occupied block found
    // is the last used block; append at its forward successor.
    for (int y = image.height - blockSize; y >= 0; y -= blockSize)
    {
        for (int x = image.width - blockSize; x >= 0; x -= blockSize)
        {
            if (!IsBlockEmpty(image, x, y, blockSize))
            {
                outX = x + blockSize;
                outY = y;
                if (outX >= image.width)
                {
                    outX = 0;
                    outY += blockSize;
                }
                if (outY >= image.height)
                {
                    return false;
                }
                return true;
            }
        }
    }

    // No occupied block: append from the atlas origin.
    return true;
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

bool AtlasBuilder::RegionIsEmpty(int x, int y, int width, int height) const
{
    const int block = core::kTextureBlockSize;
    if (atlasImage_.Empty() || occupancy_.empty() || x < 0 || y < 0 ||
        width <= 0 || height <= 0 || x % block != 0 || y % block != 0 ||
        x + width > atlasImage_.width || y + height > atlasImage_.height)
    {
        return false;
    }

    const int firstColumn = x / block;
    const int firstRow = y / block;
    const int lastColumn = (x + width + block - 1) / block;
    const int lastRow = (y + height + block - 1) / block;
    for (int row = firstRow; row < lastRow; ++row)
    {
        for (int column = firstColumn; column < lastColumn; ++column)
        {
            const std::size_t index =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(occupancyColumns_) +
                static_cast<std::size_t>(column);
            if (occupancy_[index] != 0)
            {
                return false;
            }
        }
    }
    return true;
}

void AtlasBuilder::ReserveRegion(int x, int y, int width, int height)
{
    const int block = core::kTextureBlockSize;
    const int firstColumn = x / block;
    const int firstRow = y / block;
    const int lastColumn = (x + width + block - 1) / block;
    const int lastRow = (y + height + block - 1) / block;
    for (int row = firstRow; row < lastRow; ++row)
    {
        for (int column = firstColumn; column < lastColumn; ++column)
        {
            const std::size_t index =
                static_cast<std::size_t>(row) * static_cast<std::size_t>(occupancyColumns_) +
                static_cast<std::size_t>(column);
            occupancy_[index] = 1;
        }
    }
}

bool AtlasBuilder::PlaceInEmptySpace(int width, int height, int& outX, int& outY) const
{
    const int block = core::kTextureBlockSize;
    if (width <= 0 || height <= 0 || atlasImage_.Empty())
    {
        return false;
    }

    // Scan only forward from the append point. Earlier black blocks may be
    // intentional black content or packing gaps, so incremental mode must not
    // go back and fill them.
    const auto tryFrom = [&](int originX, int originY) -> bool
    {
        for (int y = originY; y + height <= atlasImage_.height; y += block)
        {
            const int x0 = (y == originY) ? originX : 0;
            for (int x = x0; x + width <= atlasImage_.width; x += block)
            {
                if (RegionIsEmpty(x, y, width, height))
                {
                    outX = x;
                    outY = y;
                    return true;
                }
            }
        }
        return false;
    };

    if (tryFrom(startX_, startY_))
    {
        return true;
    }
    return false;
}

bool AtlasBuilder::LoadBase(const Image& atlas, int startX, int startY, std::string* errorMessage)
{
    if (atlas.Empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Base atlas image is empty.";
        }
        return false;
    }

    const int block = core::kTextureBlockSize;
    if (atlas.width % block != 0 || atlas.height % block != 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Base atlas dimensions must be divisible by the texture block size.";
        }
        return false;
    }

    if (startX < 0 || startY < 0 || startX % block != 0 || startY % block != 0 ||
        startX + block > atlas.width || startY + block > atlas.height)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Incremental startX/startY must be non-negative, "
                            "block-aligned, and inside the atlas.";
        }
        return false;
    }

    if (!IsBlockEmpty(atlas, startX, startY, block))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Incremental start cell is not empty.";
        }
        return false;
    }

    int appendX = 0;
    int appendY = 0;
    if (!FindAppendStart(atlas, block, appendX, appendY))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Base atlas has no append space after its last occupied 8x8 block.";
        }
        return false;
    }
    if (appendX != startX || appendY != startY)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Incremental startX/startY is not the row-major append point "
                            "(expected " +
                            std::to_string(appendX) + "," + std::to_string(appendY) + ").";
        }
        return false;
    }

    tiles_.clear();
    tileIndexByKey_.clear();
    entries_.clear();
    entryIndexByKey_.clear();

    atlasImage_ = atlas;
    targetWidth_ = atlas.width;
    targetHeight_ = atlas.height;
    autoSize_ = false;
    incremental_ = true;
    startX_ = startX;
    startY_ = startY;
    occupancyColumns_ = atlas.width / block;
    occupancyRows_ = atlas.height / block;
    occupancy_.assign(
        static_cast<std::size_t>(occupancyColumns_) * static_cast<std::size_t>(occupancyRows_), 0);
    for (int row = 0; row < occupancyRows_; ++row)
    {
        for (int column = 0; column < occupancyColumns_; ++column)
        {
            if (!IsBlockEmpty(atlas, column * block, row * block, block))
            {
                occupancy_[static_cast<std::size_t>(row) * static_cast<std::size_t>(occupancyColumns_) +
                           static_cast<std::size_t>(column)] = 1;
            }
        }
    }
    built_ = false;
    return true;
}

bool AtlasBuilder::FindTileInAtlas(const Image& tile, Rect& outAtlasRect) const
{
    outAtlasRect = Rect{};
    if (tile.Empty() || atlasImage_.Empty())
    {
        return false;
    }
    if (tile.width > atlasImage_.width || tile.height > atlasImage_.height)
    {
        return false;
    }

    const int block = core::kTextureBlockSize;
    const std::size_t atlasRow = static_cast<std::size_t>(atlasImage_.width) * 4U;
    const std::size_t tileRow = static_cast<std::size_t>(tile.width) * 4U;

    for (int y = 0; y + tile.height <= atlasImage_.height; y += block)
    {
        for (int x = 0; x + tile.width <= atlasImage_.width; x += block)
        {
            // An empty black region can have the same pixels as a solid-black
            // tile; only reuse content from a region classified as occupied.
            if (RegionIsEmpty(x, y, tile.width, tile.height))
            {
                continue;
            }

            bool match = true;
            for (int row = 0; row < tile.height && match; ++row)
            {
                const std::uint8_t* a = atlasImage_.pixels.data() +
                    static_cast<std::size_t>(y + row) * atlasRow + static_cast<std::size_t>(x) * 4U;
                const std::uint8_t* t = tile.pixels.data() + static_cast<std::size_t>(row) * tileRow;
                if (std::memcmp(a, t, tileRow) != 0)
                {
                    match = false;
                }
            }
            if (match)
            {
                outAtlasRect = Rect{ x, y, tile.width, tile.height };
                return true;
            }
        }
    }
    return false;
}

bool AtlasBuilder::RegisterReusedTile(const std::string& key, const Rect& atlasRect, const Rect& sourceRect, std::string* errorMessage)
{
    if (key.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Tile key is empty.";
        }
        return false;
    }

    if (const auto it = entryIndexByKey_.find(key); it != entryIndexByKey_.end())
    {
        const AtlasEntry& existing = entries_[it->second];
        if (existing.atlasRect.x != atlasRect.x || existing.atlasRect.y != atlasRect.y ||
            existing.atlasRect.width != atlasRect.width || existing.atlasRect.height != atlasRect.height)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Reused tile key maps to conflicting atlas rect: " + key;
            }
            return false;
        }
        return true;
    }

    AtlasEntry entry;
    entry.key = key;
    entry.sourceRect = sourceRect;
    entry.atlasRect = atlasRect;
    entryIndexByKey_.emplace(key, entries_.size());
    entries_.push_back(std::move(entry));
    return true;
}

bool AtlasBuilder::BuildIncremental(std::string* errorMessage)
{
    if (!incremental_)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "BuildIncremental requires LoadBase first.";
        }
        return false;
    }

    if (entries_.empty() && tiles_.empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "No tiles to place in incremental atlas build.";
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

    for (const std::size_t index : order)
    {
        const PendingTile& tile = tiles_[index];
        if (entryIndexByKey_.find(tile.key) != entryIndexByKey_.end())
        {
            continue; // already registered as reused
        }

        int px = 0;
        int py = 0;
        if (!PlaceInEmptySpace(tile.image.width, tile.image.height, px, py))
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Incremental atlas has no free space for tile: " + tile.key;
            }
            return false;
        }

        Blit(atlasImage_, tile.image, px, py);
        ReserveRegion(px, py, tile.image.width, tile.image.height);

        AtlasEntry entry;
        entry.key = tile.key;
        entry.sourceRect = tile.sourceRect;
        entry.atlasRect = Rect{ px, py, tile.image.width, tile.image.height };
        entryIndexByKey_.emplace(entry.key, entries_.size());
        entries_.push_back(std::move(entry));
    }

    built_ = true;
    return true;
}

bool AtlasBuilder::Build(std::string* errorMessage)
{
    if (incremental_)
    {
        return BuildIncremental(errorMessage);
    }

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
