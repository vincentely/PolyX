#pragma once

#include <filesystem>
#include <string>

#include "atlas/AtlasBuilder.h"

namespace polyx::atlas
{
// OS-specific image codec backend. The Windows implementation
// (ImageBackend_Windows.cpp) uses GDI+; a future platform supplies its own
// translation unit. The portable LoadImageFile/SaveImagePng dispatch here.
bool DecodeImageOS(const std::filesystem::path& filePath, Image& image, std::string* errorMessage);
bool EncodePngOS(const Image& image, const std::filesystem::path& filePath, std::string* errorMessage);
} // namespace polyx::atlas
