#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <queue>
#include <thread>
#include <print>
#include <unordered_set>

#if S2_PLATFORM_WINDOWS
#include <windows.h>
#include <dbghelp.h>
#undef FormatMessage
#else
#include <dlfcn.h>
#include <cstdlib>
#endif

#include <client/crash_report_database.h>
#include <client/crashpad_client.h>
#include <client/settings.h>

#include <dynlibutils/module.hpp>
#include <dynlibutils/virtual.hpp>
#include <dynlibutils/vthook.hpp>
#include <CLI/CLI.hpp>
#include <glaze/glaze.hpp>
#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

#include <plugify/assembly.hpp>
#include <plugify/extension.hpp>
#include <plugify/logger.hpp>
#include <plugify/manager.hpp>
#include <plugify/plugify.hpp>

#include <plg/debugging.hpp>
#include <plg/enum.hpp>
#include <plg/format.hpp>

#include <eiface.h>
#include <convar.h>
#include <igamesystem.h>
#include <tier0/logging.h>
#include <appframework/iappsystem.h>

using namespace plugify;
using namespace crashpad;
namespace fs = std::filesystem;

// Source2 color definitions
namespace S2Colors {
	static const Color RESET = Color(255, 255, 255, 255);
	static const Color WHITE = Color(255, 255, 255, 255);
	static const Color RED = Color(255, 0, 0, 255);
	static const Color GREEN = Color(0, 255, 0, 255);
	static const Color YELLOW = Color(255, 255, 0, 255);
	static const Color BLUE = Color(0, 127, 255, 255);
	static const Color MAGENTA = Color(255, 0, 255, 255);
	static const Color ORANGE = Color(255, 127, 0, 255);
	static const Color CYAN = Color(0, 255, 255, 255);
	static const Color GRAY = Color(127, 127, 127, 255);
	static const Color BLACK = Color(0, 0, 0, 255);
}

namespace {
	// 0  0x00  NUL  (Null)                    ❌ NEVER USE - String terminator
	// 1  0x01  SOH  (Start of Heading)        ✅ Safe to use
	// 2  0x02  STX  (Start of Text)           ✅ Safe to use
	// 3  0x03  ETX  (End of Text)             ✅ Safe to use
	// 4  0x04  EOT  (End of Transmission)     ✅ Safe to use
	// 5  0x05  ENQ  (Enquiry)                 ✅ Safe to use
	// 6  0x06  ACK  (Acknowledge)             ✅ Safe to use
	// 7  0x07  BEL  (Bell)                    ⚠️  AVOID - Makes beep sound
	// 8  0x08  BS   (Backspace)               ⚠️  AVOID - Terminal control
	// 9  0x09  HT   (Horizontal Tab)          ❌ AVOID - Common in text (\t)
	// 10 0x0A  LF   (Line Feed)               ❌ AVOID - Newline on Unix (\n)
	// 11 0x0B  VT   (Vertical Tab)            ⚠️  Risky - Sometimes used
	// 12 0x0C  FF   (Form Feed)               ⚠️  Risky - Page break
	// 13 0x0D  CR   (Carriage Return)         ❌ AVOID - Newline on Windows (\r)
	// 14 0x0E  SO   (Shift Out)               ✅ Safe to use
	// 15 0x0F  SI   (Shift In)                ✅ Safe to use
	// 16 0x10  DLE  (Data Link Escape)        ✅ Safe to use
	// 17 0x11  DC1  (Device Control 1/XON)    ⚠️  Risky - Flow control
	// 18 0x12  DC2  (Device Control 2)        ✅ Safe to use
	// 19 0x13  DC3  (Device Control 3/XOFF)   ⚠️  Risky - Flow control
	// 20 0x14  DC4  (Device Control 4)        ✅ Safe to use
	// 21 0x15  NAK  (Negative Acknowledge)    ✅ Safe to use
	// 22 0x16  SYN  (Synchronous Idle)        ✅ Safe to use
	// 23 0x17  ETB  (End of Trans. Block)     ✅ Safe to use
	// 24 0x18  CAN  (Cancel)                  ✅ Safe to use
	// 25 0x19  EM   (End of Medium)           ✅ Safe to use
	// 26 0x1A  SUB  (Substitute)              ⚠️  AVOID - EOF on Windows (Ctrl+Z)
	// 27 0x1B  ESC  (Escape)                  ❌ AVOID - ANSI escape sequences
	// 28 0x1C  FS   (File Separator)          ✅ Safe to use
	// 29 0x1D  GS   (Group Separator)         ✅ Safe to use
	// 30 0x1E  RS   (Record Separator)        ✅ Safe to use
	// 31 0x1F  US   (Unit Separator)          ✅ Safe to use
}

// ANSI Color codes
struct Colors {
	static constexpr char RESET = '\x01';    // SOH (Start of Heading)
	static constexpr char WHITE = '\x01';    // SOH (Start of Heading)
	static constexpr char RED = '\x02';      // STX (Start of Text)
	static constexpr char GREEN = '\x03';    // ETX (End of Text)
	static constexpr char YELLOW = '\x04';   // EOT (End of Transmission)
	static constexpr char BLUE = '\x05';     // ENQ (Enquiry)
	static constexpr char MAGENTA = '\x06';  // ACK (Acknowledge)
	static constexpr char ORANGE = '\x07';   // BEL (Bell)
	static constexpr char CYAN = '\x08';     // BS  (Backspace
	static constexpr char GRAY = '\x0B';     // VT  (Vertical Tab)
	static constexpr char BLACK = '\x0C';    // FF  (Form Feed)
};

class AnsiColorParser {
public:
	struct TextSegment {
		std::string_view text;
		Color color;
	};

	// Use bit flags to quickly check if a byte is a color code
	static constexpr bool isColorCode[256] = {
		false,  // 0  0x00 (unused)
		true,   // 1  0x01 RESET
		true,   // 2  0x02
		true,   // 3  0x03
		true,   // 4  0x04
		true,   // 5  0x05
		true,   // 6  0x06
		true,   // 7  0x07
		true,   // 8  0x08
		false,  // 9  0x09 (skip - TAB)
		false,  // 10 0x0A (skip - LF)
		true,   // 11 0x0B
		true,   // 12 0x0C
		false,  // 13 0x0D (skip - CR)
		// ... rest are false
	};

	inline static Color colorMap[256] = {
		S2Colors::WHITE,    // 0  0x00 (unused)
		S2Colors::WHITE,    // 1  0x01 RESET
		S2Colors::RED,      // 2  0x02
		S2Colors::GREEN,    // 3  0x03
		S2Colors::YELLOW,   // 4  0x04
		S2Colors::BLUE,     // 5  0x05
		S2Colors::MAGENTA,  // 6  0x06
		S2Colors::ORANGE,   // 7  0x07
		S2Colors::CYAN,     // 8  0x08
		S2Colors::WHITE,    // 9  0x09 (skip - TAB)
		S2Colors::WHITE,    // 10 0x0A (skip - LF)
		S2Colors::GRAY,     // 11 0x0B
		S2Colors::BLACK,    // 12 0x0C
		S2Colors::WHITE,    // 13 0x0D (skip - CR)
		// ... etc
	};

	static std::vector<TextSegment> Tokenize(std::string_view str) {
		std::vector<TextSegment> segments;

		Color current_color = S2Colors::WHITE;
		size_t text_start = 0;

		for (size_t i = 0; i < str.length(); ++i) {
			auto byte = static_cast<unsigned char>(str[i]);
			if (byte < 32 && isColorCode[byte]) {
				const_cast<char&>(str[i]) = '\0';  // Null-terminate at color

				// Found color code
				if (i > text_start) {
					segments.emplace_back(str.substr(text_start, i - text_start), current_color);
				}

				current_color = colorMap[byte];
				text_start = i + 1;
			}
		}

		// Add remaining text
		if (text_start < str.length()) {
			segments.emplace_back(str.substr(text_start), current_color);
		}

		return segments;
	}

	// Helper to strip color codes for display/logging
	static std::string StripColors(std::string_view input) {
		std::string result;
		result.reserve(input.length());

		for (char c : input) {
			auto byte = static_cast<unsigned char>(c);
			if (byte >= 32 || !isColorCode[byte]) {
				result += c;
			}
		}

		return result;
	}
};

class ConsoleLoggger final : public ILogger {
public:
	explicit ConsoleLoggger(
	    const char* name,
	    int flags = 0,
	    LoggingVerbosity_t verbosity = LV_DEFAULT,
	    Color color = UNSPECIFIED_LOGGING_COLOR
	) {
		m_channelID = LoggingSystem_RegisterLoggingChannel(name, nullptr, flags, verbosity, color);
	}

	~ConsoleLoggger() override = default;

	void Log(std::string_view message, Color color, bool newLine) const {
		assert((*message.end()) == 0);
		assert(message.size() < 2048);
		std::scoped_lock<std::mutex> lock(m_mutex);
		LoggingSystem_Log(m_channelID, LS_MESSAGE, color, message.data());
		if (newLine && message.back() != '\n') {
			LoggingSystem_Log(m_channelID, LS_MESSAGE, color, "\n");
		}
	}

	// ReSharper disable once CppPassValueParameterByConstReference
	void Log(std::string message, bool newLine) const {
		auto tokens = AnsiColorParser::Tokenize(message);

		std::scoped_lock<std::mutex> lock(m_mutex);
		for (const auto& [text, color] : tokens) {
			for (auto segments = Tokenize(text); const auto& segment : segments) {
				LoggingSystem_Log(m_channelID, LS_MESSAGE, color, segment.data());
			}
		}
		if (newLine && message.back() != '\n') {
			LoggingSystem_Log(m_channelID, LS_MESSAGE, "\n");
		}
	}

	void Log(std::string_view message, Severity severity, [[maybe_unused]] std::source_location loc) override {
		if (severity <= m_severity) {
			auto output = FormatMessage(message, severity, loc);

			std::scoped_lock<std::mutex> lock(m_mutex);
			for (auto segments = Tokenize(output); const auto& segment : segments) {
				switch (severity) {
					case Severity::Unknown:
						LoggingSystem_Log(m_channelID, LS_MESSAGE, S2Colors::WHITE, segment.data());
						break;
					case Severity::Fatal:
						LoggingSystem_Log(m_channelID, LS_ERROR, S2Colors::MAGENTA, segment.data());
						break;
					case Severity::Error:
						LoggingSystem_Log(m_channelID, LS_WARNING, S2Colors::RED, segment.data());
						break;
					case Severity::Warning:
						LoggingSystem_Log(m_channelID, LS_WARNING, S2Colors::ORANGE, segment.data());
						break;
					case Severity::Info:
						LoggingSystem_Log(m_channelID, LS_MESSAGE, S2Colors::YELLOW, segment.data());
						break;
					case Severity::Debug:
						LoggingSystem_Log(m_channelID, LS_MESSAGE, S2Colors::GREEN, segment.data());
						break;
					case Severity::Verbose:
						LoggingSystem_Log(m_channelID, LS_MESSAGE, S2Colors::WHITE, segment.data());
						break;
					default:
						break;
				}
			}
		}
	}

	void SetLogLevel(Severity minSeverity) override {
		m_severity = minSeverity;
	}

	void Flush() override {
	}

protected:
	static std::string
	FormatMessage(std::string_view message, Severity severity, const std::source_location& loc) {
		using namespace std::chrono;

		auto now = system_clock::now();

		// Split into seconds + milliseconds
		auto seconds = floor<std::chrono::seconds>(now);
		auto ms = duration_cast<milliseconds>(now - seconds);

		return std::format(
			"[{:%F %T}.{:03d}] [{}] [{}:{}] {}\n",
			seconds,  // %F = YYYY-MM-DD, %T = HH:MM:SS
			static_cast<int>(ms.count()),
			plg::enum_to_string(severity),
			loc.file_name(),
			loc.line(),
			message
		);
	}

	static std::vector<std::string_view> Tokenize(std::string_view str, size_t max_size = 2048) {
		std::vector<std::string_view> segments;

		for (size_t pos = 0; pos < str.size();) {
			size_t chunk_size = std::min(max_size, str.size() - pos);

			// Try to break at space if we're at max size and not at the end
			if (chunk_size == max_size && pos + chunk_size < str.size()) {
				// Look for space within the chunk
				auto nl_pos = str.rfind('\n', pos + chunk_size - 1);
				if (nl_pos != std::string_view::npos && nl_pos > pos) {
					const_cast<char&>(str[nl_pos]) = '\0';  // Null-terminate at space
					chunk_size = nl_pos - pos;
				}
			}

			segments.push_back(str.substr(pos, chunk_size));
			pos += chunk_size + (pos + chunk_size < str.size() && str[pos + chunk_size] == '\0');
		}

		return segments;
	}

private:
	mutable std::mutex m_mutex;
	std::atomic<Severity> m_severity{ Severity::Unknown };
	LoggingChannelID_t m_channelID;
};

