#include "core/Logger.h"

namespace polyx::core
{
Logger::Logger(std::ostream& output)
    : output_(&output)
{
}

void Logger::Info(std::string_view message) const
{
    if (output_ != nullptr)
    {
        (*output_) << "[Info] " << message << '\n';
    }
}

void Logger::Warn(std::string_view message) const
{
    ++warningCount_;
    if (output_ != nullptr)
    {
        (*output_) << "[Warn] " << message << '\n';
    }
}

void Logger::Error(std::string_view message) const
{
    ++errorCount_;
    if (output_ != nullptr)
    {
        (*output_) << "[Error] " << message << '\n';
    }
}

std::size_t Logger::WarningCount() const
{
    return warningCount_;
}

std::size_t Logger::ErrorCount() const
{
    return errorCount_;
}
} // namespace polyx::core