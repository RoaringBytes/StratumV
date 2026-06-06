// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
#include "EngineLog.h"

#include <chrono>
#include <cstdio>
#include <algorithm>

namespace sv {

EngineLog& EngineLog::get()
{
    static EngineLog instance;
    return instance;
}

EngineLog::EngineLog()
{
    m_buffer.resize(m_capacity);
    auto now = std::chrono::steady_clock::now();
    m_startTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

void EngineLog::log(LogSeverity sev, const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logv(sev, tag, fmt, args);
    va_end(args);
}

void EngineLog::logv(LogSeverity sev, const char* tag, const char* fmt, va_list args)
{
    // Format message
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);

    // Compute timestamp
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Also print to stdout/stderr for immediate visibility
    const char* sevStr = severityToString(sev);
    if (sev >= LogSeverity::Error)
        fprintf(stderr, "[%s][%s] %s\n", tag ? tag : "?", sevStr, buf);
    else
        printf("[%s][%s] %s\n", tag ? tag : "?", sevStr, buf);

    // Write to ring buffer
    std::lock_guard<std::mutex> lock(m_mutex);
    LogEntry& entry = m_buffer[m_head];
    entry.id          = m_nextId++;
    entry.severity    = sev;
    entry.tag         = tag ? tag : "";
    entry.message     = buf;
    entry.timestampMs = nowMs - m_startTime;

    m_head = (m_head + 1) % m_capacity;
    if (m_count < m_capacity) m_count++;
}

std::vector<LogEntry> EngineLog::getEntries(uint64_t sinceId,
                                            LogSeverity minSev,
                                            const char* tagFilter,
                                            size_t limit) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<LogEntry> result;
    result.reserve(std::min(limit, m_count));

    // Iterate ring buffer from oldest to newest
    size_t start = (m_count < m_capacity) ? 0 : m_head;
    for (size_t i = 0; i < m_count && result.size() < limit; i++) {
        size_t idx = (start + i) % m_capacity;
        const LogEntry& e = m_buffer[idx];

        if (e.id <= sinceId) continue;
        if (e.severity < minSev) continue;
        if (tagFilter && tagFilter[0] && e.tag != tagFilter) continue;

        result.push_back(e);
    }

    return result;
}

void EngineLog::setCapacity(size_t cap)
{
    if (cap == 0) cap = kDefaultCapacity;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Collect current entries (oldest first)
    std::vector<LogEntry> old;
    old.reserve(m_count);
    size_t start = (m_count < m_capacity) ? 0 : m_head;
    for (size_t i = 0; i < m_count; i++) {
        size_t idx = (start + i) % m_capacity;
        old.push_back(m_buffer[idx]);
    }

    // Resize and re-fill
    m_capacity = cap;
    m_buffer.resize(cap);

    // Keep the most recent entries that fit
    size_t keep = std::min(old.size(), cap);
    size_t skip = old.size() - keep;
    for (size_t i = 0; i < keep; i++)
        m_buffer[i] = old[skip + i];

    m_head  = keep % cap;
    m_count = keep;
}

uint64_t EngineLog::latestId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nextId - 1;
}

} // namespace sv