class FileLoggingListener final : public ILoggingListener {
public:
	static Result<std::unique_ptr<FileLoggingListener>>
	Create(const fs::path& filename, bool async = true) {
		std::error_code ec;
		fs::create_directories(filename.parent_path(), ec);

		errno = 0;
		std::ofstream file(filename, std::ios::app);
		if (!file) {
			return MakeError(
			    "Failed to open log file: {} - {}",
			    plg::as_string(filename),
			    std::strerror(errno)
			);
		}

		return std::make_unique<FileLoggingListener>(std::move(file), async);
	}

	explicit FileLoggingListener(std::ofstream&& file, bool async = true)
	    : _async(async)
	    , _running(true)
	    , _file(std::move(file)) {
		if (_async) {
			_worker_thread = std::thread(&FileLoggingListener::ProcessQueue, this);
		}
	}

	~FileLoggingListener() {
		if (_async) {
			Stop();
		}
		_file.close();
	}

	void Log(const LoggingContext_t* pContext, const tchar* pMessage) override {
		if (pContext && (pContext->m_Flags & LCF_CONSOLE_ONLY) == 0) {
			std::string_view message = pMessage;
			if (message.ends_with('\n')) {
				message = message.substr(0, message.size() - 1);
			}
			if (message.empty()) {
				return;
			}

			auto now = std::chrono::system_clock::now();
			auto seconds = std::chrono::floor<std::chrono::seconds>(now);

			std::string formatted_message;
			try {
				std::chrono::zoned_time zt{ std::chrono::current_zone(), seconds };
				formatted_message = std::format("[{:%Y%m%d_%H%M%S}] {}", zt, message);
			} catch (const std::exception&) {
				// Fallback to UTC if local timezone fails
				// auto time_t = std::chrono::system_clock::to_time_t(seconds);
				formatted_message = std::format(
				    "[{:%Y%m%d_%H%M%S}] {}",
				    std::chrono::utc_clock::from_sys(seconds),
				    message
				);
			}

			if (_async) {
				{
					std::lock_guard lock(_queue_mutex);
					_message_queue.emplace(std::move(formatted_message));
				}
				_condition.notify_one();
			} else {
				Write(formatted_message);
			}
		}
	}

private:
	bool _async;
	std::atomic<bool> _running;
	std::mutex _queue_mutex;
	std::queue<std::string> _message_queue;
	std::condition_variable _condition;
	std::thread _worker_thread;
	std::mutex _file_mutex;
	std::ofstream _file;

	void Write(const std::string& message) {
		std::lock_guard lock(_file_mutex);
		std::println(_file, "{}", message);
		_file.flush();
	}

	void ProcessQueue() {
		while (_running) {
			std::unique_lock lock(_queue_mutex);
			_condition.wait(lock, [this] { return !_message_queue.empty() || !_running; });

			while (!_message_queue.empty()) {
				auto message = std::move(_message_queue.front());
				_message_queue.pop();
				lock.unlock();
				Write(message);
				lock.lock();
			}
		}
	}

	void Stop() {
		_running = false;
		_condition.notify_one();
		if (_worker_thread.joinable()) {
			_worker_thread.join();
		}
	}
};

enum class PlugifyState { Wait, Load, Unload, Reload };

std::shared_ptr<Plugify> s_plugify;
std::unique_ptr<CrashpadClient> s_crashpad;
std::shared_ptr<ConsoleLoggger> s_logger;
std::unique_ptr<FileLoggingListener> s_listener;
PlugifyState s_state;

namespace plg {
	/*PLUGIFY_FORCE_INLINE void print(const char* msg) {
		s_logger->Log(msg, S2Colors::WHITE, false);
	}

	PLUGIFY_FORCE_INLINE void print(std::string&& msg) {
		s_logger->Log(std::move(msg), false);
	}

	template<typename... Args>
	PLUGIFY_FORCE_INLINE void print(std::format_string<Args...> fmt, Args&&... args) {
		s_logger->Log(std::format(fmt, std::forward<Args>(args)...), false);
	}*/

	PLUGIFY_FORCE_INLINE void print(const char* msg) {
		s_logger->Log(msg, S2Colors::WHITE, true);
	}

	PLUGIFY_FORCE_INLINE void print(std::string&& msg) {
		s_logger->Log(std::move(msg), true);
	}

	template <typename... Args>
	PLUGIFY_FORCE_INLINE void print(std::format_string<Args...> fmt, Args&&... args) {
		s_logger->Log(std::format(fmt, std::forward<Args>(args)...), true);
	}
}

namespace {
	constexpr const char* SEPARATOR_LINE = "--------------------------------------------------------------------------------";
	constexpr const char* DOUBLE_LINE = "================================================================================";

	template <typename T>
	Result<T> ReadJson(const fs::path& path) {
		errno = 0;
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return MakeError(
				"Failed to read json file: {} - {}",
				plg::as_string(path),
				std::strerror(errno)
			);
		}
		auto text = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		auto json = glz::read_jsonc<T>(text);
		if (!json) {
			return MakeError(glz::format_error(json.error(), text));
		}

		return *json;
	}

	using ColorCode = char;

	// Helper to apply color
	std::string Colorize(std::string_view text, ColorCode color) {
		return std::format("{}{}{}", color, text, Colors::RESET);
	}

	// Helper to format duration
	std::string FormatDuration(std::chrono::microseconds duration) {
		using namespace std::chrono;
		auto ns = duration_cast<nanoseconds>(duration).count();
		if (ns < 1000) {
			return std::format("{}ns", ns);
		} else if (ns < 1000000) {
			return std::format("{:.2f}us", static_cast<double>(ns) / 1000.0);
		} else if (ns < 1000000000) {
			return std::format("{:.2f}ms", static_cast<double>(ns) / 1000000.0);
		} else {
			return std::format("{:.2f}s", static_cast<double>(ns) / 1000000000.0);
		}
	}

	// Get state info with color

	struct StateInfo {
		std::string_view symbol;
		ColorCode color;
	};

	struct Glyphs {
		std::string_view Ok;
		std::string_view Fail;
		std::string_view Warning;
		std::string_view Skipped;
		std::string_view Valid;
		std::string_view Resolving;
		std::string_view Arrow;
		std::string_view Number;
		std::string_view Unknown;
		std::string_view Missing;
		std::string_view Equal;
		std::string_view NotEqual;
		std::string_view Running;
	};

	// Unicode (original)
	inline constexpr Glyphs UnicodeGlyphs{
		"✓", "✗", "⚠", "○", "●", "⋯", "→", "#", "?", "ℹ", "=", "≠", "⚙",
	};

	// Plain ASCII fallback
	inline constexpr Glyphs AsciiGlyphs{
		"v", "x", "!", "o", "*", "...", "->", "#", "?", "i", "=", "!=", ">>",
	};

	// selection strategy examples:

	// 1) Compile-time: define USE_ASCII or USE_ASCII_ANSI in your build
#define USE_ASCII
#if defined(USE_ASCII)
	inline constexpr const Glyphs& Icons = AsciiGlyphs;
#else
	inline constexpr const Glyphs& Icons = UnicodeGlyphs;
