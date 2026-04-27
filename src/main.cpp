#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <print>
#include <queue>
#include <semaphore>
#include <thread>
#include <unordered_set>

#include <inetworksystem.h>

#if S2_PLATFORM_WINDOWS
#include <windows.h>
#include <dbghelp.h>
#undef FormatMessage
#else
#include <dlfcn.h>
#include <cstdlib>
#endif

#include <dynlibutils/module.hpp>
#include <dynlibutils/virtual.hpp>
#include <dynlibutils/vthook.hpp>
#include <CLI/CLI.hpp>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <glaze/toml.hpp>
#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>
#include <daking/MPSC_queue.hpp>

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
#include <tier0/minidump.h>
#include <engine/hoststate.h>
#include <appframework/iappsystem.h>

using namespace plugify;
using namespace std::string_view_literals;
namespace fs = std::filesystem;

#if __has_include(<glaze/yaml.hpp>)
using Value = glz::generic;
#else
using Value = glz::json_t;
#endif

#include <sentry.h>

#if S2_PLATFORM_WINDOWS
#define sentry_pcall(fn, options, path) fn##w_n(options, path.c_str(), path.size())
#define sentry_pcall2(fn, path) fn##w_n(path.c_str(), path.size())
#else
#define sentry_pcall(fn, options, path) fn##_n(options, path.c_str(), path.size())
#define sentry_pcall2(fn, path) fn##_n(path.c_str(), path.size())
#endif

auto sentry_value_new_string_v = [](std::string_view value) {
	return sentry_value_new_string_n(value.data(), value.size());
};

auto sentry_set_tag_v = [](std::string_view key, std::string_view value) {
	sentry_set_tag_n(key.data(), key.size(), value.data(), value.size());
};

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

bool s_sentry, s_breadcrumbs; size_t s_breadcrumbs_max = 100;


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
		_channel = LoggingSystem_RegisterLoggingChannel(name, nullptr, flags, verbosity, color);
		_worker_thread = std::jthread([this](std::stop_token stoken) {
			ProcessQueue(stoken);
		});
	}

	~ConsoleLoggger() override {
		_worker_thread.request_stop();
		_semaphore.release();
	}

	void Log(std::string_view message, Color color, bool newLine) {
		if (message.empty())
			return;

		assert(message.size() < 2048);

		LoggingSystem_LogDirect(_channel, LS_MESSAGE, color, message.data());
		if (newLine && message.back() != '\n') {
			LoggingSystem_LogDirect(_channel, LS_MESSAGE, color, "\n");
		}
	}

	// ReSharper disable once CppPassValueParameterByConstReference
	void Log(std::string message, bool newLine) {
		if (message.empty())
			return;

		auto tokens = AnsiColorParser::Tokenize(message);

		for (const auto& [text, color] : tokens) {
			for (auto segments = Tokenize(text); const auto& segment : segments) {
				LoggingSystem_LogDirect(_channel, LS_MESSAGE, color, segment.data());
			}
		}
		if (newLine && message.back() != '\n') {
			LoggingSystem_LogDirect(_channel, LS_MESSAGE, "\n");
		}
	}

	void Log(std::string_view message, Severity severity, const Location& location = Location::current()) override {
		if (message.empty())
			return;

		_queue.enqueue(LogEntry{
			.message = std::string(message),
			.severity = severity,
			.timestamp = GetTimestamp(),
			.location = location
		});
		_semaphore.release();
	}

	static double GetTimestamp() {
		auto now = std::chrono::system_clock::now();
		return std::chrono::duration<double>(now.time_since_epoch()).count();
	}

	void SetLogLevel(Severity minSeverity) override {
		_min_severity = minSeverity;
	}

	Severity GetLogLevel() override {
		return _min_severity;
	}

	void Flush() override {
	}

	template<typename F>
	void Foreach(const F& func) {
		for (const auto& [message, severity, timestamp, location] : _breadcrumbs) {
			std::string_view type = "default";
			std::string_view category = severity == Severity::Trace ? "trace" : "log";
			std::string_view level = severity == Severity::Trace ? "info" : SeverityToLevel(severity);
			func(type, category, level, message, timestamp, location);
		}
	}

protected:
	struct alignas(std::hardware_constructive_interference_size) LogEntry {
		std::string message;
		Severity severity;
		double timestamp;
		Location location;
	};

	struct LogParams {
		LoggingSeverity_t severity;
		Color color;
	};

	static inline LogParams kLogParams[] = {
		/* Unknown */ { LS_MESSAGE, S2Colors::WHITE   },
		/* Trace   */ { LS_MESSAGE, S2Colors::WHITE   },
		/* Debug   */ { LS_MESSAGE, S2Colors::GREEN   },
		/* Info    */ { LS_MESSAGE, S2Colors::YELLOW  },
		/* Warning */ { LS_WARNING, S2Colors::ORANGE  },
		/* Error   */ { LS_WARNING, S2Colors::RED     },
		/* Fatal   */ { LS_ERROR,   S2Colors::MAGENTA },
	};

	void Write(LogEntry& entry) {
		const auto& [message, severity, timestamp, location] = entry;

		[[maybe_unused]] auto guard = plg::make_scope_guard([&] {
			if (s_sentry && s_breadcrumbs && severity != Severity::Unknown) {
				if (_breadcrumbs.size() > s_breadcrumbs_max) _breadcrumbs.pop_front();
				_breadcrumbs.push_back(std::move(entry));
			}
		});

		if (severity == Severity::Unknown) {
			WriteMessage(message, severity);
			return;
		} else if (severity < _min_severity) {
			return;
		}

		auto output = FormatMessage(message, severity, location);
		WriteMessage(output, severity);
	}

	void WriteMessage(std::string_view message, Severity severity) const {
		const auto& [sev, color] = kLogParams[static_cast<size_t>(severity)];

		for (auto segments = Tokenize(message); const auto& seg : segments) {
			LoggingSystem_LogDirect(
				_channel,
				sev,
				color,
				seg.data()
			);
		}
		if (message.back() != '\n') {
			LoggingSystem_LogDirect(_channel, sev, "\n");
		}
	}

	void ProcessQueue(std::stop_token stoken) {
		LogEntry entry;
		while (!stoken.stop_requested()) {
			_semaphore.acquire(); // sleeps until a Log() wakes it
			if (_queue.try_dequeue(entry)) // spurious wake from destructor: no-op
				Write(entry);
		}
		while (_queue.try_dequeue(entry))
			Write(entry);
	}

