#pragma once

// Set by CMake; enabled for standalone/manual builds unless explicitly disabled.
#ifndef MENUHTTP_WITH_LOGGER
#define MENUHTTP_WITH_LOGGER 1
#endif

#if MENUHTTP_WITH_LOGGER

#include <cstdint>
#include <string>

// Appends to <GarrysMod>/menuhttp.log after DllMain returns.
void MenuHTTP_Log(const char* format, ...);

// Writes every byte of data, without redaction or truncation. CR, LF, NUL and
// other control characters are escaped for an unambiguous one-line record.
void MenuHTTP_LogData(std::uint64_t id, const char* label, const std::string& data);

#else

// Discard calls at preprocessing time, including evaluation of their arguments.
// No log.cpp, file I/O, OutputDebugString or logging symbols are linked.
#define MenuHTTP_Log(...) ((void)0)
#define MenuHTTP_LogData(...) ((void)0)

#endif