#endif

	StateInfo GetStateInfo(ExtensionState state) {
		switch (state) {
			case ExtensionState::Parsed:
			case ExtensionState::Resolved:
			case ExtensionState::Started:
			case ExtensionState::Loaded:
			case ExtensionState::Exported:
				return { Icons.Ok, Colors::MAGENTA };
			case ExtensionState::Failed:
			case ExtensionState::Corrupted:
				return { Icons.Fail, Colors::RED };
			case ExtensionState::Unresolved:
				return { Icons.Warning, Colors::YELLOW };
			case ExtensionState::Disabled:
			case ExtensionState::Skipped:
				return { Icons.Skipped, Colors::GRAY };
			case ExtensionState::Loading:
			case ExtensionState::Starting:
			case ExtensionState::Parsing:
			case ExtensionState::Resolving:
			case ExtensionState::Exporting:
			case ExtensionState::Ending:
			case ExtensionState::Terminating:
				return { Icons.Resolving, Colors::CYAN };
			case ExtensionState::Running:
				return { Icons.Running, Colors::GREEN };
			default:
				return { Icons.Unknown, Colors::GRAY };
		}
	}

	// Helper to truncate string
	std::string Truncate(const std::string& str, size_t maxLen) {
		if (str.length() <= maxLen) {
			return str;
		}
		return str.substr(0, maxLen - 3) + "...";
	}

	// Format file size
	std::uintmax_t GetSizeRecursive(const fs::path& path) {
		namespace fs = fs;
		std::uintmax_t totalSize = 0;
		std::error_code ec;

		if (fs::is_regular_file(path, ec)) {
			auto sz = fs::file_size(path, ec);
			if (!ec) {
				totalSize += sz;
			}
		} else if (fs::is_directory(path)) {
			for (auto& entry : fs::recursive_directory_iterator(
			         path,
			         fs::directory_options::skip_permission_denied,
			         ec
			     )) {
				if (fs::is_regular_file(entry, ec)) {
					auto sz = fs::file_size(entry, ec);
					if (!ec) {
						totalSize += sz;
					}
				}
			}
		}
		return totalSize;
	}

	std::string FormatFileSize(const fs::path& path) {
		try {
			auto size = GetSizeRecursive(path);
			if (size < 1024) {
				return std::format("{} B", size);
			} else if (size < 1024 * 1024) {
				return std::format("{:.1f} KB", static_cast<double>(size) / 1024.0);
			} else if (size < 1024ull * 1024 * 1024) {
				return std::format("{:.1f} MB", static_cast<double>(size) / (1024.0 * 1024.0));
			} else {
				return std::format("{:.1f} GB", static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0));
			}
		} catch (...) {
			return "N/A";
		}
	}

	UniqueId FormatId(std::string_view str) {
		UniqueId::Value result;
		auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), result);
		if (ec != std::errc{}) {
			plg::print("{}: {}", Colorize("Error", Colors::RED), std::make_error_code(ec).message());
			return {};
		} else if (ptr != str.data() + str.size()) {
			plg::print(
			    "{}: Invalid argument: trailing characters after the valid part",
			    Colorize("Error", Colors::RED)
			);
			return {};
		}
		return UniqueId{ result };
	}

	fs::path FormatFileName(std::string_view type, std::string_view format) {
		using namespace std::chrono;
		auto now = system_clock::now();
		auto seconds = floor<std::chrono::seconds>(now);
		auto ms = duration_cast<milliseconds>(now - seconds);  // %F = YYYY-MM-DD, %T = HH:MM:SS
		std::string timestamp = std::format("{:%F-%T}", seconds, static_cast<int>(ms.count()));
		for (auto& c : timestamp) {
			if (c == ':') {
				c = '-';
			}
		}
		return std::format("{}-{}.{}", type, timestamp, format);
	}

	// Filter options
	struct FilterOptions {
		std::optional<std::vector<ExtensionState>> states;
		std::optional<std::vector<std::string>> languages;
		std::optional<std::string> searchQuery;
		bool showOnlyFailed = false;
		bool showOnlyWithErrors = false;
	};

	// Sort options
	enum class SortBy { Name, Version, State, Language, LoadTime };

	// Helper to check if extension matches filter
	bool MatchesFilter(const Extension* ext, const FilterOptions& filter) {
		if (filter.showOnlyFailed && ext->GetState() != ExtensionState::Failed) {
			return false;
		}

		if (filter.showOnlyWithErrors && !ext->HasErrors()) {
			return false;
		}

		if (filter.states.has_value()) {
			auto state = ext->GetState();
			if (std::find(filter.states->begin(), filter.states->end(), state)
			    == filter.states->end()) {
				return false;
			}
		}

		if (filter.languages.has_value()) {
			const auto& lang = ext->GetLanguage();
			if (std::find(filter.languages->begin(), filter.languages->end(), lang)
			    == filter.languages->end()) {
				return false;
			}
		}

		if (filter.searchQuery.has_value()) {
			std::string query = filter.searchQuery.value();
			std::transform(query.begin(), query.end(), query.begin(), ::tolower);

			std::string name = ext->GetName();
			std::transform(name.begin(), name.end(), name.begin(), ::tolower);

			std::string desc = ext->GetDescription();
			std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);

			if (name.find(query) == std::string::npos && desc.find(query) == std::string::npos) {
				return false;
			}
		}

		return true;
	}

	// Convert extension to JSON
	glz::json_t ExtensionToJson(const Extension* ext) {
		glz::json_t j;
		j["id"] = UniqueId::Value{ ext->GetId() };
		j["name"] = ext->GetName();
		j["version"] = ext->GetVersionString();
		j["type"] = ext->IsPlugin() ? "plugin" : "module";
		j["state"] = plg::enum_to_string(ext->GetState());
		j["language"] = ext->GetLanguage();
		j["location"] = plg::as_string(ext->GetLocation());

		if (!ext->GetDescription().empty()) {
			j["description"] = ext->GetDescription();
		}
		if (!ext->GetAuthor().empty()) {
			j["author"] = ext->GetAuthor();
		}
		if (!ext->GetWebsite().empty()) {
			j["website"] = ext->GetWebsite();
		}
		if (!ext->GetLicense().empty()) {
			j["license"] = ext->GetLicense();
		}

		// Dependencies
		if (!ext->GetDependencies().empty()) {
			glz::json_t::array_t dependencies;
			for (const auto& dep : ext->GetDependencies()) {
				glz::json_t::object_t depJson;
				depJson["name"] = dep.GetName();
				depJson["constraints"] = dep.GetConstraints().to_string();
				depJson["optional"] = dep.IsOptional();
				dependencies.emplace_back(std::move(depJson));
			}
			j["dependencies"] = glz::json_t::array_t{ std::move(dependencies) };
		}

		// Performance
		j["performance"]["total_time_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
		                                        ext->GetTotalTime()
		)
		                                        .count();

		// Errors and warnings
		if (ext->HasErrors()) {
			glz::json_t::array_t errors;
			errors.reserve(ext->GetErrors().size());
			for (const auto& error : ext->GetErrors()) {
				errors.emplace_back(error);
			}
			j["errors"] = glz::json_t::array_t{ std::move(errors) };
		}
		if (ext->HasWarnings()) {
			glz::json_t::array_t warnings;
			warnings.reserve(ext->GetWarnings().size());
			for (const auto& warning : ext->GetWarnings()) {
				warnings.emplace_back(warning);
			}
			j["warnings"] = glz::json_t::array_t{ std::move(warnings) };
		}

		return j;
	}

	// Filter extensions based on criteria
	std::vector<const Extension*>
	FilterExtensions(const std::vector<const Extension*>& extensions, const FilterOptions& filter) {
		std::vector<const Extension*> result;

		for (const auto& ext : extensions) {
			if (!MatchesFilter(ext, filter)) {
				continue;
			}
			result.push_back(ext);
		}

		return result;
	}

	// Sort extensions
	void SortExtensions(std::vector<const Extension*>& extensions, SortBy sortBy, bool reverse = false) {
		std::sort(
		    extensions.begin(),
		    extensions.end(),
		    [sortBy, reverse](const Extension* a, const Extension* b) {
			    bool result = false;
			    switch (sortBy) {
				    case SortBy::Name:
					    result = a->GetName() < b->GetName();
					    break;
				    case SortBy::Version:
					    result = a->GetVersion() < b->GetVersion();
					    break;
				    case SortBy::State:
					    result = a->GetState() < b->GetState();
					    break;
				    case SortBy::Language:
					    result = a->GetLanguage() < b->GetLanguage();
					    break;
				    case SortBy::LoadTime:
					    result = a->GetOperationTime(ExtensionState::Loaded)
					             < b->GetOperationTime(ExtensionState::Loaded);
					    break;
			    }
			    return reverse ? !result : result;
		    }
		);
	}

	// Print dependency tree
	void PrintDependencyTree(
	    const Extension* ext,
	    const Manager& manager,
	    const std::string& prefix = "",
	    bool isLast = true
	) {
		// Print current extension
		std::string connector = isLast ? "└─ " : "├─ ";
		auto [symbol, color] = GetStateInfo(ext->GetState());

		plg::print(
		    "{}{}{} {} {} {}",
		    prefix,
		    connector,
		    Colorize(symbol, color),
		    Colorize(ext->GetName(), Colors::ORANGE),
		    Colorize(ext->GetVersionString(), Colors::GRAY),
		    ext->HasErrors() ? Colorize("[ERROR]", Colors::RED) : ""
		);

		// Print dependencies
		const auto& deps = ext->GetDependencies();
		std::string newPrefix = prefix + (isLast ? "    " : "│   ");

		for (size_t i = 0; i < deps.size(); ++i) {
			const auto& dep = deps[i];
			bool lastDep = (i == deps.size() - 1);

			// Try to find the actual dependency
			if (auto depExt = manager.FindExtension(dep.GetName())) {
				PrintDependencyTree(depExt, manager, newPrefix, lastDep);
			} else {
				std::string depConnector = lastDep ? "└─ " : "├─ ";
				std::string status = dep.IsOptional() ? "[optional]" : "[required]";
				plg::print(
				    "{}{}{} {} {} {} {}",
				    newPrefix,
				    depConnector,
				    Icons.Skipped,
				    dep.GetName(),
				    dep.GetConstraints().to_string(),
				    Colorize(status, Colors::GRAY),
				    Colorize("[NOT FOUND]", Colors::YELLOW)
				);
			}
		}
	}

	std::string GetVersionString() {
		constexpr auto year = std::string_view(__DATE__).substr(7, 4);
		return std::format(
			""
			R"(      ____)" "\n"
			R"( ____|    \         Plugify {})" "\n"
			R"((____|     `._____  Copyright (C) 2023-{} Untrusted Modders Team)" "\n"
			R"( ____|       _|___)" "\n"
			R"((____|     .'       This program may be freely redistributed under)" "\n"
			R"(     |____/         the terms of the MIT License.)",
			s_plugify->GetVersion(),
			year
		);
	}

	struct HealthReport {
		size_t score = 100;  // 0-100
		std::vector<std::string> issues;
		std::vector<std::string> warnings;
		std::flat_map<std::string, size_t> statistics;
	};

	HealthReport CalculateSystemHealth(const Manager& manager) {
		HealthReport report;

		auto allExtensions = manager.GetExtensions();
		report.statistics["total_extensions"] = allExtensions.size();

		size_t failedCount = 0;
		size_t errorCount = 0;
		size_t warningCount = 0;
		size_t slowLoadCount = 0;

		for (const auto& ext : allExtensions) {
			if (ext->GetState() == ExtensionState::Failed
			    || ext->GetState() == ExtensionState::Corrupted) {
				++failedCount;
				report.issues.push_back(std::format("{} is in failed state", ext->GetName()));
			}

			errorCount += ext->GetErrors().size();
			warningCount += ext->GetWarnings().size();

			// Check for slow loading (> 1 second)
			auto loadTime = ext->GetOperationTime(ExtensionState::Loaded);
			if (std::chrono::duration_cast<std::chrono::milliseconds>(loadTime).count() > 1000) {
				++slowLoadCount;
				report.warnings.push_back(
				    std::format("{} took {} to load", ext->GetName(), FormatDuration(loadTime))
				);
			}
		}

		report.statistics["failed_extensions"] = failedCount;
		report.statistics["extensions_with_errors"] = errorCount;
		report.statistics["total_warnings"] = warningCount;
		report.statistics["slow_loading_extensions"] = slowLoadCount;

		// Calculate score
		if (failedCount > 0) {
			report.score -= failedCount * 15;
		}
		if (errorCount > 0) {
			report.score -= errorCount * 10;
		}
		if (warningCount > 0) {
			report.score -= std::min<size_t>(warningCount * 2, 20);
		}
		if (slowLoadCount > 0) {
			report.score -= std::min<size_t>(slowLoadCount * 5, 15);
		}

		report.score = std::max<size_t>(0, report.score);

		return report;
	}

	// Helper function to parse comma-separated values
	std::vector<std::string> ParseCsv(const std::string& str) {
		std::vector<std::string> result;
		std::stringstream ss(str);
		std::string item;
		while (std::getline(ss, item, ',')) {
			// Trim whitespace
			item.erase(0, item.find_first_not_of(" \t"));
			item.erase(item.find_last_not_of(" \t") + 1);
			if (!item.empty()) {
				result.push_back(item);
			}
		}
		return result;
	};

	// Helper to parse sort option
	SortBy ParseSortBy(std::string_view str) {
		if (str == "name") {
			return SortBy::Name;
		}
		if (str == "version") {
			return SortBy::Version;
		}
		if (str == "state") {
			return SortBy::State;
		}
		if (str == "language") {
			return SortBy::Language;
		}
		if (str == "loadtime") {
			return SortBy::LoadTime;
		}
		return SortBy::Name;
	};

	// Helper to parse state filter
	std::vector<ExtensionState> ParseStates(const std::vector<std::string>& strs) {
		std::vector<ExtensionState> states;
		states.reserve(strs.size());
		for (const auto& str : strs) {
			std::string lower = str;
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

			if (lower == "loaded") {
				states.push_back(ExtensionState::Loaded);
			} else if (lower == "started") {
				states.push_back(ExtensionState::Started);
			} else if (lower == "failed") {
				states.push_back(ExtensionState::Failed);
			} else if (lower == "disabled") {
				states.push_back(ExtensionState::Disabled);
			} else if (lower == "corrupted") {
				states.push_back(ExtensionState::Corrupted);
			} else if (lower == "unresolved") {
				states.push_back(ExtensionState::Unresolved);
			}
			// Add more as needed
		}
		return states;
	};

	bool CheckManager() {
		if (!s_plugify->IsInitialized()) {
			plg::print("{}: Initialize system before use.", Colorize("Error", Colors::RED));
			return false;
		}
		const auto& manager = s_plugify->GetManager();
		if (!manager.IsInitialized()) {
			plg::print(
			    "{}: You must load plugin manager before query any information from it.",
			    Colorize("Error", Colors::RED)
			);
			return false;
		}
		return true;
	}

	void LoadManager() {
		if (!s_plugify->IsInitialized()) {
			plg::print("{}: Initialize system before use.", Colorize("Error", Colors::RED));
			return;
		}
		const auto& manager = s_plugify->GetManager();
		if (manager.IsInitialized()) {
			plg::print("{}: Plugin manager already loaded.", Colorize("Error", Colors::RED));
		} else {
			s_state = PlugifyState::Load;
		}
	}

	void UnloadManager() {
		if (!s_plugify->IsInitialized()) {
			plg::print("{}: Initialize system before use.", Colorize("Error", Colors::RED));
			return;
		}
		const auto& manager = s_plugify->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: Plugin manager already unloaded.", Colorize("Error", Colors::RED));
		} else {
			s_state = PlugifyState::Unload;
		}
	}

	void ReloadManager() {
		if (!s_plugify->IsInitialized()) {
			plg::print("{}: Initialize system before use.", Colorize("Error", Colors::RED));
			return;
		}
		const auto& manager = s_plugify->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: Plugin manager not loaded.", Colorize("Warning", Colors::YELLOW));
		} else {
			s_state = PlugifyState::Reload;
		}
	}

	void ListPlugins(
	    const FilterOptions& filter = {},
	    SortBy sortBy = SortBy::Name,
	    bool reverseSort = false,
	    bool jsonOutput = false
	) {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto plugins = manager.GetExtensionsByType(ExtensionType::Plugin);

		// Apply filters
		auto filtered = FilterExtensions(plugins, filter);
		SortExtensions(filtered, sortBy, reverseSort);

		// Output
		if (jsonOutput) {
			glz::json_t::array_t objects;
			objects.reserve(filtered.size());
			for (const auto& plugin : filtered) {
				objects.emplace_back(ExtensionToJson(plugin));
			}
			plg::print(*glz::json_t{ std::move(objects) }.dump());
			return;
		}

		auto count = filtered.size();
		if (!count) {
			plg::print(Colorize("No plugins found matching criteria.", Colors::YELLOW));
			return;
		}

		plg::print(
		    "{}:",
		    Colorize(std::format("Listing {} plugin{}", count, (count > 1) ? "s" : ""), Colors::ORANGE)
		);
		plg::print(SEPARATOR_LINE);

		// Header
		plg::print(
		    "{} {} {} {} {} {}",
		    Colorize(std::format("{:<3}", Icons.Number), Colors::GRAY),
		    Colorize(std::format("{:<25}", "Name"), Colors::GRAY),
		    Colorize(std::format("{:<15}", "Version"), Colors::GRAY),
		    Colorize(std::format("{:<12}", "State"), Colors::GRAY),
		    Colorize(std::format("{:<8}", "Lang"), Colors::GRAY),
		    Colorize(std::format("{:<12}", "Load Time"), Colors::GRAY)
		);
		plg::print(SEPARATOR_LINE);

		size_t index = 1;
		for (const auto& plugin : filtered) {
			auto state = plugin->GetState();
			auto stateStr = plg::enum_to_string(state);
			auto [symbol, color] = GetStateInfo(state);
			const auto& name = !plugin->GetName().empty()
			                       ? plugin->GetName()
			                       : plg::as_string(plugin->GetLocation().filename());

			// Get load time if available
			std::string loadTime = "N/A";
			try {
				auto duration = plugin->GetOperationTime(ExtensionState::Loaded);
				if (duration.count() > 0) {
					loadTime = FormatDuration(duration);
				}
			} catch (...) {
			}

			plg::print(
			    "{:<3} {:<25} {:<15} {} {:<11} {:<8} {:<12}",
			    index++,
			    Truncate(name, 24),
			    plugin->GetVersionString(),
			    Colorize(symbol, color),
			    Truncate(std::string(stateStr), 10),
			    Truncate(plugin->GetLanguage(), 7),
			    loadTime
			);

			// Show errors/warnings if any
			if (plugin->HasErrors()) {
				for (const auto& error : plugin->GetErrors()) {
					plg::print("     └─ {}: {}", Colorize("Error", Colors::RED), error);
				}
			}
			if (plugin->HasWarnings()) {
				for (const auto& warning : plugin->GetWarnings()) {
					plg::print("     └─ {}: {}", Colorize("Warning", Colors::YELLOW), warning);
				}
			}
		}
		plg::print(SEPARATOR_LINE);

		// Summary
		if (filter.states.has_value() || filter.languages.has_value()
		    || filter.searchQuery.has_value() || filter.showOnlyFailed) {
			plg::print(
			    "{}",
			    Colorize(
			        std::format("Filtered: {} of {} total plugins shown", filtered.size(), plugins.size()),
			        Colors::GRAY
			    )
			);
		}
	}

	void ListModules(
	    const FilterOptions& filter = {},
	    SortBy sortBy = SortBy::Name,
	    bool reverseSort = false,
	    bool jsonOutput = false
	) {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto modules = manager.GetExtensionsByType(ExtensionType::Module);

		// Apply filters
		auto filtered = FilterExtensions(modules, filter);
		SortExtensions(filtered, sortBy, reverseSort);

		// Output
		if (jsonOutput) {
			glz::json_t::array_t objects;
			objects.reserve(filtered.size());
			for (const auto& module : filtered) {
				objects.emplace_back(ExtensionToJson(module));
			}
			plg::print(*glz::json_t{ std::move(objects) }.dump());
			return;
		}

		auto count = filtered.size();
		if (!count) {
			plg::print(Colorize("No modules found matching criteria.", Colors::YELLOW));
			return;
		}

		plg::print(
		    "{}:",
		    Colorize(std::format("Listing {} module{}", count, (count > 1) ? "s" : ""), Colors::ORANGE)
		);
		plg::print(SEPARATOR_LINE);

		// Header
		plg::print(
		    "{} {} {} {} {} {}",
		    Colorize(std::format("{:<3}", Icons.Number), Colors::GRAY),
		    Colorize(std::format("{:<25}", "Name"), Colors::GRAY),
		    Colorize(std::format("{:<15}", "Version"), Colors::GRAY),
		    Colorize(std::format("{:<12}", "State"), Colors::GRAY),
		    Colorize(std::format("{:<8}", "Lang"), Colors::GRAY),
		    Colorize(std::format("{:<12}", "Load Time"), Colors::GRAY)
		);
		plg::print(SEPARATOR_LINE);

		size_t index = 1;
		for (const auto& module : filtered) {
			auto state = module->GetState();
			auto stateStr = plg::enum_to_string(state);
			auto [symbol, color] = GetStateInfo(state);

			// Get load time if available
			std::string loadTime = "N/A";
			try {
				auto duration = module->GetOperationTime(ExtensionState::Loaded);
				if (duration.count() > 0) {
					loadTime = FormatDuration(duration);
				}
			} catch (...) {
			}

			plg::print(
			    "{:<3} {:<25} {:<15} {} {:<11} {:<8} {:<12}",
			    index++,
			    Truncate(module->GetName(), 24),
			    module->GetVersionString(),
			    Colorize(symbol, color),
			    Truncate(std::string(stateStr), 10),
			    Truncate(module->GetLanguage(), 7),
			    loadTime
			);

			// Show errors/warnings if any
			if (module->HasErrors()) {
				for (const auto& error : module->GetErrors()) {
					plg::print("     └─ {}: {}", Colorize("Error", Colors::RED), error);
				}
			}
			if (module->HasWarnings()) {
				for (const auto& warning : module->GetWarnings()) {
					plg::print("     └─ {}: {}", Colorize("Warning", Colors::YELLOW), warning);
				}
			}
		}
		plg::print(SEPARATOR_LINE);

		// Summary
		if (filter.states.has_value() || filter.languages.has_value()
		    || filter.searchQuery.has_value() || filter.showOnlyFailed) {
			plg::print(
			    "{}",
			    Colorize(
			        std::format("Filtered: {} of {} total modules shown", filtered.size(), modules.size()),
			        Colors::GRAY
			    )
			);
		}
	}

	void ShowPlugin(std::string_view identifier, bool plugin_use_id, bool jsonOutput) {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto plugin = plugin_use_id ? manager.FindExtension(FormatId(identifier))
		                            : manager.FindExtension(identifier);

		if (!plugin) {
			if (jsonOutput) {
				glz::json_t error;
				error["error"] = std::format("Plugin {} not found", identifier);
				plg::print(*error.dump());
			} else {
				plg::print("{}: Plugin {} not found.", Colorize("Error", Colors::RED), identifier);
			}
			return;
		}

		if (!plugin->IsPlugin()) {
			if (jsonOutput) {
				glz::json_t error;
				error["error"] = std::format("'{}' is not a plugin (it's a module)", identifier);
				plg::print(*error.dump());
			} else {
				plg::print(
				    "{}: '{}' is not a plugin (it's a module).",
				    Colorize("Error", Colors::RED),
				    identifier
				);
			}
			return;
		}

		// JSON output
		if (jsonOutput) {
			plg::print(*ExtensionToJson(plugin).dump());
			return;
		}

		// Display detailed plugin information with colors
		plg::print(DOUBLE_LINE);
		plg::print(
		    "{}: {}",
		    Colorize("PLUGIN INFORMATION", Colors::ORANGE),
		    Colorize(plugin->GetName(), Colors::CYAN)
		);
		plg::print(DOUBLE_LINE);

		// Status indicator
		auto [symbol, stateColor] = GetStateInfo(plugin->GetState());
		plg::print(
		    "\n{} {} {}",
		    Colorize(symbol, stateColor),
		    Colorize("Status:", Colors::ORANGE),
		    Colorize(plg::enum_to_string(plugin->GetState()), stateColor)
		);

		// Basic Information
		plg::print(Colorize("\n[Basic Information]", Colors::CYAN));
		plg::print("  {:<15} {}", Colorize("ID:", Colors::GRAY), plugin->GetId());
		plg::print("  {:<15} {}", Colorize("Name:", Colors::GRAY), plugin->GetName());
		plg::print(
		    "  {:<15} {}",
		    Colorize("Version:", Colors::GRAY),
		    Colorize(plugin->GetVersionString(), Colors::GREEN)
		);
		plg::print("  {:<15} {}", Colorize("Language:", Colors::GRAY), plugin->GetLanguage());
		plg::print(
		    "  {:<15} {}",
		    Colorize("Location:", Colors::GRAY),
		    plg::as_string(plugin->GetLocation())
		);
		plg::print(
		    "  {:<15} {}",
		    Colorize("File Size:", Colors::GRAY),
		    FormatFileSize(plugin->GetLocation())
		);

		// Optional Information
		if (!plugin->GetDescription().empty() || !plugin->GetAuthor().empty()
		    || !plugin->GetWebsite().empty() || !plugin->GetLicense().empty()) {
			plg::print(Colorize("\n[Additional Information]", Colors::CYAN));
			if (!plugin->GetDescription().empty()) {
				plg::print(
				    "  {:<15} {}",
				    Colorize("Description:", Colors::GRAY),
				    plugin->GetDescription()
				);
			}
			if (!plugin->GetAuthor().empty()) {
				plg::print(
				    "  {:<15} {}",
				    Colorize("Author:", Colors::GRAY),
				    Colorize(plugin->GetAuthor(), Colors::MAGENTA)
				);
			}
			if (!plugin->GetWebsite().empty()) {
				plg::print(
				    "  {:<15} {}",
				    Colorize("Website:", Colors::GRAY),
				    Colorize(plugin->GetWebsite(), Colors::BLUE)
				);
			}
			if (!plugin->GetLicense().empty()) {
				plg::print("  {:<15} {}", Colorize("License:", Colors::GRAY), plugin->GetLicense());
			}
		}

		// Plugin-specific information
		if (!plugin->GetEntry().empty()) {
			plg::print(Colorize("\n[Plugin Details]", Colors::CYAN));
			plg::print(
			    "  {:<15} {}",
			    Colorize("Entry Point:", Colors::GRAY),
			    Colorize(plugin->GetEntry(), Colors::YELLOW)
			);
		}

		// Methods
		const auto& methods = plugin->GetMethods();
		if (!methods.empty()) {
			plg::print(
			    Colorize("\n[Exported Methods]", Colors::CYAN)
			    + Colorize(std::format(" ({} total)", methods.size()), Colors::GRAY)
			);

			size_t displayCount = std::min<size_t>(methods.size(), 10);
			for (size_t i = 0; i < displayCount; ++i) {
				const auto& method = methods[i];
				plg::print(
				    "  {}{:<2} {}",
				    Colorize(Icons.Number, Colors::GRAY),
				    i + 1,
				    Colorize(method.GetName(), Colors::GREEN)
				);
				if (!method.GetFuncName().empty()) {
					plg::print(
					    "      {} {}",
					    Colorize("Func Name:", Colors::GRAY),
					    method.GetFuncName()
					);
				}
			}
			if (methods.size() > 10) {
				plg::print(
				    "  {} ... and {} more methods",
				    Colorize(Icons.Arrow, Colors::GRAY),
				    methods.size() - 10
				);
			}
		}

		// Platforms
		const auto& platforms = plugin->GetPlatforms();
		if (!platforms.empty()) {
			plg::print(Colorize("\n[Supported Platforms]", Colors::CYAN));
			plg::print("  {}", Colorize(plg::join(platforms, ", "), Colors::GREEN));
		}

		// Dependencies
		const auto& deps = plugin->GetDependencies();
		if (!deps.empty()) {
			plg::print(
			    Colorize("\n[Dependencies]", Colors::CYAN)
			    + Colorize(std::format(" ({} total)", deps.size()), Colors::GRAY)
			);
			for (const auto& dep : deps) {
				std::string depIndicator = dep.IsOptional() ? Colorize(Icons.Skipped, Colors::GRAY)
				                                            : Colorize(Icons.Valid, Colors::GREEN);
				plg::print(
				    "  {} {} {}",
				    depIndicator,
				    Colorize(dep.GetName(), Colors::ORANGE),
				    Colorize(dep.GetConstraints().to_string(), Colors::GRAY)
				);
				if (dep.IsOptional()) {
					plg::print("    └─ {}", Colorize("Optional", Colors::GRAY));
				}
			}
		}

		// Conflicts
		const auto& conflicts = plugin->GetConflicts();
		if (!conflicts.empty()) {
			plg::print(
			    Colorize("\n[Conflicts]", Colors::YELLOW)
			    + Colorize(std::format(" ({} total)", conflicts.size()), Colors::GRAY)
			);
			for (const auto& conflict : conflicts) {
				plg::print(
				    "  {} {} {}",
				    Colorize(Icons.Warning, Colors::YELLOW),
				    conflict.GetName(),
				    Colorize(conflict.GetConstraints().to_string(), Colors::GRAY)
				);
				if (!conflict.GetReason().empty()) {
					plg::print("    └─ {}", Colorize(conflict.GetReason(), Colors::RED));
				}
			}
		}

		// Performance Information
		plg::print(Colorize("\n[Performance Metrics]", Colors::CYAN));
		auto totalTime = plugin->GetTotalTime();
		plg::print(
		    "  {:<15} {}",
		    Colorize("Total Time:", Colors::GRAY),
		    Colorize(
		        FormatDuration(totalTime),
		        totalTime > std::chrono::seconds(1) ? Colors::YELLOW : Colors::GREEN
		    )
		);

		// Show timing for different operations
		ExtensionState operations[] = {
			ExtensionState::Parsing,
			ExtensionState::Resolving,
			ExtensionState::Loading,
			ExtensionState::Starting,
		};

		for (const auto& op : operations) {
			try {
				auto duration = plugin->GetOperationTime(op);
				if (duration.count() > 0) {
					bool slow = duration > std::chrono::milliseconds(500);
					plg::print(
					    "  {:<15} {}",
					    Colorize(std::format("{}:", plg::enum_to_string(op)), Colors::GRAY),
					    Colorize(FormatDuration(duration), slow ? Colors::YELLOW : Colors::GREEN)
					);
				}
			} catch (...) {
			}
		}

		// Errors and Warnings
		if (plugin->HasErrors() || plugin->HasWarnings()) {
			plg::print(Colorize("\n[Issues]", Colors::RED));
			for (const auto& error : plugin->GetErrors()) {
				plg::print("  {} {}", Colorize("ERROR:", Colors::RED), error);
			}
			for (const auto& warning : plugin->GetWarnings()) {
				plg::print("  {} {}", Colorize("WARNING:", Colors::YELLOW), warning);
			}
		} else {
			plg::print(
			    "\n{} {}",
			    Colorize(Icons.Ok, Colors::GREEN),
			    Colorize("No issues detected", Colors::GREEN)
			);
		}

		plg::print(DOUBLE_LINE);
	}

	void ShowModule(std::string_view identifier, bool module_use_id, bool jsonOutput) {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto module = module_use_id ? manager.FindExtension(FormatId(identifier))
		                            : manager.FindExtension(identifier);

		if (!module) {
			if (jsonOutput) {
				glz::json_t error;
				error["error"] = std::format("Module {} not found", identifier);
				plg::print(*error.dump());
			} else {
				plg::print("{}: Module {} not found.", Colorize("Error", Colors::RED), identifier);
			}
			return;
		}

		if (!module->IsModule()) {
			if (jsonOutput) {
				glz::json_t error;
				error["error"] = std::format("'{}' is not a module (it's a plugin)", identifier);
				plg::print(*error.dump());
			} else {
				plg::print(
				    "{}: '{}' is not a module (it's a plugin).",
				    Colorize("Error", Colors::RED),
				    identifier
				);
			}
			return;
		}

		// JSON output
		if (jsonOutput) {
			plg::print(*ExtensionToJson(module).dump());
			return;
		}

		// Display detailed module information with colors
		plg::print(DOUBLE_LINE);
		plg::print(
		    "{}: {}",
		    Colorize("MODULE INFORMATION", Colors::ORANGE),
		    Colorize(module->GetName(), Colors::MAGENTA)
		);
		plg::print(DOUBLE_LINE);

		// Status indicator
		auto [symbol, stateColor] = GetStateInfo(module->GetState());
		plg::print(
		    "\n{} {} {}",
		    Colorize(symbol, stateColor),
		    Colorize("Status:", Colors::ORANGE),
		    Colorize(plg::enum_to_string(module->GetState()), stateColor)
		);

		// Basic Information
		plg::print(Colorize("\n[Basic Information]", Colors::CYAN));
		plg::print("  {:<15} {}", Colorize("ID:", Colors::GRAY), module->GetId());
		plg::print("  {:<15} {}", Colorize("Name:", Colors::GRAY), module->GetName());
		plg::print(
		    "  {:<15} {}",
		    Colorize("Version:", Colors::GRAY),
		    Colorize(module->GetVersionString(), Colors::GREEN)
		);
		plg::print("  {:<15} {}", Colorize("Language:", Colors::GRAY), module->GetLanguage());
		plg::print(
		    "  {:<15} {}",
		    Colorize("Location:", Colors::GRAY),
		    plg::as_string(module->GetLocation())
		);
		plg::print(
		    "  {:<15} {}",
		    Colorize("File Size:", Colors::GRAY),
		    FormatFileSize(module->GetLocation())
		);

		// Optional Information
		if (!module->GetDescription().empty() || !module->GetAuthor().empty()
		    || !module->GetWebsite().empty() || !module->GetLicense().empty()) {
			plg::print(Colorize("\n[Additional Information]", Colors::CYAN));
			if (!module->GetDescription().empty()) {
				plg::print(
				    "  {:<15} {}",
				    Colorize("Description:", Colors::GRAY),
				    module->GetDescription()
				);
			}
			if (!module->GetAuthor().empty()) {
				plg::print(
				    "  {:<15} {}",
				    Colorize("Author:", Colors::GRAY),
				    Colorize(module->GetAuthor(), Colors::MAGENTA)
				);
			}
			if (!module->GetWebsite().empty()) {
				plg::print(
				    "  {:<15} {}",
				    Colorize("Website:", Colors::GRAY),
				    Colorize(module->GetWebsite(), Colors::BLUE)
				);
			}
			if (!module->GetLicense().empty()) {
				plg::print("  {:<15} {}", Colorize("License:", Colors::GRAY), module->GetLicense());
			}
		}

		// Module-specific information
		if (!module->GetRuntime().empty()) {
			plg::print(Colorize("\n[Module Details]", Colors::CYAN));
			plg::print(
			    "  {:<15} {}",
			    Colorize("Runtime:", Colors::GRAY),
			    Colorize(plg::as_string(module->GetRuntime()), Colors::YELLOW)
			);
		}

		// Directories
		const auto& dirs = module->GetDirectories();
		if (!dirs.empty()) {
			plg::print(
			    Colorize("\n[Search Directories]", Colors::CYAN)
			    + Colorize(std::format(" ({} total)", dirs.size()), Colors::GRAY)
			);

			size_t displayCount = std::min<size_t>(dirs.size(), 5);
			for (size_t i = 0; i < displayCount; ++i) {
				std::error_code ec;
				bool exists = fs::exists(dirs[i], ec);
				plg::print(
				    "  {} {}",
				    exists ? Colorize(Icons.Ok, Colors::GREEN) : Colorize(Icons.Fail, Colors::RED),
				    plg::as_string(dirs[i])
				);
			}
			if (dirs.size() > 5) {
				plg::print(
				    "  {} ... and {} more directories",
				    Colorize(Icons.Arrow, Colors::GRAY),
				    dirs.size() - 5
				);
			}
		}

		// Assembly Information
		if (auto assembly = module->GetAssembly()) {
			plg::print(Colorize("\n[Assembly Information]", Colors::CYAN));
			plg::print("  {} Assembly loaded and active", Colorize(Icons.Ok, Colors::GREEN));
			// Add more assembly details if available in your IAssembly interface
		}

		// Platforms
		const auto& platforms = module->GetPlatforms();
		if (!platforms.empty()) {
			plg::print(Colorize("\n[Supported Platforms]", Colors::CYAN));
			plg::print("  {}", Colorize(plg::join(platforms, ", "), Colors::GREEN));
		}

		// Dependencies
		const auto& deps = module->GetDependencies();
		if (!deps.empty()) {
			plg::print(
			    Colorize("\n[Dependencies]", Colors::CYAN)
			    + Colorize(std::format(" ({} total)", deps.size()), Colors::GRAY)
			);
			for (const auto& dep : deps) {
				std::string depIndicator = dep.IsOptional() ? Colorize(Icons.Skipped, Colors::GRAY)
				                                            : Colorize(Icons.Valid, Colors::GREEN);
				plg::print(
				    "  {} {} {}",
				    depIndicator,
				    Colorize(dep.GetName(), Colors::ORANGE),
				    Colorize(dep.GetConstraints().to_string(), Colors::GRAY)
				);
				if (dep.IsOptional()) {
					plg::print("    └─ {}", Colorize("Optional", Colors::GRAY));
				}
			}
		}

		// Conflicts
		const auto& conflicts = module->GetConflicts();
		if (!conflicts.empty()) {
			plg::print(
			    Colorize("\n[Conflicts]", Colors::YELLOW)
			    + Colorize(std::format(" ({} total)", conflicts.size()), Colors::GRAY)
			);
			for (const auto& conflict : conflicts) {
				plg::print(
				    "  {} {} {}",
				    Colorize(Icons.Warning, Colors::YELLOW),
				    conflict.GetName(),
				    Colorize(conflict.GetConstraints().to_string(), Colors::GRAY)
				);
				if (!conflict.GetReason().empty()) {
					plg::print("    └─ {}", Colorize(conflict.GetReason(), Colors::RED));
				}
			}
		}

		// Performance Information
		plg::print(Colorize("\n[Performance Metrics]", Colors::CYAN));
		auto totalTime = module->GetTotalTime();
		plg::print(
		    "  {:<15} {}",
		    Colorize("Total Time:", Colors::GRAY),
		    Colorize(
		        FormatDuration(totalTime),
		        totalTime > std::chrono::seconds(1) ? Colors::YELLOW : Colors::GREEN
		    )
		);

		// Show timing for different operations
		ExtensionState operations[] = {
			ExtensionState::Parsing,
			ExtensionState::Resolving,
			ExtensionState::Loading,
			ExtensionState::Starting,
		};

		for (const auto& op : operations) {
			try {
				auto duration = module->GetOperationTime(op);
				if (duration.count() > 0) {
					bool slow = duration > std::chrono::milliseconds(500);
					plg::print(
					    "  {:<15} {}",
					    Colorize(std::format("{}:", plg::enum_to_string(op)), Colors::GRAY),
					    Colorize(FormatDuration(duration), slow ? Colors::YELLOW : Colors::GREEN)
					);
				}
			} catch (...) {
			}
		}

		// Errors and Warnings
		if (module->HasErrors() || module->HasWarnings()) {
			plg::print(Colorize("\n[Issues]", Colors::RED));
			for (const auto& error : module->GetErrors()) {
				plg::print("  {} {}", Colorize("ERROR:", Colors::RED), error);
			}
			for (const auto& warning : module->GetWarnings()) {
				plg::print("  {} {}", Colorize("WARNING:", Colors::YELLOW), warning);
			}
		} else {
			plg::print(
			    "\n{} {}",
			    Colorize(Icons.Ok, Colors::GREEN),
			    Colorize("No issues detected", Colors::GREEN)
			);
		}

		plg::print(DOUBLE_LINE);
	}

	void ShowHealth() {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto report = CalculateSystemHealth(manager);

		// Determine health status color
		ColorCode scoreColor = Colors::GREEN;
		std::string status = "HEALTHY";
		if (report.score < 50) {
			scoreColor = Colors::RED;
			status = "CRITICAL";
		} else if (report.score < 75) {
			scoreColor = Colors::YELLOW;
			status = "WARNING";
		}

		plg::print(DOUBLE_LINE);
		plg::print(Colorize("SYSTEM HEALTH CHECK", Colors::ORANGE));
		plg::print(DOUBLE_LINE);

		// Overall score
		plg::print(
		    "\n{}: {} {}",
		    Colorize("Overall Health Score", Colors::ORANGE),
		    Colorize(std::format("{}/100", report.score), scoreColor),
		    Colorize(std::format("[{}]", status), scoreColor)
		);

		// Statistics
		plg::print(Colorize("\n[Statistics]", Colors::CYAN));
		plg::print("  Total Extensions:        {}", report.statistics["total_extensions"]);
		plg::print(
		    "  Failed Extensions:       {} {}",
		    report.statistics["failed_extensions"],
		    report.statistics["failed_extensions"] > 0 ? Colorize(Icons.Warning, Colors::RED)
		                                               : Colorize(Icons.Ok, Colors::GREEN)
		);
		plg::print(
		    "  Extensions with Errors:  {} {}",
		    report.statistics["extensions_with_errors"],
		    report.statistics["extensions_with_errors"] > 0 ? Colorize(Icons.Warning, Colors::YELLOW)
		                                                    : Colorize(Icons.Ok, Colors::GREEN)
		);
		plg::print("  Total Warnings:          {}", report.statistics["total_warnings"]);
		plg::print("  Slow Loading Extensions: {}", report.statistics["slow_loading_extensions"]);

		// Issues
		if (!report.issues.empty()) {
			plg::print(Colorize("\n[Critical Issues]", Colors::RED));
			for (const auto& issue : report.issues) {
				plg::print("  {} {}", Colorize(Icons.Fail, Colors::RED), issue);
			}
		}

		// Warnings
		if (!report.warnings.empty()) {
			plg::print(Colorize("\n[Warnings]", Colors::YELLOW));
			for (const auto& warning : report.warnings) {
				plg::print("  {} {}", Colorize(Icons.Warning, Colors::YELLOW), warning);
			}
		}

		// Recommendations
		plg::print(Colorize("\n[Recommendations]", Colors::CYAN));
		if (report.score == 100) {
			plg::print("  {} System is running optimally!", Colorize(Icons.Ok, Colors::GREEN));
		} else {
			if (report.statistics["failed_extensions"] > 0) {
				plg::print("  • Fix or remove failed extensions");
			}
			if (report.statistics["extensions_with_errors"] > 0) {
				plg::print("  • Review and resolve extension errors");
			}
			if (report.statistics["slow_loading_extensions"] > 0) {
				plg::print("  • Investigate slow-loading extensions for optimization");
			}
		}

		plg::print(DOUBLE_LINE);
	}

	void ShowDependencyTree(std::string_view name, bool useId = false) {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto ext = useId ? manager.FindExtension(FormatId(name)) : manager.FindExtension(name);

		if (!ext) {
			plg::print("{} {} not found.", Colorize("Error:", Colors::RED), name);
			return;
		}

		plg::print(DOUBLE_LINE);
		plg::print("{}: {}", Colorize("DEPENDENCY TREE", Colors::ORANGE), ext->GetName());
		plg::print(DOUBLE_LINE);
		plg::print("");

		PrintDependencyTree(ext, manager);

		// Also show what depends on this extension
		plg::print(Colorize("\n[Reverse Dependencies]", Colors::CYAN));
		plg::print("Extensions that depend on this:");

		bool found = false;
		for (const auto& other : manager.GetExtensions()) {
			for (const auto& dep : other->GetDependencies()) {
				if (dep.GetName() == ext->GetName()) {
					plg::print(
					    "  • {} {}",
					    other->GetName(),
					    dep.IsOptional() ? Colorize("[optional]", Colors::GRAY) : ""
					);
					found = true;
				}
			}
		}

		if (!found) {
			plg::print("  {}", Colorize("None", Colors::GRAY));
		}

		plg::print(DOUBLE_LINE);
	}

	void SearchExtensions(std::string_view query) {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto allExtensions = manager.GetExtensions();

		std::vector<const Extension*> matches;
		std::string lowerQuery(query);
		std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

		for (const auto& ext : allExtensions) {
			std::string name = ext->GetName();
			std::string desc = ext->GetDescription();
			std::string author = ext->GetAuthor();

			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
			std::transform(author.begin(), author.end(), author.begin(), ::tolower);

			if (name.find(lowerQuery) != std::string::npos || desc.find(lowerQuery) != std::string::npos
			    || author.find(lowerQuery) != std::string::npos) {
				matches.push_back(ext);
			}
		}

		if (matches.empty()) {
			plg::print(
			    "{} No extensions found matching '{}'",
			    Colorize(Icons.Missing, Colors::YELLOW),
			    query
			);
			return;
		}

		plg::print(
		    "{}: Found {} match{} for '{}'",
		    Colorize("SEARCH RESULTS", Colors::ORANGE),
		    matches.size(),
		    matches.size() > 1 ? "es" : "",
		    query
		);
		plg::print(SEPARATOR_LINE);

		for (const auto& ext : matches) {
			auto [symbol, color] = GetStateInfo(ext->GetState());
			plg::print(
			    "{} {} {} {} {}",
			    Colorize(symbol, color),
			    Colorize(ext->GetName(), Colors::ORANGE),
			    Colorize(ext->GetVersionString(), Colors::GRAY),
			    ext->IsPlugin() ? "[Plugin]" : "[Module]",
			    Colorize(std::format("({})", ext->GetLanguage()), Colors::GRAY)
			);

			if (!ext->GetDescription().empty()) {
				plg::print("  {}", Truncate(ext->GetDescription(), 70));
			}
		}
		plg::print(SEPARATOR_LINE);
	}

	void ValidateExtension(const fs::path& path) {
		plg::print("{}: {}", Colorize("VALIDATING", Colors::ORANGE), plg::as_string(path));
		plg::print(SEPARATOR_LINE);

		// Check if file exists
		std::error_code ec;
		if (!fs::exists(path, ec)) {
			plg::print("{} File does not exist", Colorize("✗", Colors::RED));
			return;
		}

		// Check file extension
		const auto& fileExt = plg::as_string(path.extension());
		bool isPlugin = (fileExt == ".plg" || fileExt == ".pplugin");
		bool isModule = (fileExt == ".mod" || fileExt == ".pmodule");

		if (!isPlugin && !isModule) {
			plg::print("{} Invalid file extension: {}", Colorize("✗", Colors::RED), fileExt);
			return;
		}

		plg::print("{} File exists", Colorize(Icons.Ok, Colors::GREEN));
		plg::print(
		    "{} Valid extension type: {}",
		    Colorize(Icons.Ok, Colors::GREEN),
		    isPlugin ? "Plugin" : "Module"
		);
		plg::print("{} File size: {}", Colorize(Icons.Missing, Colors::CYAN), FormatFileSize(path));

		// Try to parse manifest (you'd need to implement manifest parsing)
		// This is a placeholder for actual validation logic
		/*plg::print("{} Manifest validation: {}",
		            Colorize(Icons.Missing, Colors::CYAN),
		            "Would parse and validate manifest here");*/
		// TODO

		plg::print(SEPARATOR_LINE);
		plg::print("{}: Validation complete", Colorize("RESULT", Colors::ORANGE));
	}

	void CompareExtensions(std::string_view name1, std::string_view name2, bool useId = false) {
		if (!CheckManager()) {
			return;
		}

		const auto& manager = s_plugify->GetManager();
		auto ext1 = useId ? manager.FindExtension(FormatId(name1)) : manager.FindExtension(name1);
		auto ext2 = useId ? manager.FindExtension(FormatId(name2)) : manager.FindExtension(name2);

		if (!ext1) {
			plg::print("{}: Extension {} not found.", Colorize("Error", Colors::RED), name1);
			return;
		}
		if (!ext2) {
			plg::print("{}: Extension {} not found.", Colorize("Error", Colors::RED), name2);
			return;
		}

		plg::print(DOUBLE_LINE);
		plg::print(Colorize("EXTENSION COMPARISON", Colors::ORANGE));
		plg::print(DOUBLE_LINE);

		// Basic comparison table
		auto printRow = [](std::string_view label, std::string_view val1, std::string_view val2) {
			bool same = (val1 == val2);
			plg::print("{:<20} {:<25} {} {:<25}", label, val1, same ? Icons.Equal : Icons.NotEqual, val2);
		};

		plg::print(
		    "\n{:<20} {:<25}   {:<25}",
		    "",
		    Colorize(ext1->GetName(), Colors::CYAN),
		    Colorize(ext2->GetName(), Colors::MAGENTA)
		);
		plg::print(SEPARATOR_LINE);

		printRow("Type:", ext1->IsPlugin() ? "Plugin" : "Module", ext2->IsPlugin() ? "Plugin" : "Module");
		printRow("Version:", ext1->GetVersionString(), ext2->GetVersionString());
		printRow("Language:", ext1->GetLanguage(), ext2->GetLanguage());
		printRow("State:", plg::enum_to_string(ext1->GetState()), plg::enum_to_string(ext2->GetState()));
		printRow("Author:", ext1->GetAuthor(), ext2->GetAuthor());
		printRow("License:", ext1->GetLicense(), ext2->GetLicense());

		// Dependencies comparison
		plg::print(Colorize("\n[Dependencies]", Colors::ORANGE));
		auto deps1 = ext1->GetDependencies();
		auto deps2 = ext2->GetDependencies();

		std::set<std::string> depNames1, depNames2;
		for (const auto& d : deps1) {
			depNames1.insert(d.GetName());
		}
		for (const auto& d : deps2) {
			depNames2.insert(d.GetName());
		}

		std::vector<std::string> onlyIn1, onlyIn2, common;
		std::set_difference(
		    depNames1.begin(),
		    depNames1.end(),
		    depNames2.begin(),
		    depNames2.end(),
		    std::back_inserter(onlyIn1)
		);
		std::set_difference(
		    depNames2.begin(),
		    depNames2.end(),
		    depNames1.begin(),
		    depNames1.end(),
		    std::back_inserter(onlyIn2)
		);
		std::set_intersection(
		    depNames1.begin(),
		    depNames1.end(),
		    depNames2.begin(),
		    depNames2.end(),
		    std::back_inserter(common)
		);

		if (!common.empty()) {
			plg::print("  Common: {}", plg::join(common, ", "));
		}
		if (!onlyIn1.empty()) {
			plg::print("  Only in {}: {}", ext1->GetName(), plg::join(onlyIn1, ", "));
		}
		if (!onlyIn2.empty()) {
			plg::print("  Only in {}: {}", ext2->GetName(), plg::join(onlyIn2, ", "));
		}

		// Performance comparison
		plg::print(Colorize("\n[Performance]", Colors::ORANGE));
		plg::print(
		    "  Load Time:     {:<15} vs {:<15}",
		    FormatDuration(ext1->GetOperationTime(ExtensionState::Loaded)),
		    FormatDuration(ext2->GetOperationTime(ExtensionState::Loaded))
		);
		plg::print(
		    "  Total Time:    {:<15} vs {:<15}",
		    FormatDuration(ext1->GetTotalTime()),
		    FormatDuration(ext2->GetTotalTime())
		);

		plg::print(DOUBLE_LINE);
	}
};

