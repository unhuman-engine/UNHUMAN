#include <uhepch.h>
#include "Log.h"

#include "UHE/AssestsManager/VfsSystem.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <chrono>
#include <exception>
#include <filesystem>

namespace UHE {
	std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
	std::shared_ptr <spdlog::logger> Log::s_ClientLogger;
	
	std::shared_ptr<ImGuiLogSink<std::mutex>> Log::s_ImGuiSink;
	std::string Log::s_LogFilePath;

	void Log::Init()
	{
		spdlog::set_pattern("%^[%T] %n: %v%$");

		auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		s_ImGuiSink = std::make_shared<ImGuiLogSink<std::mutex>>();

		std::vector<spdlog::sink_ptr> sinks{ consoleSink, s_ImGuiSink };

		// Persist every message to <project>/logs/console.log. The file is
		// truncated on launch so each editor session gets a clean log, while
		// startup failures below still surface on stdout.
		try
		{
			std::filesystem::path root = FileSystem::Get().GetRootPath();
			if (root.empty())
				root = std::filesystem::current_path();

			std::filesystem::path logDir = root / "logs";
			std::filesystem::create_directories(logDir);

			s_LogFilePath = (logDir / "console.log").string();

			auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(s_LogFilePath, true);
			fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %n: %v");
			sinks.push_back(fileSink);
		}
		catch (const std::exception& e)
		{
			s_LogFilePath.clear();
			std::cerr << "[UHE::Log] WARNING: could not create console.log: " << e.what() << std::endl;
		}

		s_CoreLogger = std::make_shared<spdlog::logger>("UHE", sinks.begin(), sinks.end());
		s_CoreLogger->set_level(spdlog::level::trace);
		s_CoreLogger->flush_on(spdlog::level::err);

		s_ClientLogger = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());
		s_ClientLogger->set_level(spdlog::level::trace);
		s_ClientLogger->flush_on(spdlog::level::err);

		// Register with the registry so the periodic flusher below reaches them
		// and console.log keeps up with the app without paying per-message I/O.
		spdlog::register_logger(s_CoreLogger);
		spdlog::register_logger(s_ClientLogger);
		spdlog::flush_every(std::chrono::seconds(1));

		if (!s_LogFilePath.empty())
			s_CoreLogger->info("Console log file: {}", s_LogFilePath);
	}
	
}
