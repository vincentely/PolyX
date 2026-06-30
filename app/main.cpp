#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "app/BatchProcessor.h"
#include "app/Manifest.h"
#include "core/Config.h"
#include "core/Console.h"
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
    polyx::core::WaitForAnyKey();
    std::cout << '\n';
}

std::filesystem::path ExeDir(const std::filesystem::path& arg0)
{
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(arg0, ec);
    if (ec || abs.empty())
    {
        return std::filesystem::current_path();
    }
    return abs.parent_path();
}
} // namespace

int main(int argc, char* argv[])
{
    polyx::core::EnableUtf8Console();

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

    if (!config.manifestPath.empty())
    {
        polyx::core::Logger logger(std::cout);

        polyx::manifest::Request request;
        std::string manifestError;
        if (!polyx::manifest::ReadRequest(config.manifestPath, request, &manifestError))
        {
            logger.Error("Failed to read manifest: " + manifestError);
            PauseForKeyPress();
            return 1;
        }

        std::error_code ec;
        const std::filesystem::path jsonDir = std::filesystem::absolute(config.manifestPath, ec).parent_path();
        std::string folderName = jsonDir.filename().u8string();
        if (folderName.empty())
        {
            folderName = "manifest";
        }
        const std::filesystem::path outputDir = ExeDir(executableName) / std::filesystem::u8path("output_" + folderName);

        logger.Info("Manifest: " + config.manifestPath.string() + " (" + std::to_string(request.items.size()) + " items)");
        logger.Info("Output dir: " + outputDir.string());

        polyx::app::BatchProcessor processor(config, logger);
        polyx::manifest::Result result;
        const bool ok = processor.RunManifest(request, jsonDir, outputDir, result);

        const std::filesystem::path resultPath = outputDir / "result.json";
        std::string resultError;
        if (!polyx::manifest::WriteResult(resultPath, result, &resultError))
        {
            logger.Error("Failed to write result: " + resultError);
        }
        else
        {
            logger.Info("Result written: " + resultPath.string());
        }

        std::size_t okCount = 0;
        std::size_t warnCount = 0;
        std::size_t errorCount = 0;
        for (const polyx::manifest::ResultItem& item : result.items)
        {
            if (item.status == "ok") ++okCount;
            else if (item.status == "warn") ++warnCount;
            else ++errorCount;
        }
        logger.Info("Done: atlas " + std::to_string(result.atlasWidth) + "x" + std::to_string(result.atlasHeight) +
                    "  meshes ok=" + std::to_string(okCount) + " warn=" + std::to_string(warnCount) +
                    " error=" + std::to_string(errorCount));

        PauseForKeyPress();
        return ok ? 0 : 1;
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
    if (config.autoAtlasSize)
    {
        logger.Info("Atlas target size: auto smallest power-of-two square");
    }
    else
    {
        logger.Info("Atlas target size: requested " + std::to_string(config.requestedAtlasWidth) + "x" + std::to_string(config.requestedAtlasHeight) +
                    ", normalized " + std::to_string(config.atlasWidth) + "x" + std::to_string(config.atlasHeight));
    }

    polyx::app::BatchProcessor processor(config, logger);
    const bool ok = processor.Run();

    PauseForKeyPress();
    return ok ? 0 : 1;
}