private:
	static std::string
	FormatMessage(std::string_view message, Severity severity, const Location& location) {
		using namespace std::chrono;

		auto now = system_clock::now();
		auto seconds = floor<std::chrono::seconds>(now);
		auto ms = duration_cast<milliseconds>(now - seconds);

		return std::format(
			"[{:%F %T}.{:03d}] [{}] [{}:({}:{}): {}] {}",
			seconds,
			static_cast<int>(ms.count()),
			plg::enum_to_string(severity),
			location.module_name().empty()
			? location.file_name()
			: std::format("{} => {}", location.module_name(), location.file_name()),
			location.line(),
			location.column(),
			location.function_name(),
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

	static std::string_view SeverityToLevel(Severity severity) {
		switch (severity) {
			case Severity::Trace:   return "trace";
			case Severity::Debug:   return "debug";
			case Severity::Info:    return "info";
			case Severity::Warning: return "warning";
			case Severity::Error:   return "error";
			case Severity::Fatal:   return "fatal";
			default:                return "info";
		}
	}

private:
	std::atomic<Severity> _min_severity{ Severity::Unknown };
	LoggingChannelID_t _channel{ INVALID_LOGGING_CHANNEL_ID };
	std::counting_semaphore<> _semaphore{ 0 };
	std::jthread _worker_thread;
	daking::MPSC_queue<LogEntry> _queue;
	std::deque<LogEntry> _breadcrumbs;
};

class FileLoggingListener final : public ILoggingListener {
public:
	static constexpr size_t kMaxBytes = 10 * 1024 * 1024;

    static Result<std::unique_ptr<FileLoggingListener>>
    Create(fs::path base, size_t max_bytes = kMaxBytes, bool auto_flush = true) {
    	std::error_code ec;
    	fs::create_directories(base.parent_path(), ec);

    	auto path = TimestampedPath(base);
		errno = 0;
        std::ofstream file(path, std::ios::app);
        if (!file) {
            return MakeError(
                "Failed to open log file: {} - {}",
                plg::as_string(path),
                std::strerror(errno)
            );
        }

        return std::make_unique<FileLoggingListener>(std::move(file), std::move(path), std::move(base), max_bytes, auto_flush);
    }

    explicit FileLoggingListener(std::ofstream&& file, fs::path&& path, fs::path&& base, size_t max_bytes, bool auto_flush)
        : _file(std::move(file))
        , _path(std::move(path))
        , _base(std::move(base))
        , _max_bytes(max_bytes)
        , _auto_flush(auto_flush) {

        _worker_thread = std::jthread([this](std::stop_token stoken) {
            ProcessQueue(stoken);
        });
    }

    ~FileLoggingListener() {
        _worker_thread.request_stop();
        _semaphore.release();
    }

    FileLoggingListener(const FileLoggingListener&) = delete;
    FileLoggingListener& operator=(const FileLoggingListener&) = delete;
    FileLoggingListener(FileLoggingListener&&) = delete;
    FileLoggingListener& operator=(FileLoggingListener&&) = delete;

    void Log(const LoggingContext_t* pContext, const tchar* pMessage) override {
        if (!pContext || (pContext->m_Flags & LCF_CONSOLE_ONLY) != 0 || pMessage == nullptr || pMessage[0] == '\0')
            return;

        _queue.enqueue(pMessage);
        _semaphore.release();
    }

protected:
    void Write(std::string_view message) {
    	if (ShouldRotate())
    		RotateLog();

    	WriteMessage(message);
    }

	bool ShouldRotate() {
    	auto pos = _file.tellp();
    	return pos >= static_cast<std::streamoff>(_max_bytes);
    }

	void RotateLog() {
    	_file.close();
    	_path = TimestampedPath(_base);
    	_file.open(_path, std::ios::trunc);

    	if (!_file)
    		// todo: log error
    		return;
    }

	void WriteMessage(std::string_view message) {
    	if (!_file)
    		return;

    	using namespace std::chrono;
    	auto now = system_clock::now();
    	auto seconds = floor<std::chrono::seconds>(now);
    	auto ms = duration_cast<milliseconds>(now - seconds);

    	std::print(_file, "[{:%F %T}.{:03d}] {}", seconds, static_cast<int>(ms.count()), message);

    	if (_auto_flush)
    		_file.flush();
    }

    void ProcessQueue(std::stop_token stoken) {
        std::string entry;
        while (!stoken.stop_requested()) {
            _semaphore.acquire();
            if (_queue.try_dequeue(entry))
                Write(entry);
        }
        while (_queue.try_dequeue(entry))
            Write(entry);
    }

	// Helper: turn /logs/session.log → /logs/session-20260418_143022.log
	static fs::path TimestampedPath(const fs::path& base) {
    	using namespace std::chrono;
    	auto now = system_clock::now();
    	auto seconds = floor<std::chrono::seconds>(now);

    	return base.parent_path()
		   / std::format(
			   "{}-{:%Y%m%d_%H%M%S}{}",
			   plg::as_string(base.stem()),
			   utc_clock::from_sys(seconds),
			   plg::as_string(base.extension())
		   );
    }

private:
    std::ofstream _file;
    fs::path _path;
    fs::path _base;
	const size_t _max_bytes{};
    const bool _auto_flush{};
    std::counting_semaphore<> _semaphore{ 0 };
    std::jthread _worker_thread;
    daking::MPSC_queue<std::string> _queue;
};

enum class PlugifyState { Wait, Load, Unload, Reload, Quit };

std::shared_ptr<Plugify> s_plugify;
std::shared_ptr<ConsoleLoggger> s_logger;
std::unique_ptr<FileLoggingListener> s_listener;
PlugifyState s_state;

sentry_value_t SentryNativeOnCrash([[maybe_unused]] const sentry_ucontext_t* uctx, sentry_value_t event, [[maybe_unused]] void* userdata) {
	if (s_logger) s_logger->Foreach([](std::string_view type, std::string_view category, std::string_view level, std::string_view message, double timestamp, const Location& location) {
		sentry_value_t crumb = sentry_value_new_object();
		sentry_value_set_by_key(crumb, "type", sentry_value_new_string_v(type));
		sentry_value_set_by_key(crumb, "message", sentry_value_new_string_v(message));
		sentry_value_set_by_key(crumb, "category", sentry_value_new_string_v(category));
		sentry_value_set_by_key(crumb, "timestamp", sentry_value_new_double(timestamp));
		sentry_value_set_by_key(crumb, "level", sentry_value_new_string_v(level));
		sentry_value_set_by_key(crumb, "origin", sentry_value_new_string_v(location.module_name()));
		sentry_value_t data = sentry_value_new_object();
		sentry_value_set_by_key(data, "line", sentry_value_new_uint64(location.line()));
		sentry_value_set_by_key(data, "column", sentry_value_new_uint64(location.column()));
		sentry_value_set_by_key(data, "file_name", sentry_value_new_string_v(location.file_name()));
		sentry_value_set_by_key(data, "function_name", sentry_value_new_string_v(location.function_name()));
		sentry_value_set_by_key(data, "module_name", sentry_value_new_string_v(location.module_name()));
		sentry_value_set_by_key(crumb, "data", data);
		sentry_add_breadcrumb(crumb);
	});

	CMiniDumpComment comment(131072);
	LoggingSystem_GetLogCapture(&comment, true);
	sentry_value_t message = sentry_value_new_string(comment.GetStartPointer());
	sentry_value_set_by_key(event, "message", message);

	return event;
}

#define BASE_PATH PLUGIFY_PATH_LITERAL("" S2_GAME_NAME "/" "addons" "/" "plugify" "/")
#define MAMBA_PATH BASE_PATH PLUGIFY_PATH_LITERAL("bin" "/" S2_BINARY "/" S2_EXECUTABLE_PREFIX "micromamba" S2_EXECUTABLE_SUFFIX)

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
	Result<T> ReadFile(const fs::path& path) {
		errno = 0;
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return MakeError(
				"Failed to read file: {} - {}",
				plg::as_string(path),
				std::strerror(errno)
			);
		}

		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

		T value;
		auto ext = path.extension().string();

		glz::error_ctx error;

		if (ext == ".yml" || ext == ".yaml") {
			error = glz::read_yaml(value, text);
		} else if (ext == ".jsonc") {
			error = glz::read_jsonc(value, text);
		} else if (ext == ".json") {
			error = glz::read_json(value, text);
		}

		if (!error) {
			return MakeError(glz::format_error(error, text));
		}

		return value;
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

		if (filter.states) {
			auto state = ext->GetState();
			if (std::find(filter.states->begin(), filter.states->end(), state)
			    == filter.states->end()) {
				return false;
			}
		}

		if (filter.languages) {
			const auto& lang = ext->GetLanguage();
			if (std::find(filter.languages->begin(), filter.languages->end(), lang)
			    == filter.languages->end()) {
				return false;
			}
		}

		if (filter.searchQuery) {
			std::string query = *filter.searchQuery;
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
	Value ExtensionToJson(const Extension* ext) {
		Value j;
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
			Value::array_t dependencies;
			for (const auto& dep : ext->GetDependencies()) {
				Value::object_t depJson;
				depJson["name"] = dep.GetName();
				depJson["constraints"] = dep.GetConstraints().to_string();
				depJson["optional"] = dep.IsOptional();
				dependencies.emplace_back(std::move(depJson));
			}
			j["dependencies"] = Value::array_t{ std::move(dependencies) };
		}

		// Performance
		j["performance"]["total_time_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
		                                        ext->GetTotalTime()
		)
		                                        .count();

		// Errors and warnings
		if (ext->HasErrors()) {
			Value::array_t errors;
			errors.reserve(ext->GetErrors().size());
			for (const auto& error : ext->GetErrors()) {
				errors.emplace_back(error);
			}
			j["errors"] = Value::array_t{ std::move(errors) };
		}
		if (ext->HasWarnings()) {
			Value::array_t warnings;
			warnings.reserve(ext->GetWarnings().size());
			for (const auto& warning : ext->GetWarnings()) {
				warnings.emplace_back(warning);
			}
			j["warnings"] = Value::array_t{ std::move(warnings) };
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
					    result = a->GetOperationTime(ExtensionState::Loading)
					             < b->GetOperationTime(ExtensionState::Loading);
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
		std::map<std::string, size_t> statistics;
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
			auto loadTime = ext->GetOperationTime(ExtensionState::Loading);
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
	SortBy ParseSortBy(std::string str) {
		std::transform(str.begin(), str.end(), str.begin(), ::tolower);

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

	// Helper to parse extension state
	ExtensionState ParseExtensionState(std::string str) {
		std::transform(str.begin(), str.end(), str.begin(), ::tolower);

		if (str == "loaded") {
			return ExtensionState::Loaded;
		}
		if (str == "started") {
			return ExtensionState::Started;
		}
		if (str == "failed") {
			return ExtensionState::Failed;
		}
		if (str == "disabled") {
			return ExtensionState::Disabled;
		}
		if (str == "corrupted") {
			return ExtensionState::Corrupted;
		}
		if (str == "unresolved") {
			return ExtensionState::Unresolved;
		}

		return ExtensionState::Unknown;
	};

	// Helper to parse state filter
	std::vector<ExtensionState> ParseStates(const std::vector<std::string>& strs) {
		std::vector<ExtensionState> states;
		states.reserve(strs.size());
		for (const auto& str : strs) {
			auto state = ParseExtensionState(str);
			if (state != ExtensionState::Unknown) {
				states.push_back(state);
			}
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
			Value::array_t objects;
			objects.reserve(filtered.size());
			for (const auto& plugin : filtered) {
				objects.emplace_back(ExtensionToJson(plugin));
			}
			plg::print(*Value{ std::move(objects) }.dump());
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
				auto duration = plugin->GetOperationTime(ExtensionState::Loading);
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
		if (filter.states || filter.languages
		    || filter.searchQuery || filter.showOnlyFailed) {
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
			Value::array_t objects;
			objects.reserve(filtered.size());
			for (const auto& module : filtered) {
				objects.emplace_back(ExtensionToJson(module));
			}
			plg::print(*Value{ std::move(objects) }.dump());
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
				auto duration = module->GetOperationTime(ExtensionState::Loading);
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
		if (filter.states || filter.languages
		    || filter.searchQuery || filter.showOnlyFailed) {
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
				Value error;
				error["error"] = std::format("Plugin {} not found", identifier);
				plg::print(*error.dump());
			} else {
				plg::print("{}: Plugin {} not found.", Colorize("Error", Colors::RED), identifier);
			}
			return;
		}

		if (!plugin->IsPlugin()) {
			if (jsonOutput) {
				Value error;
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
				Value error;
				error["error"] = std::format("Module {} not found", identifier);
				plg::print(*error.dump());
			} else {
				plg::print("{}: Module {} not found.", Colorize("Error", Colors::RED), identifier);
			}
			return;
		}

		if (!module->IsModule()) {
			if (jsonOutput) {
				Value error;
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
		    FormatDuration(ext1->GetOperationTime(ExtensionState::Loading)),
		    FormatDuration(ext2->GetOperationTime(ExtensionState::Loading))
		);
		plg::print(
		    "  Total Time:    {:<15} vs {:<15}",
		    FormatDuration(ext1->GetTotalTime()),
		    FormatDuration(ext2->GetTotalTime())
		);

		plg::print(DOUBLE_LINE);
	}

	// Helper to parse severity option
	Severity ParseSeverity(std::string str) {
		std::transform(str.begin(), str.end(), str.begin(), ::tolower);

		if (str == "trace") {
			return Severity::Trace;
		}
		if (str == "debug") {
			return Severity::Debug;
		}
		if (str == "info") {
			return Severity::Info;
		}
		if (str == "warning") {
			return Severity::Warning;
		}
		if (str == "error") {
			return Severity::Error;
		}
		if (str == "fatal") {
			return Severity::Fatal;
		}

		return Severity::Info;
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
	if (s_plugify && s_plugify->IsInitialized() && s_plugify->GetManager().IsInitialized()) {
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

	fs::path gameDir(Plat_GetGameDirectory());
	fs::path baseDir = gameDir / BASE_PATH;
	fs::path exePath = gameDir / MAMBA_PATH;

	std::error_code ec;
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

std::unique_ptr<DynLibUtils::CModule> s_server;
DynLibUtils::CVTFHookAuto<&IGameSystem::ServerPostSimulate> s_ServerPostSimulate;

void ServerPostSimulate(IGameSystem* pThis, const EventServerPostSimulate_t& msg) {
	s_ServerPostSimulate.Call(pThis, msg);

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
		case PlugifyState::Quit:
			s_plugify.reset();
			return;
		case PlugifyState::Wait:
			return;
	}

	s_state = PlugifyState::Wait;
}

static constexpr auto RWX_PERMS =
		fs::perms::owner_all |
		fs::perms::group_read |
		fs::perms::group_exec |
		fs::perms::others_read |
		fs::perms::others_exec;

class SentryInitializer {
	struct Metadata {
		std::string dsn;
		std::filesystem::path database_path;
		std::filesystem::path logs_path;
		std::optional<std::string> environment;
		std::optional<std::string> release;
		std::optional<std::string> dist;
		std::optional<std::string> proxy;
		std::optional<std::string> ca_certs;
		std::optional<std::string> transport_thread_name;
		std::optional<std::filesystem::path> handler_path;
		std::optional<std::filesystem::path> external_crash_reporter_path;
		std::optional<std::map<std::string, std::string>> tags;
		std::optional<std::vector<std::filesystem::path>> attachments;
		std::optional<double> sample_rate;
		std::optional<double> traces_sample_rate;
		std::optional<size_t> max_breadcrumbs;
		std::optional<size_t> max_spans;
		std::optional<uint64_t> shutdown_timeout;
		std::optional<bool> debug;
		std::optional<bool> listen_console;
		std::optional<bool> auto_session_tracking;
		std::optional<bool> require_user_consent;
		std::optional<bool> symbolize_stacktraces;
		std::optional<bool> system_crash_reporter_enabled;
		std::optional<bool> enable_logging_when_crashed;
		std::optional<sentry_level_t> logger_level;
		std::optional<bool> propagate_traceparent;
		std::optional<bool> crashpad_wait_for_upload;
		std::optional<bool> crashpad_limit_stack_capture_to_sp;
		std::optional<bool> attach_screenshot;
		std::optional<bool> enable_logs;
		std::optional<bool> logs_with_attributes;
		std::optional<bool> logs_with_breadcrumbs;
		std::optional<bool> enabled;
	};

	static Result<fs::path> ValidateHandler(const fs::path& exeDir, const fs::path& handlerFile) {
		fs::path handlerPath = exeDir / handlerFile;
		handlerPath.replace_filename(std::format(S2_EXECUTABLE_PREFIX "{}" S2_EXECUTABLE_SUFFIX, handlerFile.filename().string()));

		std::error_code ec;
		if (!fs::exists(handlerPath, ec)) {
			return MakeError("Crashpad handler not found: {}", plg::as_string(handlerPath));
		}

		fs::permissions(handlerPath, RWX_PERMS, fs::perm_options::replace, ec);
		if (ec) {
			return MakeError("Failed to set {} handler permissions: {}", plg::as_string(handlerPath), ec.message());
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

public:
	static Result<bool>
	Initialize(const fs::path& exeDir, const fs::path& annotationsPath) {
		// Load metadata
		auto metadataResult = ReadFile<Metadata>(exeDir / annotationsPath);
		if (!metadataResult) {
			return MakeError(std::move(metadataResult.error()));
		}

		const auto& metadata = *metadataResult;

		// Check if sentry is enabled
		if (!metadata.enabled.value_or(false)) {
			return false;
		}

		// Validate DSN
		if (metadata.dsn.empty()) {
			return MakeError("Sentry DSN is required but not provided");
		}

		// Validate handler executable
		auto handlerResult = ValidateHandler(exeDir, metadata.handler_path.value_or("crashpad_handler"));
		if (!handlerResult) {
			return MakeError(std::move(handlerResult.error()));
		}

		// Setup directories
		auto databaseResult = EnsureDirectory(exeDir / metadata.database_path, "database");
		if (!databaseResult) {
			return MakeError(std::move(databaseResult.error()));
		}

		// Create Sentry options
		sentry_options_t* options = sentry_options_new();

		// Set DSN
		sentry_options_set_dsn(options, metadata.dsn.c_str());

		// Set database path
		sentry_pcall(sentry_options_set_database_path, options, databaseResult->native());

		// Set handler path
		sentry_pcall(sentry_options_set_handler_path, options, handlerResult->native());

		// Set environment if provided
		if (metadata.environment) {
			sentry_options_set_environment_n(options, metadata.environment->c_str(), metadata.environment->size());
		}

		// Set release if provided
		if (metadata.release) {
			sentry_options_set_release_n(options, metadata.release->c_str(), metadata.release->size());
		}

		// Set dist if provided
		if (metadata.dist) {
			sentry_options_set_dist_n(options, metadata.dist->c_str(), metadata.dist->size());
		}

		// Set proxy if provided
		if (metadata.proxy) {
			sentry_options_set_proxy_n(options, metadata.proxy->c_str(), metadata.proxy->size());
		}

		// Set CA certs if provided
		if (metadata.ca_certs) {
			sentry_options_set_ca_certs_n(options, metadata.ca_certs->c_str(), metadata.ca_certs->size());
		}

		// Set transport thread name if provided
		if (metadata.transport_thread_name) {
			sentry_options_set_transport_thread_name_n(options, metadata.transport_thread_name->c_str(), metadata.transport_thread_name->size());
		}

		// Set sample rate
		if (metadata.sample_rate) {
			sentry_options_set_sample_rate(options, *metadata.sample_rate);
		}

		// Set traces sample rate
		if (metadata.traces_sample_rate) {
			sentry_options_set_traces_sample_rate(options, *metadata.traces_sample_rate);
		}

		// Set max breadcrumbs
		if (metadata.max_breadcrumbs) {
			sentry_options_set_max_breadcrumbs(options, *metadata.max_breadcrumbs);
			s_breadcrumbs_max = *metadata.max_breadcrumbs;
		}

		// Set max spans
		if (metadata.max_spans) {
			sentry_options_set_max_spans(options, *metadata.max_spans);
		}

		// Set shutdown timeout
		if (metadata.shutdown_timeout) {
			sentry_options_set_shutdown_timeout(options, *metadata.shutdown_timeout);
		}

		// Enable debug mode if requested
		if (metadata.debug) {
			sentry_options_set_debug(options, *metadata.debug);
		}

		// Set auto session tracking
		if (metadata.auto_session_tracking) {
			sentry_options_set_auto_session_tracking(options, *metadata.auto_session_tracking);
		}

		// Set require user consent
		if (metadata.require_user_consent) {
			sentry_options_set_require_user_consent(options, *metadata.require_user_consent);
		}

		// Set symbolize stacktraces
		if (metadata.symbolize_stacktraces) {
			sentry_options_set_symbolize_stacktraces(options, *metadata.symbolize_stacktraces);
		}

		// Set system crash reporter enabled
		if (metadata.system_crash_reporter_enabled) {
			sentry_options_set_system_crash_reporter_enabled(options, *metadata.system_crash_reporter_enabled);
		}

		// Set enable logging when crashed
		if (metadata.enable_logging_when_crashed) {
			sentry_options_set_logger_enabled_when_crashed(options, *metadata.enable_logging_when_crashed);
		}

		// Set logger
		if (metadata.logger_level) {
			sentry_options_set_logger(options, [](sentry_level_t level, const char* message, va_list args, [[maybe_unused]] void *userdata) {
				char buffer[2048];
				std::vsnprintf(buffer, sizeof(buffer), message, args);
				s_logger->Log(buffer, LevelToSeverity(level));
			}, nullptr);
			sentry_options_set_logger_level(options, *metadata.logger_level);
		}

		// Set propagate traceparent
		if (metadata.propagate_traceparent) {
			sentry_options_set_propagate_traceparent(options, *metadata.propagate_traceparent);
		}

		// Set crashpad wait for upload
		if (metadata.crashpad_wait_for_upload) {
			sentry_options_set_crashpad_wait_for_upload(options, *metadata.crashpad_wait_for_upload);
		}

		// Set crashpad limit stack capture to SP
		if (metadata.crashpad_limit_stack_capture_to_sp) {
			sentry_options_set_crashpad_limit_stack_capture_to_sp(options, *metadata.crashpad_limit_stack_capture_to_sp);
		}

		// Set attach screenshot
		if (metadata.attach_screenshot) {
			sentry_options_set_attach_screenshot(options, *metadata.attach_screenshot);
		}

		// Set enable logs
		if (metadata.enable_logs) {
			sentry_options_set_enable_logs(options, *metadata.enable_logs);
		}

		// Set logs with attributes
		if (metadata.logs_with_attributes) {
			sentry_options_set_logs_with_attributes(options, *metadata.logs_with_attributes);
		}

		// Set logs with breadcrumbs
		if (metadata.logs_with_breadcrumbs) {
			s_breadcrumbs = *metadata.logs_with_breadcrumbs;
		}

		// Set external crash reporter path
		if (metadata.external_crash_reporter_path) {
			sentry_pcall(sentry_options_set_external_crash_reporter_path, options, metadata.external_crash_reporter_path->native());
		}

		// Set custom tags
		if (metadata.tags) {
			for (const auto& [key, value] : *metadata.tags) {
				sentry_set_tag_n(key.c_str(), key.size(), value.c_str(), value.size());
			}
		}

		// Setup console logging if requested
		if (metadata.listen_console.value_or(false)) {
			auto logFile = exeDir / metadata.logs_path / "session.log";
			auto listenerResult = FileLoggingListener::Create(std::move(logFile));
			if (!listenerResult) {
				return MakeError(std::move(listenerResult.error()));
			} else {
				s_listener = std::move(*listenerResult);
			}
		}

		// Add custom attachments
		if (metadata.attachments) {
			for (const auto& attachment : *metadata.attachments) {
				fs::path attachmentPath = exeDir / attachment;
				std::error_code ec;
				if (fs::exists(attachmentPath, ec)) {
					sentry_pcall2(sentry_attach_file, attachmentPath.native());
				} else if (ec) {
					return MakeError("Failed to attach file: {}", ec.message());
				}
			}
		}

		sentry_options_set_on_crash(options, SentryNativeOnCrash, nullptr);

		// Initialize Sentry
		if (sentry_init(options) != 0) {
			return MakeError("Failed to initialize Sentry");
		}

		sentry_set_tag_v("app.name", S2_PROJECT_NAME);
		sentry_set_tag_v("app.version", S2_PROJECT_VERSION);
		sentry_set_tag_v("app.game", S2_GAME_NAME);
		sentry_set_tag_v("app.start", S2_GAME_START);

		sentry_set_tag_v("os.name", S2_SYSTEM_NAME);
		sentry_set_tag_v("os.version", S2_SYSTEM_VERSION);
		sentry_set_tag_v("os.arch", S2_SYSTEM_ARCH);
		sentry_set_tag_v("os.bitness", S2_SYSTEM_BITNESS);

		sentry_set_tag_v("git.commit", S2_GIT_COMMIT_HASH);
		sentry_set_tag_v("git.date", S2_GIT_COMMIT_DATE);
		sentry_set_tag_v("git.tag", S2_GIT_TAG);
		sentry_set_tag_v("git.subject", S2_GIT_COMMIT_SUBJECT);
		sentry_set_tag_v("git.branch", S2_GIT_BRANCH);
		sentry_set_tag_v("git.url", S2_GIT_URL);

		sentry_set_tag_v("build.system", S2_BUILD_SYSTEM);
		sentry_set_tag_v("build.type",  S2_BUILD_TYPE);
		sentry_set_tag_v("build.date",  S2_BUILD_DATE);
		sentry_set_tag_v("build.compiler", S2_BUILD_COMPILER);
		sentry_set_tag_v("build.compiler_version", S2_BUILD_COMPILER_VERSION);
		sentry_set_tag_v("build.toolchain", S2_BUILD_TOOLCHAIN);
		sentry_set_tag_v("build.generator", S2_BUILD_GENERATOR);
		sentry_set_tag_v("build.cmake_version", S2_BUILD_CMAKE_VERSION);
		sentry_set_tag_v("build.cxx_standard", S2_BUILD_CXX_STANDARD);
		sentry_set_tag_v("build.linker", S2_BUILD_LINKER);
		sentry_set_tag_v("build.lto", S2_BUILD_LTO);
		sentry_set_tag_v("build.sanitizers", S2_BUILD_SANITIZERS);
		sentry_set_tag_v("build.target", S2_BUILD_TARGET);
		sentry_set_tag_v("build.host", S2_BUILD_HOST);

		return true;
	}

	static void Shutdown() {
		sentry_close();
	}

private:
	static Severity LevelToSeverity(sentry_level_t level) {
		switch (level) {
			case SENTRY_LEVEL_TRACE:   return Severity::Trace;
			case SENTRY_LEVEL_DEBUG:   return Severity::Debug;
			case SENTRY_LEVEL_INFO:    return Severity::Info;
			case SENTRY_LEVEL_WARNING: return Severity::Warning;
			case SENTRY_LEVEL_ERROR:   return Severity::Error;
			case SENTRY_LEVEL_FATAL:   return Severity::Fatal;
			default:                   return Severity::Unknown;
		}
	}
};

namespace glz {
	template <>
	struct meta<sentry_level_t> {
		static constexpr auto value = enumerate(
			"trace",   SENTRY_LEVEL_TRACE,
			"debug",   SENTRY_LEVEL_DEBUG,
			"info",    SENTRY_LEVEL_INFO,
			"warning", SENTRY_LEVEL_WARNING,
			"error",   SENTRY_LEVEL_ERROR,
			"fatal",   SENTRY_LEVEL_FATAL
		);
	};

	// std::filesystem::path
	template <>
	struct from<JSON, std::filesystem::path> {
		template <auto Opts>
		static void op(std::filesystem::path& value, auto&&... args) {
			std::string str;
			parse<JSON>::op<Opts>(str, args...);
			value = str;
			if (!value.empty()) {
				value.make_preferred();
			}
		}
	};

	template <>
	struct to<JSON, std::filesystem::path> {
		template <auto Opts>
		static void op(const std::filesystem::path& value, auto&&... args) noexcept {
			serialize<JSON>::op<Opts>(value.generic_string(), args...);
		}
	};

	// std::filesystem::path
	template <>
	struct from<YAML, std::filesystem::path> {
		template <auto Opts>
		static void op(std::filesystem::path& value, auto&&... args) {
			std::string str;
			parse<YAML>::op<Opts>(str, args...);
			value = str;
			if (!value.empty()) {
				value.make_preferred();
			}
		}
	};

	template <>
	struct to<YAML, std::filesystem::path> {
		template <auto Opts>
		static void op(const std::filesystem::path& value, auto&&... args) noexcept {
			serialize<YAML>::op<Opts>(value.generic_string(), args...);
		}
	};
}

std::unique_ptr<DynLibUtils::CModule> s_engine;
DynLibUtils::CVTFHookAuto<&IHostStateMgr::RequestHS_Quit> s_HostStateMgrQuit;

void HostStateMgrQuit(IHostStateMgr* pThis) {
	s_HostStateMgrQuit.Call(pThis);

	s_state = PlugifyState::Quit;
}

class PlugifyInitializer {
private:
	template<typename T>
	static Result<T*> FindInterface(CAppSystemDict* dict, std::string_view interfaceName) {
		for (const auto& system : dict->m_Systems) {
			if (system.m_pInterfaceName == interfaceName) {
				if (auto* pSystem = static_cast<T*>(system.m_pSystem)) {
					plg::print("{}: Found {} interface", Colorize("Info", Colors::BLUE), interfaceName);
					return pSystem;
				}
			}
		}
		return MakeError("{} interface not found", interfaceName);
	}

	static Result<ConVarRefAbstract> FindConVar(std::string_view conVarName) {
		if (!g_pCVar) {
			return MakeError(CVAR_INTERFACE_VERSION " interface was not found\n");
		}

		ConVarRef conVarRef = g_pCVar->FindConVar(conVarName.data());
		if (!conVarRef.IsValidRef()) {
			return MakeError("Failed to find cvar: {}\n", conVarName);
		}

		ConVarData* conVarData = g_pCVar->GetConVarData(conVarRef);
		if (conVarData == nullptr) {
			return MakeError("Failed to get data for cvar: {}\n", conVarName);
		}

		return ConVarRefAbstract(conVarRef, conVarData);
	}

	static Result<void> InstallServerHooks() {
		DynLibUtils::CModule server("server");
		if (!server) {
			return MakeError("Failed to load server module");
		}

		if (auto table = server.GetVirtualTableByName("CLightQueryGameSystem")) {
			DynLibUtils::CVirtualTable vtable(table);
			s_ServerPostSimulate.Hook(vtable, &ServerPostSimulate);
		} else {
			return MakeError("Virtual table CLightQueryGameSystem not found");
		}

		DynLibUtils::CModule engine("engine2");
		if (!engine) {
			return MakeError("Failed to load engine module");
		}

		if (auto table = engine.GetVirtualTableByName("CHostStateMgr")) {
			DynLibUtils::CVirtualTable vtable(table);
			s_HostStateMgrQuit.Hook(vtable, &HostStateMgrQuit);
		} else {
			return MakeError("Virtual table CHostStateMgr not found");
		}

		s_server = std::make_unique<DynLibUtils::CModule>(std::move(server));
		s_engine = std::make_unique<DynLibUtils::CModule>(std::move(engine));

		plg::print("{}: Server hooks installed", Colorize("Info", Colors::GREEN));
		return {};
	}

	static void SetupCrashHandler() {
		if (!s_sentry)
			return;

#if S2_PLATFORM_WINDOWS
		SetDefaultMiniDumpHandler([](MiniDumpHandlerData_t* data) {
			sentry_ucontext_t ctx{ .exception_ptrs = *data->pExceptionInfo };
			sentry_handle_exception(&ctx);
		}, true);
		plg::print("{}: Crash handler registered", Colorize("Info", Colors::GREEN));
#endif
	}

	static void SetupServerTags() {
		if (!s_sentry)
			return;

		if (auto hostname = FindConVar("hostname")) {
			sentry_set_tag_v("host.name", hostname->GetAs<CUtlString>());
		}
		if (auto hostip = FindConVar("hostip")) {
			sentry_set_tag_v("host.ip", hostip->GetAs<CUtlString>());
		}
		if (auto hostport = FindConVar("hostport")) {
			sentry_set_tag_v("host.port", hostport->GetAs<CUtlString>());
		}

		if (g_pNetworkSystem) {
			sentry_set_tag_v("host.public_ip", g_pNetworkSystem->GetPublicAdr().ToString());
			sentry_set_tag_v("host.local_ip", g_pNetworkSystem->GetLocalAdr().ToString());
		}
	}

	static void FindInterfaces(CAppSystemDict* dict) {
		// Find ICvar interface
		if (auto cvarResult = FindInterface<ICvar>(dict, CVAR_INTERFACE_VERSION); !cvarResult) {
			plg::print("{}: {}", Colorize("Warning", Colors::YELLOW), cvarResult.error());
			// Non-fatal: continue without ICvar
		} else {
			g_pCVar = *cvarResult;
		}

		// Find INetworkSystem interface
		if (auto networkResult = FindInterface<INetworkSystem>(dict, NETWORKSYSTEM_INTERFACE_VERSION); !networkResult) {
			plg::print("{}: {}", Colorize("Warning", Colors::YELLOW), networkResult.error());
			// Non-fatal: continue without INetworkSystem
		} else {
			g_pNetworkSystem = *networkResult;
		}
	}

	static Result<fs::path> ValidateMicromamba(const fs::path& exePath) {
		std::error_code ec;
		if (!fs::exists(exePath, ec)) {
			return MakeError("Micromamba executable not found at: {}", plg::as_string(exePath));
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
	static Result<std::shared_ptr<Plugify>> Initialize(CAppSystemDict* dict) {
		// Setup logging if listener exists
		if (s_listener) {
			LoggingSystem_PushLoggingState(false, false);
			LoggingSystem_RegisterLoggingListener(s_listener.get());
		}

		// Notify about sentry
		plg::print(
			"{}: Sentry {} in configuration",
			Colorize("Info", Colors::BLUE),
			Colorize(s_sentry ? "enabled" : "disabled", Colors::MAGENTA)
		);

		// Setup crash handler
		SetupCrashHandler();

		// Find requires interfaces
		FindInterfaces(dict);

		// Register ConVars
		ConVar_Register(FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);

		// Append sentry tags to identify server
		SetupServerTags();

		// Build base directory path
		fs::path gameDir(Plat_GetGameDirectory());
		fs::path baseDir = gameDir / BASE_PATH;
		fs::path exePath = gameDir / MAMBA_PATH;

		// Install server hooks
		if (auto hookResult = InstallServerHooks(); !hookResult) {
			plg::print("{}: {}", Colorize("Warning", Colors::YELLOW), hookResult.error());
			// Non-fatal: continue without hooks
		}

		// Validate micromamba
		if (auto mambaResult = ValidateMicromamba(exePath); !mambaResult) {
			return MakeError(std::move(mambaResult.error()));
		}

		// Create and initialize Plugify context
		return CreatePlugifyContext(baseDir);
	}
};

DynLibUtils::CVTFHookAuto<&CAppSystemDict::OnAppSystemLoaded> s_OnAppSystemLoaded;
std::unordered_set<std::string> s_loadList;

void OnAppSystemLoaded(CAppSystemDict* pThis) {
	s_OnAppSystemLoaded.Call(pThis);

	if (s_plugify) {
		return;
	}

	if (s_loadList.empty()) {
		s_loadList.reserve(static_cast<size_t>(pThis->m_Modules.Count()));
	}

	constexpr std::string_view moduleName = S2_GAME_START;

	for (const auto& module : pThis->m_Modules) {
		if (module.m_pModuleName) {
			if (s_loadList.insert(module.m_pModuleName).second) {
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
	plg::path_string execPath(MAX_PATH, PLUGIFY_PATH_LITERAL('\0'));

	while (true) {
#if S2_PLATFORM_WINDOWS
		size_t len = ::GetModuleFileNameW(nullptr, execPath.data(), static_cast<DWORD>(execPath.length()));
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
	Severity severity = Severity::Info;

	for (int i = 1; i < argc; ++i) {
		std::string_view arg = argv[i];

		if (arg.starts_with("--verbosity=")) {
			std::string_view value = arg.substr(12); // after '='
			severity = ParseSeverity(std::string(value));
		}
	}

	auto binary_path = ExecutablePath().value_or(fs::current_path());

	std::error_code ec;
	if (fs::is_directory(binary_path, ec) && binary_path.filename() == "game") {
		binary_path /= "bin/" S2_BINARY;
	}

	if (!std::is_debugger_present()) {
		auto result = SentryInitializer::Initialize(binary_path, "sentry.jsonc");
		if (!result) {
			std::println(std::cerr, "Sentry error: {}", result.error());
			return 1;
		}
		s_sentry = *result;
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
	s_logger->SetLogLevel(severity);

	auto table = engine.GetVirtualTableByName("CMaterialSystem2AppSystemDict");
	DynLibUtils::CVirtualTable vtable(table);
	s_OnAppSystemLoaded.Hook(vtable, &OnAppSystemLoaded);

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

	if (s_sentry) {
		SentryInitializer::Shutdown();
	}

	s_ServerPostSimulate.Unhook();
	s_OnAppSystemLoaded.Unhook();
	s_HostStateMgrQuit.Unhook();

	g_pCVar = nullptr;
	g_pSource2Server = nullptr;

	s_server.reset();
	s_engine.reset();
	s_listener.reset();
	s_logger.reset();

	return res;
}
