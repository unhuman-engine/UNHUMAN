#pragma once
#include "uhepch.h"
#include "spdlog/sinks/base_sink.h"
#include <mutex>

namespace UHE {

	struct ImGuiLogEntry   
	{
		std::string Message;
		spdlog::level::level_enum Level;
	};

	template<typename Mutex>
	class ImGuiLogSink   
		: public spdlog::sinks::base_sink<Mutex>
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
		void sink_it_(const spdlog::details::log_msg& msg) override {
			spdlog::memory_buf_t formatted;
			this->formatter_->format(msg, formatted);

			ImGuiLogEntry entry;
			entry.Message = fmt::to_string(formatted);
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
}
