#include "core/Console.h"

#if defined(_WIN32)
#include <conio.h>
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
} // namespace polyx::core
