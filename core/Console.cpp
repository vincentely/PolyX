#include "core/Console.h"

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <cstdio>
#endif

namespace polyx::core
{
void WaitForAnyKey()
{
#if defined(_WIN32)
    (void)_getch();
#else
    (void)std::getchar();
#endif
}

void EnableUtf8Console()
{
#if defined(_WIN32)
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
#endif
}
} // namespace polyx::core
