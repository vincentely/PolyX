#include "core/Config.h"

#include <cstdlib>
#include <limits>
#include <string>

namespace polyx::core
{
namespace
{
bool TryParseSizeT(const std::string& text, std::size_t& value)
{
    if (text.empty())
    {
        return false;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0')
    {
        return false;
    }

    if (parsed == 0ULL)
    {
        return false;
    }

    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }

    value = static_cast<std::size_t>(parsed);
    return true;
}

bool IsPowerOfTwo(std::size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

std::size_t RoundUpToPowerOfTwo(std::size_t value)
{
    if (value <= 1)
    {
        return 1;
    }

    --value;
    for (std::size_t shift = 1; shift < sizeof(std::size_t) * 8; shift <<= 1)
    {
        value |= value >> shift;
    }
    return value + 1;
}
} // namespace

void ApplyRootDirectory(AppConfig& config, const std::filesystem::path& rootDir)
{
    config.rootDir = rootDir;
    config.inputDir = config.rootDir / "input";
    config.outputDir = config.rootDir / "output";
}

void NormalizeAtlasDimensions(AppConfig& config)
{
    config.atlasWidth = config.requestedAtlasWidth;
    config.atlasHeight = config.requestedAtlasHeight;

    if (!IsPowerOfTwo(config.atlasWidth))
    {
        config.atlasWidth = RoundUpToPowerOfTwo(config.atlasWidth);
    }

    if (!IsPowerOfTwo(config.atlasHeight))
    {
        config.atlasHeight = RoundUpToPowerOfTwo(config.atlasHeight);
    }

    if (config.atlasWidth == 0)
    {
        config.atlasWidth = 1;
    }

    if (config.atlasHeight == 0)
    {
        config.atlasHeight = 1;
    }
}

bool ParseCommandLine(int argc, char* argv[], AppConfig& config, std::string* errorMessage)
{
    config = AppConfig{};

    bool rootSet = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i] != nullptr ? argv[i] : "";

        if (argument == "--help")
        {
            config.showHelp = true;
            continue;
        }

        if (argument == "-q" || argument == "--quiet")
        {
            config.verbose = false;
            continue;
        }

        if (argument == "-w" || argument == "--width" || argument == "--atlas-width")
        {
            if (i + 1 >= argc)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Missing value after width option.";
                }
                return false;
            }

            std::size_t parsed = 0;
            if (!TryParseSizeT(argv[++i], parsed))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Invalid value for width option.";
                }
                return false;
            }

            config.requestedAtlasWidth = parsed;
            continue;
        }

        if (argument == "-h" || argument == "--height" || argument == "--atlas-height")
        {
            if (i + 1 >= argc)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Missing value after height option.";
                }
                return false;
            }

            std::size_t parsed = 0;
            if (!TryParseSizeT(argv[++i], parsed))
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Invalid value for height option.";
                }
                return false;
            }

            config.requestedAtlasHeight = parsed;
            continue;
        }

        if (argument == "--root")
        {
            if (i + 1 >= argc)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "Missing value after --root.";
                }
                return false;
            }

            ApplyRootDirectory(config, argv[++i]);
            rootSet = true;
            continue;
        }

        if (!argument.empty() && argument.front() == '-')
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = "Unknown option: " + argument;
            }
            return false;
        }

        if (!rootSet)
        {
            ApplyRootDirectory(config, argument);
            rootSet = true;
            continue;
        }

        if (errorMessage != nullptr)
        {
            *errorMessage = "Too many positional arguments.";
        }
        return false;
    }

    NormalizeAtlasDimensions(config);
    return true;
}

void PrintUsage(std::ostream& os, const std::filesystem::path& executableName)
{
    os << "Usage: " << executableName.string() << " [options] [root-dir]\n"
       << "Options:\n"
       << "  --root <dir>             Root directory that contains input/ and output/\n"
       << "  -w, --width <n>          Atlas width in pixels (default: 1024)\n"
       << "  -h, --height <n>         Atlas height in pixels (default: 1024)\n"
       << "  -q, --quiet              Reduce console output\n"
       << "  --help                   Show this help message\n";
}
} // namespace polyx::core