// Main command handler using CLI11
CON_COMMAND_F(plugify, "Plugify control options", FCVAR_NONE) {
	if (!s_plugify || !s_plugify->IsInitialized()) {
		plg::print("{}: Initialize system before use.", Colorize("Error", Colors::RED));
		return;
	}

	// Create a temporary CLI app for parsing
	CLI::App app{ "Plugify Management System" };
	app.require_subcommand();  // 1 or more
	// interactiveApp.allow_extras();
	// interactiveApp.prefix_command();
	app.set_version_flag("-v,--version", GetVersionString());
	app.usage("Usage: plugify <command> [options]");
	// flag to display full help at once
	app.set_help_flag();
	app.set_help_all_flag("-h, --help", "Print this help message and exit");

	// Global options
	bool jsonOutput = false;

	app.add_flag("-j,--json", jsonOutput, "Output in JSON format");

	// Add all commands (similar to main but simplified)
	auto* load = app.add_subcommand("load", "Load manager");
	auto* unload = app.add_subcommand("unload", "Unload manager");
	auto* reload = app.add_subcommand("reload", "Reload manager");
	auto* plugins = app.add_subcommand("plugins", "List plugins");
	auto* modules = app.add_subcommand("modules", "List modules");
	// Show plugin/module commands
	auto* plugin = app.add_subcommand("plugin", "Show plugin information");
	auto* module = app.add_subcommand("module", "Show module information");
	auto* health = app.add_subcommand("health", "System health");
	auto* tree = app.add_subcommand("tree", "Show dependency tree");
	auto* search = app.add_subcommand("search", "Search extensions");
	auto* validate = app.add_subcommand("validate", "Validate extension file");
	auto* compare = app.add_subcommand("compare", "Compare two extensions");

	// Enhanced list commands with filters and sorting
	std::string pluginFilterState;
	std::string pluginFilterLang;
	std::string pluginSortBy = "name";
	bool pluginReverse = false;
	bool pluginShowFailed = false;

	plugins->add_option(
	    "--filter-state",
	    pluginFilterState,
	    "Filter by state (comma-separated: loaded,failed,disabled)"
	);
	plugins->add_option(
	    "--filter-lang",
	    pluginFilterLang,
	    "Filter by language (comma-separated: cpp,python,rust)"
	);
	plugins
	    ->add_option("-s,--sort", pluginSortBy, "Sort by: name, version, state, language, loadtime")
	    ->check(CLI::IsMember({ "name", "version", "state", "language", "loadtime" }));
	plugins->add_flag("-r,--reverse", pluginReverse, "Reverse sort order");
	plugins->add_flag("-f,--failed", pluginShowFailed, "Show only failed plugins");

	std::string moduleFilterState;
	std::string moduleFilterLang;
	std::string moduleSortBy = "name";
	bool moduleReverse = false;
	bool moduleShowFailed = false;

	modules->add_option("--filter-state", moduleFilterState, "Filter by state (comma-separated)");
	modules->add_option("--filter-lang", moduleFilterLang, "Filter by language (comma-separated)");
	modules
	    ->add_option("-s,--sort", moduleSortBy, "Sort by: name, version, state, language, loadtime")
	    ->check(CLI::IsMember({ "name", "version", "state", "language", "loadtime" }));
	modules->add_flag("-r,--reverse", moduleReverse, "Reverse sort order");
	modules->add_flag("-f,--failed", moduleShowFailed, "Show only failed modules");

	// Add options for plugin/module
	std::string plugin_name;
	bool plugin_use_id = false;
	plugin->add_option("name", plugin_name, "Plugin name or ID")->required();
	plugin->add_flag("-u,--uuid", plugin_use_id, "Use ID instead of name");
	plugin->validate_positionals();

	std::string module_name;
	bool module_use_id = false;
	module->add_option("name", module_name, "Module name or ID")->required();
	module->add_flag("-u,--uuid", module_use_id, "Use ID instead of name");
	module->validate_positionals();

	std::string tree_name;
	bool tree_use_id = false;
	tree->add_option("name", tree_name, "Extension name or ID")->required();
	tree->add_flag("-u,--uuid", tree_use_id, "Use ID instead of name");
	tree->validate_positionals();

	std::string search_query;
	search->add_option("query", search_query, "Search query")->required();
	search->validate_positionals();

	std::string validate_path;
	validate->add_option("path", validate_path, "Path to extension file")->required();
	validate->validate_positionals();

	std::string compare_ext1, compare_ext2;
	bool compare_use_id = false;
	compare->add_option("extension1", compare_ext1, "First extension")->required();
	compare->add_option("extension2", compare_ext2, "Second extension")->required();
	compare->add_flag("-u,--uuid", compare_use_id, "Use ID instead of name");
	compare->validate_positionals();

	// Set callbacks
	load->callback([]() { LoadManager(); });
	unload->callback([]() { UnloadManager(); });
	reload->callback([]() { ReloadManager(); });

	plugins->callback([&pluginFilterState,  &pluginFilterLang, &pluginShowFailed, &pluginSortBy, &pluginReverse, &jsonOutput]() {
		FilterOptions filter;
		if (!pluginFilterState.empty()) {
			filter.states = ParseStates(ParseCsv(pluginFilterState));
		}
		if (!pluginFilterLang.empty()) {
			filter.languages = ParseCsv(pluginFilterLang);
		}
		filter.showOnlyFailed = pluginShowFailed;

		ListPlugins(filter, ParseSortBy(pluginSortBy), pluginReverse, jsonOutput);
	});

	modules->callback([&moduleFilterState, &moduleFilterLang, &moduleShowFailed, &moduleSortBy, &moduleReverse, &jsonOutput]() {
		FilterOptions filter;
		if (!moduleFilterState.empty()) {
			filter.states = ParseStates(ParseCsv(moduleFilterState));
		}
		if (!moduleFilterLang.empty()) {
			filter.languages = ParseCsv(moduleFilterLang);
		}
		filter.showOnlyFailed = moduleShowFailed;

		ListModules(filter, ParseSortBy(moduleSortBy), moduleReverse, jsonOutput);
	});

	plugin->callback([&plugin_name, &plugin_use_id, &jsonOutput]() {
		ShowPlugin(plugin_name, plugin_use_id, jsonOutput);
	});

	module->callback([&module_name, &module_use_id, &jsonOutput]() {
		ShowModule(module_name, module_use_id, jsonOutput);
	});

	health->callback([]() { ShowHealth(); });

	tree->callback([&tree_name, &tree_use_id]() { ShowDependencyTree(tree_name, tree_use_id); });

	search->callback([&search_query]() {
		if (!search_query.empty()) {
			SearchExtensions(search_query);
		} else {
			plg::print("Search query required");
		}
	});

	validate->callback([&validate_path]() { ValidateExtension(validate_path); });

	compare->callback([&compare_ext1, &compare_ext2, &compare_use_id]() {
		CompareExtensions(compare_ext1, compare_ext2, compare_use_id);
	});

	// Parse arguments
	try {
		app.parse(args.ArgC(), args.ArgV());
	} catch (const CLI::ParseError& e) {
		std::stringstream out;
		std::stringstream err;
		app.exit(e, out, err);
		if (auto output = out.str(); !output.empty()) {
			plg::print(std::move(output));
		}
		if (auto error = err.str(); !error.empty()) {
			plg::print(std::move(error));
		}
	}
}

