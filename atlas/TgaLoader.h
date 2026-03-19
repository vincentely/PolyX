#pragma once

#include <filesystem>
#include <string>

#include "atlas/AtlasBuilder.h"

namespace polyx::atlas
{
bool LoadTgaImageFile(const std::filesystem::path& filePath, Image& image, std::string* errorMessage = nullptr);
} // namespace polyx::atlas
