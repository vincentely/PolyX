#include "atlas/TgaLoader.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

namespace polyx::atlas
{
namespace
{
#pragma pack(push, 1)
struct TgaHeader
{
    std::uint8_t idLength = 0;
    std::uint8_t colorMapType = 0;
    std::uint8_t imageType = 0;
    std::uint16_t colorMapFirstEntry = 0;
    std::uint16_t colorMapLength = 0;
    std::uint8_t colorMapEntrySize = 0;
    std::uint16_t xOrigin = 0;
    std::uint16_t yOrigin = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t pixelDepth = 0;
    std::uint8_t imageDescriptor = 0;
};
#pragma pack(pop)

bool ReadExact(std::istream& stream, void* buffer, std::size_t size)
{
    stream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return stream.good();
}

bool DecodePixel(std::uint8_t* rgba, const std::uint8_t* data, int bytesPerPixel, bool grayscale)
{
    if (grayscale)
    {
        rgba[0] = data[0];
        rgba[1] = data[0];
        rgba[2] = data[0];
        rgba[3] = 255;
        return true;
    }

    switch (bytesPerPixel)
    {
    case 2:
    {
        const std::uint16_t value = static_cast<std::uint16_t>(data[0]) |
                                    (static_cast<std::uint16_t>(data[1]) << 8U);
        rgba[0] = static_cast<std::uint8_t>(((value >> 10U) & 0x1FU) * 255U / 31U);
        rgba[1] = static_cast<std::uint8_t>(((value >> 5U) & 0x1FU) * 255U / 31U);
        rgba[2] = static_cast<std::uint8_t>((value & 0x1FU) * 255U / 31U);
        rgba[3] = (value & 0x8000U) != 0U ? 255U : 255U;
        return true;
    }
    case 3:
        rgba[0] = data[2];
        rgba[1] = data[1];
        rgba[2] = data[0];
        rgba[3] = 255;
        return true;
    case 4:
        rgba[0] = data[2];
        rgba[1] = data[1];
        rgba[2] = data[0];
        rgba[3] = data[3];
        return true;
    default:
        return false;
    }
}

bool WritePixel(Image& image,
                std::size_t index,
                int width,
                int height,
                bool originTop,
                bool originRight,
                const std::uint8_t* pixelData,
                int bytesPerPixel,
                bool grayscale)
{
    std::size_t row = index / static_cast<std::size_t>(width);
    std::size_t col = index % static_cast<std::size_t>(width);

    if (originRight)
    {
        col = static_cast<std::size_t>(width - 1) - col;
    }

    if (!originTop)
    {
        row = static_cast<std::size_t>(height - 1) - row;
    }

    std::uint8_t* dst = image.pixels.data() + (row * static_cast<std::size_t>(width) + col) * 4U;
    return DecodePixel(dst, pixelData, bytesPerPixel, grayscale);
}

bool LoadTgaInternal(const std::filesystem::path& filePath, Image& image, std::string* errorMessage)
{
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to open image: " + filePath.string();
        }
        return false;
    }

    TgaHeader header{};
    if (!ReadExact(stream, &header, sizeof(header)))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Failed to read TGA header: " + filePath.string();
        }
        return false;
    }

    if (header.colorMapType != 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Indexed/color-mapped TGA is not supported: " + filePath.string();
        }
        return false;
    }

    const bool grayscale = header.imageType == 3 || header.imageType == 11;
    const bool isRle = header.imageType == 10 || header.imageType == 11;
    const bool isUncompressed = header.imageType == 2 || header.imageType == 3;
    if (!isRle && !isUncompressed)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Unsupported TGA image type: " + filePath.string();
        }
        return false;
    }

    const int width = static_cast<int>(header.width);
    const int height = static_cast<int>(header.height);
    if (width <= 0 || height <= 0)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Invalid TGA image size: " + filePath.string();
        }
        return false;
    }

    const int bytesPerPixel = static_cast<int>(header.pixelDepth / 8U);
    if ((grayscale && header.pixelDepth != 8) || (!grayscale && header.pixelDepth != 16 && header.pixelDepth != 24 && header.pixelDepth != 32))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "Unsupported TGA pixel depth: " + filePath.string();
        }
        return false;
    }

    if (header.idLength > 0)
    {
        stream.ignore(header.idLength);
        if (!stream)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Failed to skip TGA ID field: " + filePath.string();
            }
            return false;
        }
    }

    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0);

    const bool originTop = (header.imageDescriptor & 0x20U) != 0U;
    const bool originRight = (header.imageDescriptor & 0x10U) != 0U;
    const std::size_t totalPixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> pixelBuffer(static_cast<std::size_t>(bytesPerPixel > 0 ? bytesPerPixel : 1));

    if (isUncompressed)
    {
        for (std::size_t i = 0; i < totalPixels; ++i)
        {
            if (!ReadExact(stream, pixelBuffer.data(), static_cast<std::size_t>(bytesPerPixel)))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Failed to read TGA pixel data: " + filePath.string();
                }
                return false;
            }

            if (!WritePixel(image, i, width, height, originTop, originRight, pixelBuffer.data(), bytesPerPixel, grayscale))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Failed to decode TGA pixel data: " + filePath.string();
                }
                return false;
            }
        }
    }
    else
    {
        std::size_t written = 0;
        while (written < totalPixels)
        {
            std::uint8_t packetHeader = 0;
            if (!ReadExact(stream, &packetHeader, sizeof(packetHeader)))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Failed to read TGA RLE packet header: " + filePath.string();
                }
                return false;
            }

            const std::size_t packetCount = static_cast<std::size_t>(packetHeader & 0x7FU) + 1U;
            if ((packetHeader & 0x80U) != 0U)
            {
                if (!ReadExact(stream, pixelBuffer.data(), static_cast<std::size_t>(bytesPerPixel)))
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = "Failed to read TGA RLE pixel data: " + filePath.string();
                    }
                    return false;
                }

                for (std::size_t i = 0; i < packetCount && written < totalPixels; ++i, ++written)
                {
                    if (!WritePixel(image, written, width, height, originTop, originRight, pixelBuffer.data(), bytesPerPixel, grayscale))
                    {
                        if (errorMessage != nullptr)
                        {
                            *errorMessage = "Failed to decode TGA RLE pixel data: " + filePath.string();
                        }
                        return false;
                    }
                }
            }
            else
            {
                for (std::size_t i = 0; i < packetCount && written < totalPixels; ++i, ++written)
                {
                    if (!ReadExact(stream, pixelBuffer.data(), static_cast<std::size_t>(bytesPerPixel)))
                    {
                        if (errorMessage != nullptr)
                        {
                            *errorMessage = "Failed to read TGA raw pixel data: " + filePath.string();
                        }
                        return false;
                    }

                    if (!WritePixel(image, written, width, height, originTop, originRight, pixelBuffer.data(), bytesPerPixel, grayscale))
                    {
                        if (errorMessage != nullptr)
                        {
                            *errorMessage = "Failed to decode TGA raw pixel data: " + filePath.string();
                        }
                        return false;
                    }
                }
            }
        }
    }

    return true;
}
} // namespace

bool LoadTgaImageFile(const std::filesystem::path& filePath, Image& image, std::string* errorMessage)
{
    image = Image{};
    return LoadTgaInternal(filePath, image, errorMessage);
}
} // namespace polyx::atlas