// Alternative shorter command
static ConCommand plg_command("plg", plugify_callback, "Plugify control options", 0);
static ConCommand pkg_command("plug", plugify_callback, "Micromamba control options", 0);

CON_COMMAND_F(micromamba, "Micromamba control options", FCVAR_NONE) {
	if (!s_plugify || !s_plugify->IsInitialized()) {
		plg::print("{}: Initialize system before use.", Colorize("Error", Colors::RED));
		return;
	}

	if (s_plugify->GetManager().IsInitialized()) {
		plg::print(
			"{}: Package operations are only allowed when plugin manager is unloaded\n"
			"Please run 'plugify unload' first.",
			Colorize("Error", Colors::RED));
		return;
	}

	std::span arguments(args.ArgV(), static_cast<size_t>(args.ArgC()));
	if (arguments.size() < 2) {
		plg::print("Usage: {} <command> [options]", Colorize("micromamba", Colors::CYAN));
		return;
	}

	fs::path baseDir(Plat_GetGameDirectory());
	baseDir /= S2_GAME_NAME "/addons/plugify/";

	std::error_code ec;
	fs::path exePath(baseDir / "bin/" S2_BINARY "/" S2_EXECUTABLE_PREFIX "micromamba" S2_EXECUTABLE_SUFFIX);
	if (!fs::exists(exePath, ec)) {
		plg::print("{}: {} missing - {}", Colorize("Error", Colors::RED), Colorize("micromamba", Colors::CYAN), plg::as_string(exePath));
		return;
	}

	std::vector<std::string> cmd;
	cmd.reserve(arguments.size() * 2);
	cmd.push_back(plg::as_string(exePath));

	// Check for specific commands that might need special handling
	std::string_view command = arguments[1];

	// Block shell injection
	if (command == "shell") {
		plg::print(
		    "'{}' is running as a subprocess and can't modify the parent shell.",
		    Colorize("micromamba", Colors::CYAN)
		);
		return;
	}

	static std::string envName;
	if (command == "activate") {
		if (arguments.size() < 2) {
			plg::print("Usage: {} activate <command> [options]", Colorize("micromamba", Colors::CYAN));
			return;
		}
		envName = arguments[2];
		plg::print("You activate environment: {}", Colorize(envName, Colors::CYAN));
		return;
	}

	// Add all arguments
	for (int i = 1; i < arguments.size(); ++i) {
		cmd.emplace_back(arguments[i]);
	}

	bool has_yes = false;
	bool has_root = false;
	bool has_name = false;
	bool has_help = false;

	// Special handling ensure some flag for non-interactive
	for (int i = 2; i < arguments.size(); ++i) {
		std::string_view arg = arguments[i];
		if (arg == "-y" || arg == "--yes") {
			has_yes = true;
			continue;
		}
		if (arg == "-r" || arg == "--root-prefix") {
			has_root = true;
			continue;
		}
		if (arg == "-n" || arg == "--name") {
			has_name = true;
			continue;
		}
		if (arg == "-h" || arg == "--help") {
			has_help = true;
			continue;
		}
	}
	if (!has_yes) {
		cmd.emplace_back("-y");  // Add -y flag for non-interactive mode
	}
	if (!has_root) {
		cmd.emplace_back("-r");  // Add -r path
		cmd.push_back(plg::as_string(baseDir));
	}
	if (!has_name && !has_help) {
		if (command == "install" || command == "update" || command == "repoquery" || command == "remove"
		    || command == "uninstall" || command == "list" || command == "search") {
			cmd.emplace_back("-n");  // Add -n name
			cmd.push_back(envName);

			plg::print("Current environment: {}", Colorize(envName, Colors::CYAN));
		}
	}

	// Execute
	reproc::process process;
	reproc::options options;
	options.env.behavior = reproc::env::extend;

	ec = process.start(cmd, options);
	if (ec) {
		plg::print("{}: Failed to start {} - {}", Colorize("Error", Colors::RED), Colorize("micromamba", Colors::CYAN), ec.message());
		return;
	}

	std::string output, error;
	reproc::sink::string sink_out(output);
	reproc::sink::string sink_err(error);

	// Real-time output streaming
	ec = reproc::drain(process, sink_out, sink_err);
	if (ec) {
		plg::print("{}: Failed to read output - {}", Colorize("Error", Colors::RED), ec.message());
		return;
	}

	auto [status, wait_ec] = process.wait(reproc::infinite);
	if (wait_ec) {
		plg::print("{}: Process wait error - {}", Colorize("Error", Colors::RED), wait_ec.message());
		return;
	} else if (status != 0) {
		plg::print("{}: Process exited with code - {}", Colorize("Error", Colors::RED), status);
		return;
	}

	if (!output.empty()) {
		plg::print(std::move(output));
	}

	if (!error.empty()) {
		plg::print(std::move(error));
	}
}

