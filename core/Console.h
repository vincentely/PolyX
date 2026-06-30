#pragma once

namespace polyx::core
{
// Block until the user presses a key. Platform-specific: Windows uses _getch,
// other platforms fall back to a buffered read. Isolated here so callers do not
// include conio.h / platform headers directly.
void WaitForAnyKey();

// Switch the attached console to UTF-8 (output + input) so non-ASCII file paths
// (e.g. CJK directories) render correctly. No-op on non-Windows; safe to call
// even when no console is attached.
void EnableUtf8Console();
} // namespace polyx::core
