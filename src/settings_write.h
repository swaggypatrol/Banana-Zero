#pragma once

// Writing dlssnr.ini back (M3): only the keys the menu owns, only the lines that carry them. Every other
// line, comments and unknown keys included, stays as it is. A key whose value is the default and has no line gets
// none; a key that has a line keeps it, updated; a model parameter set back to "the model's default" loses its line,
// since absence is what that means. StatsLog, DumpEvery and anything else the menu does not show are never touched.

#include <cstddef>

struct Settings;

// The file's text with `settings` merged in. `text` may be nullptr (no file yet: a short header is written first).
// The line ending follows the file (CRLF for a new one). Returns a new[] buffer of *length bytes, or nullptr when
// out of memory.
char* SettingsMergeIni(const char* text, const Settings& settings, size_t* length);

// Reads the file at `path`, merges, writes it back in one go (a temporary file beside it, then a replace, so a crash
// mid-write leaves the old file whole). False with the Win32 error in *error when it could not.
bool SettingsWriteIni(const wchar_t* path, const Settings& settings, unsigned long* error);

// "Intensity (model default) -> 0.8, WhiteEV 0 -> 1" for the log: every key the menu owns whose value differs
// between the two, in the file's spelling. Returns the length written; 0 when nothing differs.
size_t SettingsDiff(const Settings& before, const Settings& after, char* out, size_t size);