// Alternative shorter command
static ConCommand mamba_command("mamba", micromamba_callback, "Micromamba control options", 0);
static ConCommand conda_command("conda", micromamba_callback, "Micromamba control options", 0);

using ServerGamePostSimulateFn = void (*)(IGameSystem*, const EventServerGamePostSimulate_t&);
DynLibUtils::CVTFHookAuto<&IGameSystem::ServerGamePostSimulate> _ServerGamePostSimulate;

void ServerGamePostSimulate(IGameSystem* pThis, const EventServerGamePostSimulate_t& msg) {
	_ServerGamePostSimulate.Call(pThis, msg);

	if (!s_plugify) {
		return;
	}

	s_plugify->Update();

	switch (s_state) {
		case PlugifyState::Load: {
			auto& manager = s_plugify->GetManager();
			if (auto initResult = manager.Initialize()) {
				plg::print("{}: Plugin manager was loaded.", Colorize("Success", Colors::GREEN));
			} else {
				plg::print("{}: {}", Colorize("Error", Colors::RED), initResult.error());
			}
			break;
		}
		case PlugifyState::Unload: {
			auto& manager = s_plugify->GetManager();
			manager.Terminate();
			plg::print("{}: Plugin manager was unloaded.", Colorize("Success", Colors::GREEN));
			break;
		}
		case PlugifyState::Reload: {
			auto& manager = s_plugify->GetManager();
			manager.Terminate();
			if (auto initResult = manager.Initialize()) {
				plg::print("{}: Plugin manager was reloaded.", Colorize("Success", Colors::GREEN));
			} else {
				plg::print("{}: {}", Colorize("Error", Colors::RED), initResult.error());
			}
			break;
		}
		case PlugifyState::Wait:
			return;
	}

	s_state = PlugifyState::Wait;
}

