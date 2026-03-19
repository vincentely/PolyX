#include <conio.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "app/BatchProcessor.h"
#include "core/Config.h"
#include "core/Logger.h"

namespace
{
bool ShouldPause()
{
    const char* value = std::getenv("POLYX_NO_PAUSE");
    if (value == nullptr)
    {
        return true;
    }

    std::string flag = value;
    for (char& ch : flag)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    return !(flag == "1" || flag == "true" || flag == "yes" || flag == "on");
}

void PauseForKeyPress()
{
    if (!ShouldPause())
    {
        return;
    }

    std::cout << "\nPress any key to exit...";
    std::cout.flush();
    (void)_getch();
    std::cout << '\n';
}
} // namespace

int main(int argc, char* argv[])
{
    polyx::core::AppConfig config;
    std::string errorMessage;
    const std::filesystem::path executableName = argc > 0 ? argv[0] : "PolyX";

    if (!polyx::core::ParseCommandLine(argc, argv, config, &errorMessage))
    {
        std::cerr << errorMessage << '\n';
        polyx::core::PrintUsage(std::cerr, executableName);
        PauseForKeyPress();
        return 1;
    }

    if (config.showHelp)
    {
        polyx::core::PrintUsage(std::cout, executableName);
        return 0;
    }

    if (argc == 1)
    {
        std::cout << "No command line arguments provided.\n";
        std::cout << "Enter root directory path (relative or absolute): ";

        std::string rootPath;
        if (!std::getline(std::cin, rootPath) || rootPath.empty())
        {
            std::cerr << "Root directory path is required.\n";
            PauseForKeyPress();
            return 1;
        }

        polyx::core::ApplyRootDirectory(config, rootPath);
    }

    polyx::core::NormalizeAtlasDimensions(config);

    polyx::core::Logger logger(std::cout);
    logger.Info("Atlas target size: requested " + std::to_string(config.requestedAtlasWidth) + "x" + std::to_string(config.requestedAtlasHeight) +
                ", normalized " + std::to_string(config.atlasWidth) + "x" + std::to_string(config.atlasHeight));

    polyx::app::BatchProcessor processor(config, logger);
    const bool ok = processor.Run();

    PauseForKeyPress();
    return ok ? 0 : 1;
}
