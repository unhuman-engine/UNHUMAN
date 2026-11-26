#pragma once
#include "uhepch.h"
#include <mutex>
#include "spdlog/details/os.h"
#include "spdlog/sinks/base_sink.h"

namespace UHE
{

/// @brief One logged message, kept both whole and in parts.
///
/// The console shows the parts in columns (time, source, text) instead of
/// one long formatted line; Message stays the full line for copying.
struct ImGuiLogEntry
{
    std::string Message; ///< the formatted line, as the terminal shows it
    std::string Time;    ///< local wall time, HH:MM:SS
    std::string Logger;  ///< "UHE" (engine) or "APP" (client)
    std::string Text;    ///< the message alone
    spdlog::level::level_enum Level = spdlog::level::info;
};

template <typename Mutex> class ImGuiLogSink : public spdlog::sinks::base_sink<Mutex>
{
public:
    // Returns a copy of the buffered log entries. The lock keeps this safe
    // against log calls coming from worker threads.
    std::vector<ImGuiLogEntry> GetSnapshot()
    {
        std::lock_guard<Mutex> lock(this->mutex_);
        return m_Logs;
    }

    size_t GetCount()
    {
        std::lock_guard<Mutex> lock(this->mutex_);
        return m_Logs.size();
    }

    void Clear()
    {
        std::lock_guard<Mutex> lock(this->mutex_);
        m_Logs.clear();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);

        ImGuiLogEntry entry;
        entry.Message = fmt::to_string(formatted);
        entry.Text.assign(msg.payload.data(), msg.payload.size());
        entry.Logger.assign(msg.logger_name.data(), msg.logger_name.size());
        const std::tm local = spdlog::details::os::localtime(std::chrono::system_clock::to_time_t(msg.time));
        char time[16];
        std::snprintf(time, sizeof(time), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
        entry.Time = time;
        entry.Level = msg.level;

        m_Logs.push_back(entry);

        // Trim in a batch so steady-state logging doesn't pay an O(n)
        // erase on every single message.
        if (m_Logs.size() > kMaxLogs + kTrimBatch)
            m_Logs.erase(m_Logs.begin(), m_Logs.begin() + (m_Logs.size() - kMaxLogs));
    }

    void flush_() override {}

private:
    static constexpr size_t kMaxLogs = 2000;
    static constexpr size_t kTrimBatch = 256;

    std::vector<ImGuiLogEntry> m_Logs;
};
} // namespace UHE