namespace {
	bool g_saveCrushDumps = true;
#if S2_PLATFORM_WINDOWS
	void SaveFullDump(PEXCEPTION_POINTERS pExceptionPointers) {
		fs::path fileName = FormatFileName("crash", "mdmp");

		HANDLE hFile = ::CreateFileW(
			fileName.native().c_str(),
			GENERIC_WRITE,
			FILE_SHARE_READ,
			NULL,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			NULL
		);
		if (hFile == INVALID_HANDLE_VALUE) {
			return;
		}

		MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
		exceptionInfo.ThreadId = GetCurrentThreadId();
		exceptionInfo.ExceptionPointers = pExceptionPointers;
		exceptionInfo.ClientPointers = FALSE;

		::MiniDumpWriteDump(
			::GetCurrentProcess(),
			::GetCurrentProcessId(),
			hFile,
			MINIDUMP_TYPE(
				MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithFullMemory
				| MiniDumpWithProcessThreadData | MiniDumpWithPrivateReadWriteMemory
			),
			pExceptionPointers ? &exceptionInfo : NULL,
			NULL,
			NULL
		);

		::CloseHandle(hFile);
	}

	struct MiniDumpHandlerData_t {
		int32_t nFlags;
		int32_t nExitCode;
		PEXCEPTION_POINTERS pExceptionPointers;
		// ... more
	};

	void CrashpadGenericMiniDumpHandler(MiniDumpHandlerData_t* data) {
		if (g_saveCrushDumps) {
			SaveFullDump(data->pExceptionPointers);
		} else {
			CrashpadClient::DumpAndCrash(data->pExceptionPointers);
		}
	}
#endif
}

static constexpr auto RWX_PERMS = fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec
                                  | fs::perms::others_read | fs::perms::others_exec;

class CrashpadInitializer {
	struct Metadata {
		std::string url;
		std::string handlerApp;
		std::string databaseDir;
		std::string metricsDir;
		std::string logsDir;
		std::map<std::string, std::string> annotations;
		std::vector<std::string> arguments;
		std::vector<std::string> attachments;
		std::optional<bool> restartable;
		std::optional<bool> asynchronous_start;
		std::optional<bool> listen_console;
		std::optional<bool> enabled;
	};

	static Result<fs::path> ValidateHandler(const fs::path& exeDir, std::string_view handlerName) {
		fs::path handlerPath = exeDir
		                       / std::format(S2_EXECUTABLE_PREFIX "{}" S2_EXECUTABLE_SUFFIX, handlerName);

		std::error_code ec;
		if (!fs::exists(handlerPath, ec)) {
			return MakeError("Crashpad handler not found: {}", plg::as_string(handlerPath));
		}

		fs::permissions(handlerPath, RWX_PERMS, fs::perm_options::replace, ec);
		if (ec) {
			return MakeError("Failed to set {} handler permissions: {}", handlerName, ec.message());
		}

		return handlerPath;
	}

	static Result<fs::path> EnsureDirectory(const fs::path& dirPath, std::string_view description) {
		std::error_code ec;

		if (!fs::exists(dirPath, ec)) {
			fs::create_directories(dirPath, ec);
			if (ec) {
				return MakeError(
				    "Failed to create {} directory '{}': {}",
				    description,
				    plg::as_string(dirPath),
				    ec.message()
				);
			}
		}

		fs::permissions(dirPath, RWX_PERMS, fs::perm_options::replace, ec);
		if (ec) {
			return MakeError("Failed to set {} directory permissions: {}", description, ec.message());
		}

		return dirPath;
	}

