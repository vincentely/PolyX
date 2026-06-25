// Windows image codec backend (GDI+). This is the only translation unit that
// pulls in Windows.h / GDI+; the rest of the atlas module stays platform-neutral.

#include "atlas/ImageBackend.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

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

bool DecodeImageOS(const std::filesystem::path& filePath, Image& image, std::string* errorMessage)
{
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

bool EncodePngOS(const Image& image, const std::filesystem::path& filePath, std::string* errorMessage)
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
} // namespace polyx::atlas
