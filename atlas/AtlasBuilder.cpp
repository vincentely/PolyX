#include "atlas/AtlasBuilder.h"
#include "atlas/TgaLoader.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace polyx::atlas
{
namespace
{
class GdiPlusSession
{
public:
    GdiPlusSession()
    {
        Gdiplus::GdiplusStartupInput input;
        started_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    }

    ~GdiPlusSession()
    {
        if (started_)
        {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    bool IsStarted() const
    {
        return started_;
    }

private:
    ULONG_PTR token_ = 0;
    bool started_ = false;
};

GdiPlusSession& GetGdiPlusSession()
{
    static GdiPlusSession session;
    return session;
}

bool EnsureGdiPlus(std::string* errorMessage)
{
    if (!GetGdiPlusSession().IsStarted())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to initialize GDI+.";
        }
        return false;
    }

    return true;
}

bool GetEncoderClsid(const wchar_t* mimeType, CLSID& clsid)
{
    using namespace Gdiplus;

    UINT numEncoders = 0;
    UINT bytes = 0;
    if (GetImageEncodersSize(&numEncoders, &bytes) != Ok || bytes == 0)
    {
        return false;
    }

    std::vector<std::uint8_t> buffer(bytes);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(numEncoders, bytes, encoders) != Ok)
    {
        return false;
    }

    for (UINT i = 0; i < numEncoders; ++i)
    {
        if (std::wcscmp(encoders[i].MimeType, mimeType) == 0)
        {
            clsid = encoders[i].Clsid;
            return true;
        }
    }

    return false;
}

std::wstring ToWidePath(const std::filesystem::path& path)
{
    return path.wstring();
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

    if (!EnsureGdiPlus(errorMessage))
    {
        return false;
    }

    const std::wstring widePath = ToWidePath(filePath);
    Gdiplus::Bitmap bitmap(widePath.c_str(), FALSE);
    if (bitmap.GetLastStatus() != Gdiplus::Ok)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to load image: " + filePath.string();
        }
        return false;
    }

    const int width = static_cast<int>(bitmap.GetWidth());
    const int height = static_cast<int>(bitmap.GetHeight());
    if (width <= 0 || height <= 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Invalid image size: " + filePath.string();
        }
        return false;
    }

    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);

    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData data{};
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to lock image bits: " + filePath.string();
        }
        return false;
    }

    const auto* srcBytes = static_cast<const std::uint8_t*>(data.Scan0);
    const int srcStride = data.Stride;
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;

    if (srcStride >= 0)
    {
        for (int y = 0; y < height; ++y)
        {
            const std::uint8_t* srcRow = srcBytes + static_cast<std::size_t>(y) * static_cast<std::size_t>(srcStride);
            std::uint8_t* dstRow = image.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
            std::memcpy(dstRow, srcRow, rowBytes);
        }
    }
    else
    {
        const std::size_t stride = static_cast<std::size_t>(-srcStride);
        const std::uint8_t* firstRow = srcBytes + stride * static_cast<std::size_t>(height - 1);
        for (int y = 0; y < height; ++y)
        {
            const std::uint8_t* srcRow = firstRow - static_cast<std::size_t>(y) * stride;
            std::uint8_t* dstRow = image.pixels.data() + static_cast<std::size_t>(y) * rowBytes;
            std::memcpy(dstRow, srcRow, rowBytes);
        }
    }

    bitmap.UnlockBits(&data);

    for (std::size_t i = 0; i + 3U < image.pixels.size(); i += 4U)
    {
        std::swap(image.pixels[i + 0], image.pixels[i + 2]);
    }

    return true;
}

bool SaveImagePng(const Image& image, const std::filesystem::path& filePath, std::string* errorMessage)
{
    if (!EnsureGdiPlus(errorMessage))
    {
        return false;
    }

    if (image.Empty())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Cannot save an empty image.";
        }
        return false;
    }

    Gdiplus::Bitmap bitmap(image.width, image.height, PixelFormat32bppARGB);
    Gdiplus::Rect rect(0, 0, image.width, image.height);
    Gdiplus::BitmapData data{};
    if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to lock atlas image for writing.";
        }
        return false;
    }

    const auto* srcBytes = image.pixels.data();
    auto* dstBytes = static_cast<std::uint8_t*>(data.Scan0);
    const int dstStride = data.Stride;
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 4U;

    if (dstStride >= 0)
    {
        for (int y = 0; y < image.height; ++y)
        {
            const std::uint8_t* srcRow = srcBytes + static_cast<std::size_t>(y) * rowBytes;
            std::uint8_t* dstRow = dstBytes + static_cast<std::size_t>(y) * static_cast<std::size_t>(dstStride);
            std::memcpy(dstRow, srcRow, rowBytes);
            for (std::size_t x = 0; x < rowBytes; x += 4U)
            {
                std::swap(dstRow[x + 0], dstRow[x + 2]);
            }
        }
    }
    else
    {
        const std::size_t stride = static_cast<std::size_t>(-dstStride);
        auto* firstRow = dstBytes + stride * static_cast<std::size_t>(image.height - 1);
        for (int y = 0; y < image.height; ++y)
        {
            const std::uint8_t* srcRow = srcBytes + static_cast<std::size_t>(y) * rowBytes;
            std::uint8_t* dstRow = firstRow - static_cast<std::size_t>(y) * stride;
            std::memcpy(dstRow, srcRow, rowBytes);
            for (std::size_t x = 0; x < rowBytes; x += 4U)
            {
                std::swap(dstRow[x + 0], dstRow[x + 2]);
            }
        }
    }

    bitmap.UnlockBits(&data);

    CLSID pngClsid{};
    if (!GetEncoderClsid(L"image/png", pngClsid))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to locate PNG encoder.";
        }
        return false;
    }

    const std::wstring widePath = ToWidePath(filePath);
    if (bitmap.Save(widePath.c_str(), &pngClsid, nullptr) != Gdiplus::Ok)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to save PNG atlas: " + filePath.string();
        }
        return false;
    }

    return true;
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

    if (targetWidth_ <= 0 || targetHeight_ <= 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Invalid atlas target size.";
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

    int rowX = 0;
    int rowY = 0;
    int rowHeight = 0;

    for (const std::size_t index : order)
    {
        const PendingTile& tile = tiles_[index];
        if (tile.image.width > targetWidth_)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Tile wider than atlas target width: " + tile.key;
            }
            return false;
        }

        if (rowX > 0 && rowX + tile.image.width > targetWidth_)
        {
            rowY += rowHeight;
            rowX = 0;
            rowHeight = 0;
        }

        if (rowY + tile.image.height > targetHeight_)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Atlas target height is too small for tile: " + tile.key;
            }
            return false;
        }

        AtlasEntry entry;
        entry.key = tile.key;
        entry.sourceRect = tile.sourceRect;
        entry.atlasRect = Rect{ rowX, rowY, tile.image.width, tile.image.height };
        entryIndexByKey_.emplace(entry.key, entries_.size());
        entries_.push_back(std::move(entry));

        rowX += tile.image.width;
        rowHeight = std::max(rowHeight, tile.image.height);
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