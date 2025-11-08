#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace DebugLog {
    inline std::string GetLogPath() {
        char path[MAX_PATH] = {0};
#ifdef _WIN32
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYDOCUMENTS, NULL, 0, path))) {
            std::string base = path;
            const std::string pNoSpace = base + "\\My Games\\Fallout4VR\\F4SE\\Plugins\\F4SEVR_DLSS.log";
            const std::string pWithSpace = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\F4SEVR_DLSS.log";
            if (GetFileAttributesA(pNoSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pNoSpace;
            if (GetFileAttributesA(pWithSpace.c_str()) != INVALID_FILE_ATTRIBUTES) return pWithSpace;
            // Prefer existing folder; else default to no-space
            const std::string dNo = base + "\\My Games\\Fallout4VR\\F4SE\\Plugins\\";
            const std::string dWs = base + "\\My Games\\Fallout 4 VR\\F4SE\\Plugins\\";
            if (GetFileAttributesA(dNo.c_str()) != INVALID_FILE_ATTRIBUTES) return pNoSpace;
            if (GetFileAttributesA(dWs.c_str()) != INVALID_FILE_ATTRIBUTES) return pWithSpace;
            return pNoSpace;
        }
#endif
        return std::string("F4SEVR_DLSS.log");
    }

    inline void Write(const char* level, const char* fmt, va_list args) {
        char buffer[2048];
        if (!fmt) {
            return;
        }
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        std::string line;
        if (level && *level) {
            line = "[DLSS][";
            line += level;
            line += "] ";
        }
        line += buffer;
        line += "\n";

#ifdef _WIN32
        OutputDebugStringA(line.c_str());
#endif
        std::string logPath = GetLogPath();
#ifdef _WIN32
        // Ensure directory exists
        std::string dir = logPath;
        size_t pos = dir.find_last_of("/\\");
        if (pos != std::string::npos) {
            dir = dir.substr(0, pos);
            SHCreateDirectoryExA(NULL, dir.c_str(), NULL);
        }
#endif
        std::FILE* file = std::fopen(logPath.c_str(), "a");
        if (file) {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fclose(file);
        }
    }

    inline void Message(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        Write("INFO", fmt, args);
        va_end(args);
    }

    inline void Error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        Write("ERROR", fmt, args);
        va_end(args);
    }
}

#define _MESSAGE(...) ::DebugLog::Message(__VA_ARGS__)
#define _ERROR(...)   ::DebugLog::Error(__VA_ARGS__)