	static Result<std::unique_ptr<FileLoggingListener>> SetupConsoleLogging(
	    const fs::path& exeDir,
	    const fs::path& logsDir,
	    std::vector<base::FilePath>& attachments
	) {
		auto logFile = exeDir / logsDir / FormatFileName("session", "log");

		auto listener = FileLoggingListener::Create(logFile);
		if (!listener) {
			return MakeError("Failed to create console logger: {}", listener.error());
		}

		// Add console log as attachment with special prefix
		fs::path attachmentPath = "console.log=";
		attachmentPath += logFile.make_preferred();
		attachments.emplace_back(attachmentPath);

		return std::move(*listener);
	}

public:
	static Result<std::unique_ptr<CrashpadClient>>
	Initialize(const fs::path& exeDir, const fs::path& annotationsPath) {
		// Load metadata
		auto metadataResult = ReadJson<Metadata>(exeDir / annotationsPath);
		if (!metadataResult) {
			return MakeError("Failed to load metadata: {}", metadataResult.error());
		}

		const auto& metadata = *metadataResult;

		// Check if crashpad is enabled
		if (!metadata.enabled.value_or(false)) {
			return nullptr;
		}

		// Validate handler executable
		auto handlerResult = ValidateHandler(exeDir, metadata.handlerApp);
		if (!handlerResult) {
			return MakeError(std::move(handlerResult.error()));
		}

		// Setup directories
		auto databaseResult = EnsureDirectory(exeDir / metadata.databaseDir, "database");
		if (!databaseResult) {
			return MakeError(std::move(databaseResult.error()));
		}

		auto metricsResult = EnsureDirectory(exeDir / metadata.metricsDir, "metrics");
		if (!metricsResult) {
			return MakeError(std::move(metricsResult.error()));
		}

		// Initialize database
		base::FilePath databaseDir(*databaseResult);
		auto database = CrashReportDatabase::Initialize(databaseDir);
		if (!database) {
			return MakeError("Failed to initialize crash database");
		}

		// Configure upload settings
		database->GetSettings()->SetUploadsEnabled(!metadata.url.empty());

		// Prepare attachments
		std::vector<base::FilePath> attachments;
		attachments.reserve(metadata.attachments.size());

		for (const auto& attachment : metadata.attachments) {
			attachments.emplace_back(exeDir / attachment);
		}

		// Setup console logging if requested
		if (metadata.listen_console.value_or(false)) {
			auto listenerResult = SetupConsoleLogging(exeDir, metadata.logsDir, attachments);
			if (!listenerResult) {
				return MakeError(std::move(listenerResult.error()));
			} else {
				s_listener = std::move(*listenerResult);
			}
		}

		// Start crash handler
		auto client = std::make_unique<CrashpadClient>();
		bool started = client->StartHandler(
		    base::FilePath(*handlerResult),
		    databaseDir,
		    base::FilePath(*metricsResult),
		    metadata.url,
		    metadata.annotations,
		    metadata.arguments,
		    metadata.restartable.value_or(false),
		    metadata.asynchronous_start.value_or(false),
		    attachments
		);

		if (!started) {
			return MakeError("Failed to start Crashpad handler");
		}

		g_saveCrushDumps = false;  // Disable alternative crash handling

		return client;
	}
};

class PlugifyInitializer {
private:
	static constexpr std::string_view REQUIRED_INTERFACE = CVAR_INTERFACE_VERSION;
	static constexpr std::string_view HOOK_MODULE = "server";
	static constexpr std::string_view HOOK_CLASS = "CLightQueryGameSystem";

	static Result<ICvar*> FindCVarInterface(CAppSystemDict* systems) {
		for (const auto& system : systems->m_Systems) {
			if (system.m_pInterfaceName == REQUIRED_INTERFACE) {
				if (auto* pCvar = static_cast<ICvar*>(system.m_pSystem)) {
					plg::print("{}: Found CVar interface", Colorize("Info", Colors::BLUE));
					return pCvar;
				}
			}
		}
		return MakeError("CVar interface {} not found", REQUIRED_INTERFACE);
	}

	static Result<void> InstallServerHooks() {
		DynLibUtils::CModule server(HOOK_MODULE);
		if (!server) {
			return MakeError("Failed to load {} module", HOOK_MODULE);
		}

		auto table = server.GetVirtualTableByName(HOOK_CLASS);
		if (!table) {
			return MakeError("Virtual table {} not found", HOOK_CLASS);
		}

		DynLibUtils::CVirtualTable vtable(table);
		_ServerGamePostSimulate.Hook(vtable, &ServerGamePostSimulate);

		plg::print("{}: Server hooks installed", Colorize("Info", Colors::GREEN));
		return {};
	}

#if S2_PLATFORM_WINDOWS
	static Result<void> SetupCrashHandler() {
		using MiniDumpHandler = void (*)(MiniDumpHandlerData_t*);
		using SetMiniDumpHandlerFn = void (*)(MiniDumpHandler, bool);

		DynLibUtils::CModule tier0("tier0");
		if (!tier0) {
			return MakeError("Failed to load tier0 module");
		}

		auto SetMiniDumpHandler = tier0.GetFunctionByName("SetDefaultMiniDumpHandler")
		                              .RCast<SetMiniDumpHandlerFn>();

		if (!SetMiniDumpHandler) {
			return MakeError("SetDefaultMiniDumpHandler function not found");
		}

		SetMiniDumpHandler(&CrashpadGenericMiniDumpHandler, true);

		plg::print("{}: Crash handler registered", Colorize("Info", Colors::GREEN));
		return {};
	}
#endif

	static Result<fs::path> ValidateMicromamba(const fs::path& baseDir) {
		fs::path exePath = baseDir / "bin" / S2_BINARY
		                   / (S2_EXECUTABLE_PREFIX "micromamba" S2_EXECUTABLE_SUFFIX);

		std::error_code ec;
		if (!fs::exists(exePath, ec)) {
			return MakeError("Micromamba executable not found at: {}", exePath.string());
		}

		fs::permissions(exePath, RWX_PERMS, fs::perm_options::replace, ec);
		if (ec) {
			plg::print(
			    "{}: Failed to set micromamba permissions: {}",
			    Colorize("Warning", Colors::YELLOW),
			    ec.message()
			);
		}

		return exePath;
	}

	static Config::Paths BuildPaths(const fs::path& baseDir) {
		return {
			.baseDir = baseDir,
			.extensionsDir = "envs",
			.configsDir = "configs",
			.dataDir = "data",
			.logsDir = "logs",
			.cacheDir = "pkgs",
		};
	}

	static Result<std::shared_ptr<Plugify>> CreatePlugifyContext(const fs::path& baseDir) {
		// Build paths
		auto paths = BuildPaths(baseDir);

		// Create context
		auto buildResult = Plugify::CreateBuilder()
			.WithLogger(s_logger)
			.WithPaths(std::move(paths))
			.Build();

		if (!buildResult) {
			return MakeError("Failed to create Plugify context: {}", buildResult.error());
		}

		// Initialize context
		auto context = std::move(*buildResult);
		if (auto result = context->Initialize(); !result) {
			return MakeError("Failed to initialize context: {}", result.error());
		}

		// Initialize manager
		auto& manager = context->GetManager();
		if (auto result = manager.Initialize(); !result) {
			return MakeError("Failed to initialize plugin manager: {}", result.error());
		}

		plg::print("{}: Plugify initialized successfully", Colorize("Success", Colors::GREEN));

		return context;
	}

public:
	static Result<std::shared_ptr<Plugify>> Initialize(CAppSystemDict* systems) {
		// Setup logging if listener exists
		if (s_listener) {
			LoggingSystem_PushLoggingState(false, false);
			LoggingSystem_RegisterLoggingListener(s_listener.get());
		}

		// Notify about crashpad
		plg::print(
		    "{}: Crashpad {} in configuration",
		    Colorize("Info", Colors::BLUE),
		    Colorize(s_crashpad ? "enabled" : "disabled", Colors::MAGENTA)
		);

		// Find CVar interface
		if (auto cvarResult = FindCVarInterface(systems); !cvarResult) {
			plg::print("{}: {}", Colorize("Warning", Colors::YELLOW), cvarResult.error());
			// Non-fatal: continue without CVar
		} else {
			g_pCVar = *cvarResult;
		}

		// Install server hooks
		if (auto hookResult = InstallServerHooks(); !hookResult) {
			plg::print("{}: {}", Colorize("Warning", Colors::YELLOW), hookResult.error());
			// Non-fatal: continue without hooks
		}

#if S2_PLATFORM_WINDOWS
		// Setup crash handler
		if (auto crashResult = SetupCrashHandler(); !crashResult) {
			plg::print("{}: {}", Colorize("Warning", Colors::YELLOW), crashResult.error());
			// Non-fatal: continue without crash handler
		}
#endif

		// Register ConVars
		ConVar_Register(FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);

		// Build base directory path
		fs::path baseDir(Plat_GetGameDirectory());
		baseDir /= S2_GAME_NAME "/addons/plugify/";

		// Validate micromamba
		if (auto mambaResult = ValidateMicromamba(baseDir); !mambaResult) {
			return MakeError(std::move(mambaResult.error()));
		}

		// Create and initialize Plugify context
		return CreatePlugifyContext(baseDir);
	}
};

using OnAppSystemLoadedFn = void (*)(CAppSystemDict*);
DynLibUtils::CVTFHookAuto<&CAppSystemDict::OnAppSystemLoaded> _OnAppSystemLoaded;
std::unordered_set<std::string> g_loadList;

void OnAppSystemLoaded(CAppSystemDict* pThis) {
	_OnAppSystemLoaded.Call(pThis);

	if (s_plugify) {
		return;
	}

	if (g_loadList.empty()) {
		g_loadList.reserve(static_cast<size_t>(pThis->m_Modules.Count()));
	}

	constexpr std::string_view moduleName = S2_GAME_START;

	for (const auto& module : pThis->m_Modules) {
		if (module.m_pModuleName) {
			if (g_loadList.insert(module.m_pModuleName).second) {
				if (module.m_pModuleName == moduleName) {
					auto result = PlugifyInitializer::Initialize(pThis);
					if (!result) {
						plg::print("{}: Plugify initialization failed: {}",
							Colorize("Error", Colors::RED), result.error());

						// Optionally: decide if this should be fatal
						// throw std::runtime_error(result.error());
						return;
					}

					s_plugify = std::move(*result);
					break;
				}
			}
		}
	}
}

std::optional<fs::path> ExecutablePath() {
	std::string execPath(MAX_PATH, '\0');

	while (true) {
#if S2_PLATFORM_WINDOWS
		size_t len = ::GetModuleFileNameA(nullptr, execPath.data(), static_cast<DWORD>(execPath.length()));
		if (len == 0)
#elif S2_PLATFORM_LINUX
		ssize_t len = ::readlink("/proc/self/exe", execPath.data(), execPath.length());
		if (len == -1)
#endif
		{
			return std::nullopt;
		}

		if (len < execPath.length()) {
			break;
		}

		execPath.resize(execPath.length() * 2);
	}

	return fs::path(std::move(execPath)).parent_path();
}

int main(int argc, char* argv[]) {
	auto binary_path = ExecutablePath().value_or(fs::current_path());

	std::error_code ec;
	if (fs::is_directory(binary_path, ec) && binary_path.filename() == "game") {
		binary_path /= "bin/" S2_BINARY;
	}

	if (!std::is_debugger_present()) {
		auto result = CrashpadInitializer::Initialize(binary_path, "crashpad.jsonc");
		if (!result) {
			std::println(std::cerr, "Crashpad error: {}", result.error());
			return 1;
		}

		s_crashpad = std::move(*result);
	}

	auto engine_path = binary_path / S2_LIBRARY_PREFIX "engine2" S2_LIBRARY_SUFFIX;
	auto parent_path = binary_path.generic_string();

#if S2_PLATFORM_WINDOWS
	int flags = LOAD_WITH_ALTERED_SEARCH_PATH;
#else
	int flags = RTLD_NOW | RTLD_GLOBAL;
#endif

	DynLibUtils::CModule engine{};
	engine.LoadFromPath(plg::as_string(engine_path), flags);
	if (!engine) {
		std::println(std::cerr, "Launcher error: {} - {}", engine.GetLastError(), plg::as_string(engine_path));
		return 1;
	}

	s_logger = std::make_shared<ConsoleLoggger>("plugify");
	s_logger->SetLogLevel(Severity::Info);

	auto table = engine.GetVirtualTableByName("CMaterialSystem2AppSystemDict");
	DynLibUtils::CVirtualTable vtable(table);
	_OnAppSystemLoaded.Hook(vtable, &OnAppSystemLoaded);

	using Source2MainFn = int (*)(
	    void* hInstance,
	    void* hPrevInstance,
	    const char* pszCmdLine,
	    int nShowCmd,
	    const char* pszBaseDir,
	    const char* pszGame
	);
	auto Source2Main = engine.GetFunctionByName("Source2Main").RCast<Source2MainFn>();

	auto command_line = argc > 1 ? plg::join(std::span(argv + 1, argc - 1), " ") : "";
	int res = Source2Main(nullptr, nullptr, command_line.c_str(), 0, parent_path.c_str(), S2_GAME_NAME);

	if (s_listener) {
		LoggingSystem_PopLoggingState();
	}

	//_ServerGamePostSimulate.Unhook();
	//_OnAppSystemLoaded.Unhook();

	s_plugify.reset();
	s_listener.reset();
	s_logger.reset();
	s_crashpad.reset();

	return res;
}
