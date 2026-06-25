#pragma once

namespace polyx::core
{
// Block until the user presses a key. Platform-specific: Windows uses _getch,
// other platforms fall back to a buffered read. Isolated here so callers do not
// include conio.h / platform headers directly.
void WaitForAnyKey();
} // namespace polyx::core
