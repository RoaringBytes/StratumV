// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstdarg>

namespace sv {

// ── Log severity levels ────────────────────────────────────────────
enum class LogSeverity : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };

inline const char* severityToString(LogSeverity s) {
    switch (s) {
        case LogSeverity::Debug: return "debug";
        case LogSeverity::Info:  return "info";
        case LogSeverity::Warn:  return "warn";
        case LogSeverity::Error: return "error";
    }
    return "unknown";
}

inline LogSeverity severityFromString(const char* s) {
    if (!s || !s[0]) return LogSeverity::Debug;
    if (s[0] == 'd' || s[0] == 'D') return LogSeverity::Debug;
    if (s[0] == 'i' || s[0] == 'I') return LogSeverity::Info;
    if (s[0] == 'w' || s[0] == 'W') return LogSeverity::Warn;
    if (s[0] == 'e' || s[0] == 'E') return LogSeverity::Error;
    return LogSeverity::Debug;
}

// ── Single log entry ───────────────────────────────────────────────
struct LogEntry {
    uint64_t    id;          // monotonically increasing
    LogSeverity severity;
    std::string tag;         // e.g. "Engine", "Render", "DLL", "Config"
    std::string message;
    int64_t     timestampMs; // ms since EngineLog construction
};

// ── Ring-buffer log sink ───────────────────────────────────────────
// Thread-safe singleton.  All engine subsystems and DLL plugins write
// here via the SV_LOG_* macros.  DevServer reads via getEntries().
class EngineLog {
public:
    static EngineLog& get();

    // Primary logging interface (printf-style)
    void log(LogSeverity sev, const char* tag, const char* fmt, ...);
    void logv(LogSeverity sev, const char* tag, const char* fmt, va_list args);

    // Retrieve entries from ring buffer.
    // sinceId=0 returns the most recent `limit` entries.
    // sinceId>0 returns entries with id > sinceId (up to `limit`).
    std::vector<LogEntry> getEntries(uint64_t sinceId = 0,
                                     LogSeverity minSev = LogSeverity::Debug,
                                     const char* tagFilter = nullptr,
                                     size_t limit = 100) const;

    // Ring buffer capacity
    void   setCapacity(size_t cap);
    size_t capacity() const { return m_capacity; }

    // Latest ID (for incremental polling)
    uint64_t latestId() const;

private:
    EngineLog();

    static constexpr size_t kDefaultCapacity = 1024;

    mutable std::mutex     m_mutex;
    std::vector<LogEntry>  m_buffer;
    size_t                 m_head     = 0;   // next write position
    size_t                 m_count    = 0;   // entries currently stored
    size_t                 m_capacity = kDefaultCapacity;
    uint64_t               m_nextId   = 1;
    int64_t                m_startTime;      // steady_clock epoch (ms)
};

// ── Convenience macros ─────────────────────────────────────────────
#define SV_LOG_DEBUG(tag, ...) ::sv::EngineLog::get().log(::sv::LogSeverity::Debug, tag, __VA_ARGS__)
#define SV_LOG_INFO(tag, ...)  ::sv::EngineLog::get().log(::sv::LogSeverity::Info,  tag, __VA_ARGS__)
#define SV_LOG_WARN(tag, ...)  ::sv::EngineLog::get().log(::sv::LogSeverity::Warn,  tag, __VA_ARGS__)
#define SV_LOG_ERROR(tag, ...) ::sv::EngineLog::get().log(::sv::LogSeverity::Error, tag, __VA_ARGS__)

} // namespace sv
