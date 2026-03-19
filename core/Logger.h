#pragma once

#include <cstddef>
#include <ostream>
#include <string_view>

namespace polyx::core
{
class Logger
{
public:
    explicit Logger(std::ostream& output);

    void Info(std::string_view message) const;
    void Warn(std::string_view message) const;
    void Error(std::string_view message) const;

    std::size_t WarningCount() const;
    std::size_t ErrorCount() const;

private:
    std::ostream* output_ = nullptr;
    mutable std::size_t warningCount_ = 0;
    mutable std::size_t errorCount_ = 0;
};
} // namespace polyx::core