#pragma once

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <string>

namespace polyx::core
{
struct AppConfig
{
    std::filesystem::path rootDir = "Bin";
    std::filesystem::path inputDir = rootDir / "input";
    std::filesystem::path outputDir = rootDir / "output";
    std::size_t requestedAtlasWidth = 1024;
    std::size_t requestedAtlasHeight = 1024;
    std::size_t atlasWidth = 1024;
    std::size_t atlasHeight = 1024;
    bool showHelp = false;
    bool verbose = true;
};

void ApplyRootDirectory(AppConfig& config, const std::filesystem::path& rootDir);
void NormalizeAtlasDimensions(AppConfig& config);
bool ParseCommandLine(int argc, char* argv[], AppConfig& config, std::string* errorMessage = nullptr);
void PrintUsage(std::ostream& os, const std::filesystem::path& executableName);
} // namespace polyx::core
