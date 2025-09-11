#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <unordered_set>
#include <queue>
#include <thread>
#include <chrono>

#include <plugify/assembly.hpp>
#include <plugify/logger.hpp>
#include <plugify/plugify.hpp>
#include <plugify/extension.hpp>
#include <plugify/manager.hpp>

#include <plg/debugging.hpp>
#include <plg/format.hpp>
#include <plg/enum.hpp>

#include <client/crashpad_client.h>
#include <client/crash_report_database.h>
#include <client/settings.h>

#include <CLI/CLI.hpp>
#include <glaze/glaze.hpp>

#include <dynlibutils/module.hpp>
#include <dynlibutils/virtual.hpp>
#include <dynlibutils/vthook.hpp>

#include <appframework/iappsystem.h>
#include <convar.h>
#include <eiface.h>
#include <igamesystem.h>
#include <tier0/logging.h>

#undef FormatMessage

using namespace plugify;

// Source2 color definitions
namespace S2Colors {
	static const Color WHITE = Color(255, 255, 255, 255);
	static const Color RED = Color(255, 0, 0, 255);
	static const Color GREEN = Color(0, 255, 0, 255);
	static const Color YELLOW = Color(255, 255, 0, 255);
	static const Color BLUE = Color(0, 127, 255, 255);
	static const Color MAGENTA = Color(255, 0, 255, 255);
	static const Color CYAN = Color(0, 255, 255, 255);
	static const Color GRAY = Color(127, 127, 127, 255);
	static const Color BRIGHT_RED = Color(255, 128, 128, 255);
	static const Color BRIGHT_GREEN = Color(128, 255, 128, 255);
	static const Color BRIGHT_YELLOW =  Color(255, 255, 128, 255);
	static const Color BRIGHT_BLUE =  Color(128, 128, 255, 255);
	static const Color BRIGHT_MAGENTA =  Color(255, 128, 255, 255);
	static const Color BRIGHT_CYAN =  Color(128, 255, 255, 255);
	static const Color BRIGHT_GRAY =  Color(200, 200, 200, 255);
}

// 0x00  NUL  (Null)                    ❌ NEVER USE - String terminator
// 0x01  SOH  (Start of Heading)        ✅ Safe to use
// 0x02  STX  (Start of Text)           ✅ Safe to use
// 0x03  ETX  (End of Text)             ✅ Safe to use
// 0x04  EOT  (End of Transmission)     ✅ Safe to use
// 0x05  ENQ  (Enquiry)                 ✅ Safe to use
// 0x06  ACK  (Acknowledge)             ✅ Safe to use
// 0x07  BEL  (Bell)                    ⚠️  AVOID - Makes beep sound
// 0x08  BS   (Backspace)               ⚠️  AVOID - Terminal control
// 0x09  HT   (Horizontal Tab)          ❌ AVOID - Common in text (\t)
// 0x0A  LF   (Line Feed)               ❌ AVOID - Newline on Unix (\n)
// 0x0B  VT   (Vertical Tab)            ⚠️  Risky - Sometimes used
// 0x0C  FF   (Form Feed)               ⚠️  Risky - Page break
// 0x0D  CR   (Carriage Return)         ❌ AVOID - Newline on Windows (\r)
// 0x0E  SO   (Shift Out)               ✅ Safe to use
// 0x0F  SI   (Shift In)                ✅ Safe to use
// 0x10  DLE  (Data Link Escape)        ✅ Safe to use
// 0x11  DC1  (Device Control 1/XON)    ⚠️  Risky - Flow control
// 0x12  DC2  (Device Control 2)        ✅ Safe to use
// 0x13  DC3  (Device Control 3/XOFF)   ⚠️  Risky - Flow control
// 0x14  DC4  (Device Control 4)        ✅ Safe to use
// 0x15  NAK  (Negative Acknowledge)    ✅ Safe to use
// 0x16  SYN  (Synchronous Idle)        ✅ Safe to use
// 0x17  ETB  (End of Trans. Block)     ✅ Safe to use
// 0x18  CAN  (Cancel)                  ✅ Safe to use
// 0x19  EM   (End of Medium)           ✅ Safe to use
// 0x1A  SUB  (Substitute)              ⚠️  AVOID - EOF on Windows (Ctrl+Z)
// 0x1B  ESC  (Escape)                  ❌ AVOID - ANSI escape sequences
// 0x1C  FS   (File Separator)          ✅ Safe to use
// 0x1D  GS   (Group Separator)         ✅ Safe to use
// 0x1E  RS   (Record Separator)        ✅ Safe to use
// 0x1F  US   (Unit Separator)          ✅ Safe to use

// ANSI Color codes
struct Colors {
	// SAFE control characters (0x01-0x0F range, avoiding common ones)
	static constexpr char RESET         = '\x01';  // SOH (Start of Heading)
	static constexpr char BOLD          = '\x01';  // SOH (Start of Heading)
	static constexpr char RED           = '\x02';  // STX (Start of Text)
	static constexpr char GREEN         = '\x03';  // ETX (End of Text)
	static constexpr char YELLOW        = '\x04';  // EOT (End of Transmission)
	static constexpr char BLUE          = '\x05';  // ENQ (Enquiry)
	static constexpr char MAGENTA       = '\x06';  // ACK (Acknowledge)
	// Skip 0x07 (BEL - makes sound on some terminals)
	static constexpr char CYAN          = '\x0E';  // SO (Shift Out)
	static constexpr char GRAY          = '\x0F';  // SI (Shift In)
	static constexpr char BRIGHT_RED    = '\x10';  // DLE (Data Link Escape)
	static constexpr char BRIGHT_GREEN  = '\x11';  // DC1 (Device Control 1)
	static constexpr char BRIGHT_YELLOW = '\x12';  // DC2 (Device Control 2)
	static constexpr char BRIGHT_BLUE   = '\x13';  // DC3 (Device Control 3)
	static constexpr char BRIGHT_MAGENTA= '\x14';  // DC4 (Device Control 4)
	static constexpr char BRIGHT_CYAN   = '\x15';  // NAK (Negative Acknowledge)
	static constexpr char BRIGHT_GRAY   = '\x16';  // SYN (Synchronous Idle)
};

class AnsiColorParser {
public:
    struct TextSegment {
        std::string text;
        Color color;
    };

	// Use bit flags to quickly check if a byte is a color code
    static constexpr bool isColorCode[256] = {
        false, true,  true,  true,  true,  true,  true,  false, // 0x00-0x07
        false, false, false, false, false, false, true,  true,  // 0x08-0x0F
        true,  true,  true,  true,  true,  true,  true,  false, // 0x10-0x17
        // ... rest are false
    };

    static inline Color colorMap[256] = {
        S2Colors::WHITE,     // 0x00 (unused)
        S2Colors::WHITE,     // 0x01 RESET
        S2Colors::RED,       // 0x02
        S2Colors::GREEN,     // 0x03
        S2Colors::YELLOW,    // 0x04
        S2Colors::BLUE,      // 0x05
        S2Colors::MAGENTA,   // 0x06
        S2Colors::WHITE,     // 0x07 (skip - BEL)
        S2Colors::WHITE,     // 0x08 (skip)
        S2Colors::WHITE,     // 0x09 (skip - TAB)
        S2Colors::WHITE,     // 0x0A (skip - LF)
        S2Colors::WHITE,     // 0x0B (skip - VT)
        S2Colors::WHITE,     // 0x0C (skip - FF)
        S2Colors::WHITE,     // 0x0D (skip - CR)
        S2Colors::CYAN,      // 0x0E
        S2Colors::GRAY,      // 0x0F
        S2Colors::BRIGHT_RED,    // 0x10
        S2Colors::BRIGHT_GREEN,  // 0x11
        S2Colors::BRIGHT_YELLOW, // 0x12
        S2Colors::BRIGHT_BLUE,   // 0x13
        S2Colors::BRIGHT_MAGENTA,// 0x14
        S2Colors::BRIGHT_CYAN,   // 0x15
        S2Colors::BRIGHT_GRAY,   // 0x16
        // ... etc
    };

	static std::vector<TextSegment> Parse(const std::string& input) {
		std::vector<TextSegment> segments;
		segments.reserve(3); // Typical estimate

		Color currentColor = S2Colors::WHITE;
		size_t textStart = 0;

		for (size_t i = 0; i < input.length(); ++i) {
			unsigned char byte = static_cast<unsigned char>(input[i]);

			if (byte < 32 && isColorCode[byte]) {
				// Found color code
				if (i > textStart) {
					segments.emplace_back(
						input.substr(textStart, i - textStart),
						currentColor
					);
				}

				currentColor = colorMap[byte];
				textStart = i + 1;
			}
		}

		// Add remaining text
		if (textStart < input.length()) {
			segments.emplace_back(
				input.substr(textStart),
				currentColor
			);
		}

		return segments;
	}

	// Helper to strip color codes for display/logging
	static std::string StripColors(std::string_view input) {
		std::string result;
		result.reserve(input.length());

		for (char c : input) {
			unsigned char byte = static_cast<unsigned char>(c);
			if (byte >= 32 || !isColorCode[byte]) {
				result += c;
			}
		}

		return result;
	}
};

class S2Logger final : public ILogger {
public:
	explicit S2Logger(const char* name, int flags = 0, LoggingVerbosity_t verbosity = LV_DEFAULT, Color color = UNSPECIFIED_LOGGING_COLOR) {
		m_channelID = LoggingSystem_RegisterLoggingChannel(name, nullptr, flags, verbosity, color);
	}

	~S2Logger() override = default;

	void Log(const char* message, LoggingSeverity_t severity, Color color) const {
		std::scoped_lock<std::mutex> lock(m_mutex);
		LoggingSystem_Log(m_channelID, severity, color, message);
		LoggingSystem_Log(m_channelID, severity, "\n");
	}

	void Log(const std::string& message, LoggingSeverity_t severity) const {
		auto segments = AnsiColorParser::Parse(message);

		std::scoped_lock<std::mutex> lock(m_mutex);

		for (const auto& segment : segments) {
			// Log each segment with its associated color
			LoggingSystem_Log(m_channelID, severity, segment.color, segment.text.c_str());
		}
		LoggingSystem_Log(m_channelID, severity, "\n");
	}

	void Log(std::string_view message, Severity severity, [[maybe_unused]] std::source_location loc) override {
		if (severity <= m_severity) {
			auto output = FormatMessage(message, severity, loc);

			std::scoped_lock<std::mutex> lock(m_mutex);
			switch (severity) {
				case Severity::Unknown:
					LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 255, 255), output.c_str());
					break;
				case Severity::Fatal:
					LoggingSystem_Log(m_channelID, LS_ERROR, Color(255, 0, 255, 255), output.c_str());
					break;
				case Severity::Error:
					LoggingSystem_Log(m_channelID, LS_WARNING, Color(255, 0, 0, 255), output.c_str());
					break;
				case Severity::Warning:
					LoggingSystem_Log(m_channelID, LS_WARNING, Color(255, 127, 0, 255), output.c_str());
					break;
				case Severity::Info:
					LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 0, 255), output.c_str());
					break;
				case Severity::Debug:
					LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(0, 255, 0, 255), output.c_str());
					break;
				case Severity::Verbose:
					LoggingSystem_Log(m_channelID, LS_MESSAGE, Color(255, 255, 255, 255), output.c_str());
					break;
				default:
					break;
			}
		}
	}

	void SetLogLevel(Severity minSeverity) {
		m_severity = minSeverity;
	}

	void Flush() override {}

protected:
	static std::string FormatMessage(std::string_view message, Severity severity,
							  const std::source_location& loc) {
		using namespace std::chrono;

		auto now = system_clock::now();

		// Split into seconds + milliseconds
		auto seconds = floor<std::chrono::seconds>(now);
		auto ms = duration_cast<milliseconds>(now - seconds);

		return std::format("[{:%F %T}.{:03d}] [{}] [{}:{}] {}\n",
						   seconds, // %F = YYYY-MM-DD, %T = HH:MM:SS
						   static_cast<int>(ms.count()),
						   plg::enum_to_string(severity),
						   loc.file_name(),
						   loc.line(),
						   message);
	}

private:
	mutable std::mutex m_mutex;
	std::atomic<Severity> m_severity{Severity::Unknown};
	LoggingChannelID_t m_channelID;
};

enum class PlugifyState {
	Wait,
	Load,
	Unload,
	Reload
};

std::shared_ptr<S2Logger> s_logger;
std::shared_ptr<Plugify> s_context;
std::unique_ptr<ILoggingListener> s_listener;
PlugifyState s_state;

namespace plg {
	template<detail::is_string_like First>
	PLUGIFY_FORCE_INLINE void print(First&& first) {
		s_logger->Log(std::forward<First>(first), LS_MESSAGE);
	}

	template<typename... Args>
	PLUGIFY_FORCE_INLINE void print(std::format_string<Args...> fmt, Args&&... args) {
		s_logger->Log(std::format(fmt, std::forward<Args>(args)...), LS_MESSAGE);
	}
}

namespace {
	template<typename T>
	Result<T> ReadJson(const std::filesystem::path& path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return MakeError("Failed to read json file: {} - {}", plg::as_string(path), std::strerror(errno));
		}
		auto text = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		auto json = glz::read_jsonc<T>(text);
		if (!json) {
			return MakeError(glz::format_error(json.error(), text));
		}

		return *json;
	}

	// Icons for status display
	/*struct Icons {
		static constexpr std::string_view Ok = "[OK]";
		static constexpr std::string_view Fail = "[FAIL]";
		static constexpr std::string_view Warning = "[WARN]";
		static constexpr std::string_view Skipped = "[SKIP]";
		static constexpr std::string_view Valid = "[VALID]";
		static constexpr std::string_view Resolving = "[...]";
		static constexpr std::string_view Arrow = ">";
		static constexpr std::string_view Number = "#";
		static constexpr std::string_view Unknown = "[?]";
		static constexpr std::string_view Missing = "[!]";
	};*/

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
#if 0
	struct StateInfo {
		std::string_view symbol;
		Color color;
	};

	StateInfo GetStateInfo(ExtensionState state) {
		switch (state) {
			case ExtensionState::Started:
			case ExtensionState::Loaded:
				return { Icons.Ok, S2Colors::GREEN };
			case ExtensionState::Failed:
			case ExtensionState::Corrupted:
				return { Icons.Fail, S2Colors::RED };
			case ExtensionState::Unresolved:
				return { Icons.Warning, S2Colors::YELLOW };
			case ExtensionState::Disabled:
			case ExtensionState::Skipped:
				return { Icons.Skipped, S2Colors::GRAY };
			case ExtensionState::Loading:
			case ExtensionState::Starting:
			case ExtensionState::Parsing:
			case ExtensionState::Resolving:
				return { Icons.Resolving, S2Colors::CYAN };
			default:
				return { Icons.Unknown, S2Colors::GRAY };
		}
	}
#else
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
		"✓", "✗", "⚠", "○", "●", "⋯", "→", "#", "?", "ℹ", "=", "≠", "⚙"
	};

	// Plain ASCII fallback
	inline constexpr Glyphs AsciiGlyphs{
		"v", "x", "!", "o", "*", "...", "->", "#", "?", "i", "=", "!=", ">>"
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

#endif

	// Helper to truncate string
	std::string Truncate(const std::string& str, size_t maxLen) {
		if (str.length() <= maxLen) {
			return str;
		}
		return str.substr(0, maxLen - 3) + "...";
	}

	// Format file size
	std::uintmax_t GetSizeRecursive(const std::filesystem::path& path) {
		namespace fs = std::filesystem;
		std::uintmax_t totalSize = 0;
		std::error_code ec;

		if (fs::is_regular_file(path, ec)) {
			auto sz = fs::file_size(path, ec);
			if (!ec) {
				totalSize += sz;
			}
		} else if (fs::is_directory(path)) {
			for (auto& entry : fs::recursive_directory_iterator(path,
					fs::directory_options::skip_permission_denied, ec)) {
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

	std::string FormatFileSize(const std::filesystem::path& path) {
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
			plg::print("{}: Invalid argument: trailing characters after the valid part", Colorize("Error", Colors::RED));
			return {};
		}
		return UniqueId{result};
	}

	// Filter options
	struct FilterOptions {
		std::optional<std::vector<ExtensionState>> states;
		std::optional<std::vector<std::string>> languages;
		bool showOnlyFailed = false;
		bool showOnlyWithErrors = false;
		std::optional<std::string> searchQuery;
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
			auto lang = ext->GetLanguage();
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

	// Filter extensions based on criteria
	std::vector<const Extension*> FilterExtensions(const std::vector<const Extension*>& extensions, const FilterOptions& filter) {
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
		std::sort(extensions.begin(), extensions.end(), [sortBy, reverse](const Extension* a, const Extension* b) {
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
		});
	}

	void ShowVersion() {
		static std::string copyright = std::format(
			"Copyright (C) 2023-{}{}{}{} Untrusted Modders Team",
			__DATE__[7],
			__DATE__[8],
			__DATE__[9],
			__DATE__[10]
		);
		plg::print(R"(      ____)");
		plg::print(R"( ____|    \         Plugify )" + s_context->GetVersion().to_string());
		plg::print(R"((____|     `._____  )" + copyright);
		plg::print(R"( ____|       _|___)");
		plg::print(R"((____|     .'       This program may be freely redistributed under)");
		plg::print(R"(     |____/         the terms of the MIT License.)");
	}

	// Print dependency tree
#if 0
	void PrintDependencyTree(const Extension* ext, const Manager& manager,
							 const std::string& prefix = "", bool isLast = true) {
		std::string connector = isLast ? "└─ " : "├─ ";
		auto [symbol, color] = GetStateInfo(ext->GetState());

		std::string line = std::format("{}{}{} {} {} {}",
			prefix, connector, symbol, ext->GetName(),
			ext->GetVersionString(),
			ext->HasErrors() ? "[ERROR]" : "");
		PLG_LOG(line.c_str(), color);

		const auto& deps = ext->GetDependencies();
		std::string newPrefix = prefix + (isLast ? "    " : "│   ");

		for (size_t i = 0; i < deps.size(); ++i) {
			const auto& dep = deps[i];
			bool lastDep = (i == deps.size() - 1);

			auto depExt = manager.FindExtension(dep.GetName());
			if (depExt) {
				PrintDependencyTree(depExt, manager, newPrefix, lastDep);
			} else {
				std::string depConnector = lastDep ? "└─ " : "├─ ";
				std::string status = dep.IsOptional() ? "[optional]" : "[required]";
				std::string depLine = std::format("{}{}{} {} {} {} [NOT FOUND]",
					newPrefix, depConnector, Icons.Skipped,
					dep.GetName(), dep.GetConstraints().to_string(), status);
				PLG_WARN(depLine.c_str());
			}
		}
	}
#else
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
			Colorize(ext->GetName(), Colors::BOLD),
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
#endif

#if 0
	// Health check
	struct HealthReport {
		size_t score = 100;
		std::vector<std::string> issues;
		std::vector<std::string> warnings;
		plg::flat_map<std::string, size_t> statistics;
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
			if (ext->GetState() == ExtensionState::Failed ||
				ext->GetState() == ExtensionState::Corrupted) {
				++failedCount;
				report.issues.push_back(std::format("{} is in failed state", ext->GetName()));
			}

			errorCount += ext->GetErrors().size();
			warningCount += ext->GetWarnings().size();

			auto loadTime = ext->GetOperationTime(ExtensionState::Loaded);
			if (std::chrono::duration_cast<std::chrono::milliseconds>(loadTime).count() > 1000) {
				++slowLoadCount;
				report.warnings.push_back(
					std::format("{} took {} to load", ext->GetName(), FormatDuration(loadTime)));
			}
		}

		report.statistics["failed_extensions"] = failedCount;
		report.statistics["extensions_with_errors"] = errorCount;
		report.statistics["total_warnings"] = warningCount;
		report.statistics["slow_loading_extensions"] = slowLoadCount;

		// Calculate score
		if (failedCount > 0) report.score -= failedCount * 15;
		if (errorCount > 0) report.score -= errorCount * 10;
		if (warningCount > 0) report.score -= std::min<size_t>(warningCount * 2, 20);
		if (slowLoadCount > 0) report.score -= std::min<size_t>(slowLoadCount * 5, 15);

		report.score = std::max<size_t>(0, report.score);

		return report;
	}
#else
	struct HealthReport {
        size_t score = 100;  // 0-100
        std::vector<std::string> issues;
        std::vector<std::string> warnings;
        plg::flat_map<std::string, size_t> statistics;
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
#endif

#if 0
	// List extensions with formatting
	void ListExtensions(const std::vector<const Extension*>& extensions,
						const std::string& typeName, bool showDetails = false) {
		auto count = extensions.size();
		if (!count) {
			PLG_WARN(std::format("No {}s found.", typeName).c_str());
			return;
		}

		PLG_INFO(std::format("Listing {} {}{}:", count, typeName, count > 1 ? "s" : "").c_str());
		PLG_INFO(std::string(80, '-').c_str());

		// Header
		PLG_INFO(std::format("{:<3} {:<25} {:<15} {:<12} {:<8} {:<12}",
			"#", "Name", "Version", "State", "Lang", "Load Time").c_str());
		PLG_INFO(std::string(80, '-').c_str());

		size_t index = 1;
		for (const auto& ext : extensions) {
			auto state = ext->GetState();
			auto [symbol, color] = GetStateInfo(state);
			auto name = !ext->GetName().empty() ? ext->GetName() :
						ext->GetLocation().filename().string();

			std::string loadTime = "N/A";
			try {
				auto duration = ext->GetOperationTime(ExtensionState::Loaded);
				if (duration.count() > 0) {
					loadTime = FormatDuration(duration);
				}
			} catch (...) {}

			std::string label(plg::enum_to_string(state));

			std::string line = std::format("{:<3} {:<25} {:<15} {} {:<11} {:<8} {:<12}",
				index++,
				Truncate(name, 24),
				ext->GetVersionString(),
				symbol,
				Truncate(label, 10),
				Truncate(ext->GetLanguage(), 7),
				loadTime);
			PLG_LOG(line.c_str(), color);

			// Show errors/warnings if any
			if (showDetails) {
				for (const auto& error : ext->GetErrors()) {
					PLG_ERROR(std::format("     └─ Error: {}", error).c_str());
				}
				for (const auto& warning : ext->GetWarnings()) {
					PLG_WARN(std::format("     └─ Warning: {}", warning).c_str());
				}
			}
		}
		PLG_INFO(std::string(80, '-').c_str());
	}
#else

    void ListPlugins(
		const std::vector<const Extension*>& plugins,
		bool showDetails = false,
        const FilterOptions& filter = {},
        SortBy sortBy = SortBy::Name,
        bool reverseSort = false
    ) {
    	// Apply filters
    	auto filtered = FilterExtensions(plugins, filter);
    	SortExtensions(filtered, sortBy, reverseSort);

        auto count = filtered.size();
        if (!count) {
            plg::print(Colorize("No plugins found matching criteria.", Colors::YELLOW));
            return;
        }

        plg::print(
            "{}:",
            Colorize(std::format("Listing {} plugin{}", count, (count > 1) ? "s" : ""), Colors::BOLD)
        );
        plg::print(std::string(80, '-'));

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
        plg::print(std::string(80, '-'));

        size_t index = 1;
        for (const auto& plugin : filtered) {
            auto state = plugin->GetState();
            auto stateStr = plg::enum_to_string(state);
            auto [symbol, color] = GetStateInfo(state);
            auto& name = !plugin->GetName().empty() ? plugin->GetName() : plg::as_string(plugin->GetLocation().filename());

            // Get load time if available
            std::string loadTime = "N/A";
            try {
                auto duration = plugin->GetOperationTime(ExtensionState::Loaded);
                if (duration.count() > 0) {
                    loadTime = FormatDuration(duration);
                }
            } catch (...) {}

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
        	if (showDetails) {
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
        }
        plg::print(std::string(80, '-'));

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
		const std::vector<const Extension*>& modules,
		bool showDetails = false,
        const FilterOptions& filter = {},
        SortBy sortBy = SortBy::Name,
        bool reverseSort = false
    ) {
    	// Apply filters
    	auto filtered = FilterExtensions(modules, filter);
    	SortExtensions(filtered, sortBy, reverseSort);

        auto count = filtered.size();
        if (!count) {
            plg::print(Colorize("No modules found matching criteria.", Colors::YELLOW));
            return;
        }

        plg::print(
            "{}:",
            Colorize(std::format("Listing {} module{}", count, (count > 1) ? "s" : ""), Colors::BOLD)
        );
        plg::print(std::string(80, '-'));

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
        plg::print(std::string(80, '-'));

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
        	if (showDetails) {
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
        }
        plg::print(std::string(80, '-'));

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

#endif

#if 0
	// Show detailed extension info
	void ShowExtensionDetails(const Extension* ext, const std::string& typeName) {
		PLG_INFO(std::string(80, '=').c_str());
		PLG_INFO(std::format("{} INFORMATION: {}", typeName, ext->GetName()).c_str());
		PLG_INFO(std::string(80, '=').c_str());

		// Status
		auto [symbol, stateColor] = GetStateInfo(ext->GetState());
		PLG_LOG(std::format("{} Status: {}", symbol, plg::enum_to_string(ext->GetState())).c_str(),
				stateColor);

		// Basic Information
		PLG_INFO("[Basic Information]");
		PLG_INFO(std::format("  ID: {}", ext->GetId()).c_str());
		PLG_INFO(std::format("  Name: {}", ext->GetName()).c_str());
		PLG_INFO(std::format("  Version: {}", ext->GetVersionString()).c_str());
		PLG_INFO(std::format("  Language: {}", ext->GetLanguage()).c_str());
		PLG_INFO(std::format("  Location: {}", plg::as_string(ext->GetLocation())).c_str());
		PLG_INFO(std::format("  File Size: {}", FormatFileSize(ext->GetLocation())).c_str());

		// Optional Information
		if (!ext->GetDescription().empty()) {
			PLG_INFO(std::format("  Description: {}", ext->GetDescription()).c_str());
		}
		if (!ext->GetAuthor().empty()) {
			PLG_INFO(std::format("  Author: {}", ext->GetAuthor()).c_str());
		}
		if (!ext->GetWebsite().empty()) {
			PLG_INFO(std::format("  Website: {}", ext->GetWebsite()).c_str());
		}
		if (!ext->GetLicense().empty()) {
			PLG_INFO(std::format("  License: {}", ext->GetLicense()).c_str());
		}

		// Plugin-specific
		if (ext->IsPlugin() && !ext->GetEntry().empty()) {
			PLG_INFO("[Plugin Details]");
			PLG_INFO(std::format("  Entry Point: {}", ext->GetEntry()).c_str());
		}

		// Module-specific
		if (ext->IsModule() && !ext->GetRuntime().empty()) {
			PLG_INFO("[Module Details]");
			PLG_INFO(std::format("  Runtime: {}", plg::as_string(ext->GetRuntime()).c_str());
		}

		// Dependencies
		const auto& deps = ext->GetDependencies();
		if (!deps.empty()) {
			PLG_INFO(std::format("[Dependencies] ({} total)", deps.size()).c_str());
			for (const auto& dep : deps) {
				std::string depStr = std::format("  {} {} {}",
					dep.IsOptional() ? Icons.Skipped : Icons.Valid,
					dep.GetName(),
					dep.GetConstraints().to_string());
				PLG_INFO(depStr.c_str());
				if (dep.IsOptional()) {
					PLG_INFO("    └─ Optional");
				}
			}
		}

		// Performance
		PLG_INFO("[Performance Metrics]");
		auto totalTime = ext->GetTotalTime();
		PLG_LOG(std::format("  Total Time: {}", FormatDuration(totalTime)).c_str(),
				totalTime > std::chrono::seconds(1) ? S2Colors::YELLOW : S2Colors::GREEN);

		// Errors and Warnings
		if (ext->HasErrors() || ext->HasWarnings()) {
			PLG_INFO("[Issues]");
			for (const auto& error : ext->GetErrors()) {
				PLG_ERROR(std::format("  ERROR: {}", error).c_str());
			}
			for (const auto& warning : ext->GetWarnings()) {
				PLG_WARN(std::format("  WARNING: {}", warning).c_str());
			}
		} else {
			PLG_SUCCESS(std::format("{} No issues detected", Icons.Ok).c_str());
		}

		PLG_INFO(std::string(80, '=').c_str());
	}
#else
	void ShowPlugin(const Extension* plugin) {
        // Display detailed plugin information with colors
        plg::print(std::string(80, '='));
        plg::print(
            "{}: {}",
            Colorize("PLUGIN INFORMATION", Colors::BOLD),
            Colorize(plugin->GetName(), Colors::CYAN)
        );
        plg::print(std::string(80, '='));

        // Status indicator
        auto [symbol, stateColor] = GetStateInfo(plugin->GetState());
        plg::print(
            "\n{} {} {}",
            Colorize(symbol, stateColor),
            Colorize("Status:", Colors::BOLD),
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
        plg::print("  {:<15} {}", Colorize("Location:", Colors::GRAY), plg::as_string(plugin->GetLocation()));
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
                    Colorize(dep.GetName(), Colors::BOLD),
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
        ExtensionState operations[] = { ExtensionState::Parsing,
                                        ExtensionState::Resolving,
                                        ExtensionState::Loading,
                                        ExtensionState::Starting };

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

        plg::print(std::string(80, '='));
    }

    void ShowModule(const Extension* module) {
        // Display detailed module information with colors
        plg::print(std::string(80, '='));
        plg::print(
            "{}: {}",
            Colorize("MODULE INFORMATION", Colors::BOLD),
            Colorize(module->GetName(), Colors::MAGENTA)
        );
        plg::print(std::string(80, '='));

        // Status indicator
        auto [symbol, stateColor] = GetStateInfo(module->GetState());
        plg::print(
            "\n{} {} {}",
            Colorize(symbol, stateColor),
            Colorize("Status:", Colors::BOLD),
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
        plg::print("  {:<15} {}", Colorize("Location:", Colors::GRAY), plg::as_string(module->GetLocation()));
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
                bool exists = std::filesystem::exists(dirs[i]);
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
                    Colorize(dep.GetName(), Colors::BOLD),
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
        ExtensionState operations[] = { ExtensionState::Parsing,
                                        ExtensionState::Resolving,
                                        ExtensionState::Loading,
                                        ExtensionState::Starting };

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

        plg::print(std::string(80, '='));
    }
#endif

#if 0
	// Search extensions
	void SearchExtensions(const Manager& manager, const std::string& query) {
		auto allExtensions = manager.GetExtensions();
		std::vector<const Extension*> matches;

		std::string lowerQuery = query;
		std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

		for (const auto& ext : allExtensions) {
			std::string name = ext->GetName();
			std::string desc = ext->GetDescription();
			std::string author = ext->GetAuthor();

			std::transform(name.begin(), name.end(), name.begin(), ::tolower);
			std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
			std::transform(author.begin(), author.end(), author.begin(), ::tolower);

			if (name.find(lowerQuery) != std::string::npos ||
				desc.find(lowerQuery) != std::string::npos ||
				author.find(lowerQuery) != std::string::npos) {
				matches.push_back(ext);
			}
		}

		if (matches.empty()) {
			PLG_WARN(std::format("{} No extensions found matching '{}'", Icons.Missing, query).c_str());
			return;
		}

		PLG_INFO(std::format("SEARCH RESULTS: Found {} match{} for '{}'",
			matches.size(), matches.size() > 1 ? "es" : "", query).c_str());
		PLG_INFO(std::string(80, '-').c_str());

		for (const auto& ext : matches) {
			auto [symbol, color] = GetStateInfo(ext->GetState());
			std::string line = std::format("{} {} {} {} ({})",
				symbol, ext->GetName(), ext->GetVersionString(),
				ext->IsPlugin() ? "[Plugin]" : "[Module]",
				ext->GetLanguage());
			PLG_LOG(line.c_str(), color);

			if (!ext->GetDescription().empty()) {
				PLG_INFO(std::format("  {}", Truncate(ext->GetDescription(), 70)).c_str());
			}
		}
		PLG_INFO(std::string(80, '-').c_str());
	}
#else
	void SearchExtensions(const Manager& manager, const std::string& query) {
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
            plg::print("{} No extensions found matching '{}'", Colorize(Icons.Missing, Colors::YELLOW), query);
            return;
        }

        plg::print(
            "{}: Found {} match{} for '{}'",
            Colorize("SEARCH RESULTS", Colors::BOLD),
            matches.size(),
            matches.size() > 1 ? "es" : "",
            query
        );
        plg::print(std::string(80, '-'));

        for (const auto& ext : matches) {
            auto [symbol, color] = GetStateInfo(ext->GetState());
            plg::print(
                "{} {} {} {} {}",
                Colorize(symbol, color),
                Colorize(ext->GetName(), Colors::BOLD),
                Colorize(ext->GetVersionString(), Colors::GRAY),
                ext->IsPlugin() ? "[Plugin]" : "[Module]",
                Colorize(std::format("({})", ext->GetLanguage()), Colors::GRAY)
            );

            if (!ext->GetDescription().empty()) {
                plg::print("  {}", Truncate(ext->GetDescription(), 70));
            }
        }
        plg::print(std::string(80, '-'));
    }
#endif

#if 0
	// Compare two extensions
	void CompareExtensions(const Extension* ext1, const Extension* ext2) {
		PLG_INFO(std::string(80, '=').c_str());
		PLG_INFO("EXTENSION COMPARISON");
		PLG_INFO(std::string(80, '=').c_str());

		auto printRow = [](std::string_view label, std::string_view val1, std::string_view val2) {
			bool same = (val1 == val2);
			std::string line = std::format("{:<20} {:<25} {} {:<25}",
				label, val1, same ? "=" : "≠", val2);
			PLG_LOG(line.c_str(), same ? S2Colors::GREEN : S2Colors::YELLOW);
		};

		PLG_INFO(std::format("\n{:<20} {:<25}   {:<25}",
			"", ext1->GetName(), ext2->GetName()).c_str());
		PLG_INFO(std::string(80, '-').c_str());

		printRow("Type:", ext1->IsPlugin() ? "Plugin" : "Module",
				ext2->IsPlugin() ? "Plugin" : "Module");
		printRow("Version:", ext1->GetVersionString(), ext2->GetVersionString());
		printRow("Language:", ext1->GetLanguage(), ext2->GetLanguage());
		printRow("State:", plg::enum_to_string(ext1->GetState()),
				plg::enum_to_string(ext2->GetState()));
		printRow("Author:", ext1->GetAuthor(), ext2->GetAuthor());
		printRow("License:", ext1->GetLicense(), ext2->GetLicense());

		// Performance comparison
		PLG_INFO("\n[Performance]");
		PLG_INFO(std::format("  Load Time:     {:<15} vs {:<15}",
			FormatDuration(ext1->GetOperationTime(ExtensionState::Loaded)),
			FormatDuration(ext2->GetOperationTime(ExtensionState::Loaded))).c_str());
		PLG_INFO(std::format("  Total Time:    {:<15} vs {:<15}",
			FormatDuration(ext1->GetTotalTime()),
			FormatDuration(ext2->GetTotalTime())).c_str());

		PLG_INFO(std::string(80, '=').c_str());
	}
#else
	void CompareExtensions(const Extension* ext1, const Extension* ext2) {
        plg::print(std::string(80, '='));
        plg::print(Colorize("EXTENSION COMPARISON", Colors::BOLD));
        plg::print(std::string(80, '='));

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
        plg::print(std::string(80, '-'));

        printRow("Type:", ext1->IsPlugin() ? "Plugin" : "Module", ext2->IsPlugin() ? "Plugin" : "Module");
        printRow("Version:", ext1->GetVersionString(), ext2->GetVersionString());
        printRow("Language:", ext1->GetLanguage(), ext2->GetLanguage());
        printRow("State:", plg::enum_to_string(ext1->GetState()), plg::enum_to_string(ext2->GetState()));
        printRow("Author:", ext1->GetAuthor(), ext2->GetAuthor());
        printRow("License:", ext1->GetLicense(), ext2->GetLicense());

        // Dependencies comparison
        plg::print(Colorize("\n[Dependencies]", Colors::BOLD));
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
        plg::print(Colorize("\n[Performance]", Colors::BOLD));
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

        plg::print(std::string(80, '='));
    }
#endif

	void ShowHealth(HealthReport& report) {
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

        plg::print(std::string(80, '='));
        plg::print(Colorize("SYSTEM HEALTH CHECK", Colors::BOLD));
        plg::print(std::string(80, '='));

        // Overall score
        plg::print(
            "\n{}: {} {}",
            Colorize("Overall Health Score", Colors::BOLD),
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

        plg::print(std::string(80, '='));
    }

	void ShowDependencyTree(const Manager& manager, const Extension* ext) {
    	plg::print(std::string(80, '='));
    	plg::print("{}: {}", Colorize("DEPENDENCY TREE", Colors::BOLD), ext->GetName());
    	plg::print(std::string(80, '='));
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

    	plg::print(std::string(80, '='));
    }
}

// Main command handler using CLI11
CON_COMMAND_F(plugify, "Plugify control options", FCVAR_NONE) {
	if (!s_context) {
		plg::print("{}: Plugify context not initialized!", Colorize("Error", Colors::RED));
		return;
	}

	// Create CLI app
	CLI::App app{"Plugify Plugin Management System"};
	app.require_subcommand(); // 1 or more
	//app.allow_extras();
	//app.prefix_command();
	app.set_version_flag("-v,--version", [&app]() {
		ShowVersion();
		return "";
	});

	// Subcommands
	auto* help_cmd = app.add_subcommand("help", "Show help");
	auto* version_cmd = app.add_subcommand("version", "Show version information");
	auto* load_cmd = app.add_subcommand("load", "Load plugin manager");
	auto* unload_cmd = app.add_subcommand("unload", "Unload plugin manager");
	auto* reload_cmd = app.add_subcommand("reload", "Reload plugin manager");
	auto* plugins_cmd = app.add_subcommand("plugins", "List plugins");
	auto* modules_cmd = app.add_subcommand("modules", "List modules");
	auto* plugin_cmd = app.add_subcommand("plugin", "Show plugin information");
	auto* module_cmd = app.add_subcommand("module", "Show module information");
	auto* health_cmd = app.add_subcommand("health", "System health check");
	auto* tree_cmd = app.add_subcommand("tree", "Show dependency tree");
	auto* search_cmd = app.add_subcommand("search", "Search extensions");
	auto* compare_cmd = app.add_subcommand("compare", "Compare two extensions");

	// Options for plugins commands
	std::string pluginFilterState;
	std::string pluginFilterLang;
	std::string pluginSortBy = "name";
	bool pluginReverse = false;
	bool pluginShowFailed = false;

	plugins_cmd->add_option("--filter-state", pluginFilterState, "Filter by state (comma-separated: loaded,failed,disabled)");
	plugins_cmd->add_option("--filter-lang", pluginFilterLang, "Filter by language (comma-separated: cpp,python,rust)");
	plugins_cmd->add_option("-s,--sort", pluginSortBy, "Sort by: name, version, state, language, loadtime")
		->check(CLI::IsMember({ "name", "version", "state", "language", "loadtime" }));
	plugins_cmd->add_flag("-r,--reverse,", pluginReverse, "Reverse sort order");
	plugins_cmd->add_flag("-f,--failed", pluginShowFailed, "Show only failed plugins");

	// Options for modules commands
	std::string moduleFilterState;
	std::string moduleFilterLang;
	std::string moduleSortBy = "name";
	bool moduleReverse = false;
	bool moduleShowFailed = false;

	modules_cmd->add_option("--filter-state", moduleFilterState, "Filter by state (comma-separated: loaded,failed,disabled)");
	modules_cmd->add_option("--filter-lang", moduleFilterLang, "Filter by language (comma-separated: cpp,python,rust)");
	modules_cmd->add_option("-s,--sort", moduleSortBy, "Sort by: name, version, state, language, loadtime")
		->check(CLI::IsMember({ "name", "version", "state", "language", "loadtime" }));
	modules_cmd->add_flag("-r,--reverse", moduleReverse, "Reverse sort order");
	modules_cmd->add_flag("-f,--failed", moduleShowFailed, "Show only failed modules");

	// Helper function to parse comma-separated values
    auto parseCsv = [](const std::string& str) -> std::vector<std::string> {
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
    auto parseSortBy = [](std::string_view str) -> SortBy {
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
    auto parseStates = [](const std::vector<std::string>& strs) -> std::vector<ExtensionState> {
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
	
	// Options for plugin/module commands
	std::string plugin_name, module_name, tree_name, search_query;
	std::string compare_ext1, compare_ext2;
	bool use_id = false;
	bool show_details = false;

	plugin_cmd->add_option("name", plugin_name, "Plugin name or ID")->required();
	plugin_cmd->add_flag("-u,--uuid", use_id, "Use ID instead of name");
	plugin_cmd->validate_positionals();

	module_cmd->add_option("name", module_name, "Module name or ID")->required();
	module_cmd->add_flag("-u,--uuid", use_id, "Use ID instead of name");
	module_cmd->validate_positionals();

	plugins_cmd->add_flag("-d,--details", show_details, "Show detailed information");
	modules_cmd->add_flag("-d,--details", show_details, "Show detailed information");

	tree_cmd->add_option("name", tree_name, "Extension name or ID")->required();
	tree_cmd->add_flag("-u,--uuid", use_id, "Use ID instead of name");
	tree_cmd->validate_positionals();

	search_cmd->add_option("query", search_query, "Search query")->required();
	search_cmd->validate_positionals();

	compare_cmd->add_option("e1", compare_ext1, "First extension")->required();
	compare_cmd->add_option("e2", compare_ext2, "Second extension")->required();
	compare_cmd->add_flag("-u,--uuid", use_id, "Use ID instead of name");
	compare_cmd->validate_positionals();

	// Command callbacks
	help_cmd->callback([]() {
		plg::print(Colorize("Plugify Menu", Colors::BOLD));
		plg::print("(c) untrustedmodders");
		plg::print(Colorize("https://github.com/untrustedmodders", Colors::CYAN));
		plg::print("usage: plugify <command> [options] [arguments]");
		plg::print("  help           - Show help");
		plg::print("  version        - Version information");
		plg::print("Plugin Manager commands:");
		plg::print("  load           - Load plugin manager");
		plg::print("  unload         - Unload plugin manager");
		plg::print("  reload         - Reload plugin manager");
		plg::print("  modules        - List running modules");
		plg::print("  plugins        - List running plugins");
		plg::print("  plugin <name>  - Show information about a plugin");
		plg::print("  module <name>  - Show information about a module");
		plg::print("  health         - System health check");
		plg::print("  tree <name>    - Show dependency tree");
		plg::print("  search <query> - Search extensions");
		plg::print("  compare <e1> <e2> - Compare two extensions");
		plg::print("Options:");
		plg::print("  -h, --help     - Show help");
		plg::print("  -u, --uuid     - Use ID instead of name");
		plg::print("  -d, --details  - Show detailed information");
	});

	version_cmd->callback([&]() {
		ShowVersion();
	});

	load_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (manager.IsInitialized()) {
			plg::print("Plugin manager already loaded.");
		} else {
			s_state = PlugifyState::Load;
		}
	});

	unload_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("Plugin manager already unloaded.");
		} else {
			s_state = PlugifyState::Unload;
		}
	});

	reload_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("Plugin manager not loaded.");
		} else {
			s_state = PlugifyState::Reload;
		}
	});

#if 0
	plugins_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			PLG_ERROR("You must load plugin manager before querying any information from it.");
			return;
		}

		auto plugins = manager.GetExtensionsByType(ExtensionType::Plugin);
		ListExtensions(plugins, "plugin", show_details);
	});
#else
	plugins_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		FilterOptions filter;
		if (!pluginFilterState.empty()) {
			filter.states = parseStates(parseCsv(pluginFilterState));
		}
		if (!pluginFilterLang.empty()) {
			filter.languages = parseCsv(pluginFilterLang);
		}
		filter.showOnlyFailed = pluginShowFailed;

		auto plugins = manager.GetExtensionsByType(ExtensionType::Plugin);
		ListPlugins(plugins, show_details, filter, parseSortBy(pluginSortBy), pluginReverse);
	});
#endif

#if 0
	modules_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			PLG_ERROR("You must load plugin manager before querying any information from it.");
			return;
		}

		auto modules = manager.GetExtensionsByType(ExtensionType::Module);
		ListExtensions(modules, "module", show_details);
	});
#else
	modules_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		FilterOptions filter;
		if (!moduleFilterState.empty()) {
			filter.states = parseStates(parseCsv(moduleFilterState));
		}
		if (!moduleFilterLang.empty()) {
			filter.languages = parseCsv(moduleFilterLang);
		}
		filter.showOnlyFailed = moduleShowFailed;

		auto modules = manager.GetExtensionsByType(ExtensionType::Module);
		ListModules(modules, show_details, filter, parseSortBy(moduleSortBy), moduleReverse);
	});
#endif
	plugin_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		auto plugin = use_id ? manager.FindExtension(FormatId(plugin_name)) :
							   manager.FindExtension(plugin_name);
		if (!plugin) {
			plg::print("{}: Plugin {} not found.", Colorize("Error", Colors::RED), plugin_name);
			return;
		}

		if (!plugin->IsPlugin()) {
			plg::print(
			   "{}: '{}' is not a plugin (it's a module).",
			   Colorize("Error", Colors::RED),
			   plugin_name
		   );
			return;
		}

#if 0
		ShowExtensionDetails(plugin, "PLUGIN");
#else
		ShowPlugin(plugin);
#endif
	});

	module_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		auto module = use_id ? manager.FindExtension(FormatId(module_name)) :
							   manager.FindExtension(module_name);
		if (!module) {
			plg::print("{}: Module {} not found.", Colorize("Error", Colors::RED), module_name);
			return;
		}

		if (!module->IsModule()) {
			plg::print(
				"{}: '{}' is not a module (it's a plugin).",
				Colorize("Error", Colors::RED),
				module_name
			);
			return;
		}

#if 0
		ShowExtensionDetails(module, "MODULE");
#else
		ShowModule(module);
#endif
	});

	health_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		auto report = CalculateSystemHealth(manager);

#if 0
		// Determine health status
		Color scoreColor = S2Colors::GREEN;
		std::string status = "HEALTHY";
		if (report.score < 50) {
			scoreColor = S2Colors::RED;
			status = "CRITICAL";
		} else if (report.score < 75) {
			scoreColor = S2Colors::YELLOW;
			status = "WARNING";
		}

		PLG_INFO(std::string(80, '=').c_str());
		PLG_INFO("SYSTEM HEALTH CHECK");
		PLG_INFO(std::string(80, '=').c_str());

		PLG_LOG(std::format("Overall Health Score: {}/100 [{}]",
			report.score, status).c_str(), scoreColor);

		PLG_INFO("\n[Statistics]");
		PLG_INFO(std::format("  Total Extensions:        {}",
			report.statistics["total_extensions"]).c_str());
		PLG_INFO(std::format("  Failed Extensions:       {} {}",
			report.statistics["failed_extensions"],
			report.statistics["failed_extensions"] > 0 ? Icons.Warning : Icons.Ok).c_str());
		PLG_INFO(std::format("  Extensions with Errors:  {} {}",
			report.statistics["extensions_with_errors"],
			report.statistics["extensions_with_errors"] > 0 ? Icons.Warning : Icons.Ok).c_str());
		PLG_INFO(std::format("  Total Warnings:          {}",
			report.statistics["total_warnings"]).c_str());
		PLG_INFO(std::format("  Slow Loading Extensions: {}",
			report.statistics["slow_loading_extensions"]).c_str());

		if (!report.issues.empty()) {
			PLG_INFO("\n[Critical Issues]");
			for (const auto& issue : report.issues) {
				PLG_ERROR(std::format("  {} {}", Icons.Fail, issue).c_str());
			}
		}

		if (!report.warnings.empty()) {
			PLG_INFO("\n[Warnings]");
			for (const auto& warning : report.warnings) {
				PLG_WARN(std::format("  {} {}", Icons.Warning, warning).c_str());
			}
		}

		PLG_INFO("\n[Recommendations]");
		if (report.score == 100) {
			PLG_SUCCESS(std::format("  {} System is running optimally!", Icons.Ok).c_str());
		} else {
			if (report.statistics["failed_extensions"] > 0) {
				PLG_INFO("  • Fix or remove failed extensions");
			}
			if (report.statistics["extensions_with_errors"] > 0) {
				PLG_INFO("  • Review and resolve extension errors");
			}
			if (report.statistics["slow_loading_extensions"] > 0) {
				PLG_INFO("  • Investigate slow-loading extensions for optimization");
			}
		}

		PLG_INFO(std::string(80, '=').c_str());
#else
		ShowHealth(report);
#endif
	});

	tree_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		auto ext = use_id ? manager.FindExtension(FormatId(tree_name)) :
						   manager.FindExtension(tree_name);
		if (!ext) {
			plg::print("{}: Extension {} not found.", Colorize("Error", Colors::RED), tree_name);
			return;
		}
#if 0
		PLG_INFO(std::string(80, '=').c_str());
		PLG_INFO(std::format("DEPENDENCY TREE: {}", ext->GetName()).c_str());
		PLG_INFO(std::string(80, '=').c_str());

		PrintDependencyTree(ext, manager);

		// Reverse dependencies
		PLG_INFO("\n[Reverse Dependencies]");
		PLG_INFO("Extensions that depend on this:");

		bool found = false;
		for (const auto& other : manager.GetExtensions()) {
			for (const auto& dep : other->GetDependencies()) {
				if (dep.GetName() == ext->GetName()) {
					PLG_INFO(std::format("  • {} {}",
						other->GetName(),
						dep.IsOptional() ? "[optional]" : "").c_str());
					found = true;
				}
			}
		}

		if (!found) {
			PLG_INFO("  None");
		}

		PLG_INFO(std::string(80, '=').c_str());
#else
		ShowDependencyTree(manager, ext);
#endif
	});

	search_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		SearchExtensions(manager, search_query);
	});

	compare_cmd->callback([&]() {
		auto& manager = s_context->GetManager();
		if (!manager.IsInitialized()) {
			plg::print("{}: You must load plugin manager before querying any information from it.", Colorize("Error", Colors::RED));
			return;
		}

		auto ext1 = use_id ? manager.FindExtension(FormatId(compare_ext1)) :
						    manager.FindExtension(compare_ext1);
		auto ext2 = use_id ? manager.FindExtension(FormatId(compare_ext2)) :
						    manager.FindExtension(compare_ext2);

		if (!ext1) {
			plg::print("{}: Extension {} not found.", Colorize("Error", Colors::RED), compare_ext1);
			return;
		}
		if (!ext2) {
			plg::print("{}: Extension {} not found.", Colorize("Error", Colors::RED), compare_ext2);
			return;
		}

		CompareExtensions(ext1, ext2);
	});

	// Parse arguments
	try {
		app.parse(args.ArgC(), args.ArgV());
	} catch (const CLI::ParseError& e) {
		if (e.get_exit_code() != 0) {
			plg::print("{}: {}", Colorize("Error", Colors::RED), e.what());
			plg::print("usage: plugify <command> [options] [arguments]");
			plg::print("Type '{}' for available commands.", Colorize("plugify help", Colors::BRIGHT_RED));
		}
	}
}

// Alternative shorter command
static ConCommand plg_command("plg", plugify_callback, "Plugify control options", 0);

using ServerGamePostSimulateFn = void (*)(IGameSystem*, const EventServerGamePostSimulate_t&);
ServerGamePostSimulateFn _ServerGamePostSimulate;

void ServerGamePostSimulate(IGameSystem* pThis, const EventServerGamePostSimulate_t& msg) {
	_ServerGamePostSimulate(pThis, msg);

	s_context->Update();

	switch (s_state) {
		case PlugifyState::Load: {
			auto& manager = s_context->GetManager();
			if (auto initResult = manager.Initialize()) {
				plg::print("Plugin manager was loaded.");
			} else {
				plg::print("{}: {}", Colorize("Error", Colors::RED), initResult.error());
			}
			break;
		}
		case PlugifyState::Unload: {
			auto& manager = s_context->GetManager();
			manager.Terminate();
			plg::print("Plugin manager was unloaded.");
			break;
		}
		case PlugifyState::Reload: {
			auto& manager = s_context->GetManager();
			manager.Terminate();
			if (auto initResult = manager.Initialize()) {
				plg::print("Plugin manager was reloaded.");
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

void InitializePlugify(CAppSystemDict* pThis) {
	if (s_listener) {
		LoggingSystem_PushLoggingState(false, false);
		LoggingSystem_RegisterLoggingListener(s_listener.get());
	}

	constexpr std::string_view interfaceName = CVAR_INTERFACE_VERSION;

	for (const auto& system: pThis->m_Systems) {
		if (system.m_pInterfaceName == interfaceName) {
			g_pCVar = dynamic_cast<ICvar*>(system.m_pSystem);
			break;
		}
	}

	DynLibUtils::CModule server("server");
	if (server) {
		auto table = server.GetVirtualTableByName("CLightQueryGameSystem");
		DynLibUtils::CVirtualTable vtable(table);
		DynLibUtils::CVTFHookAuto<&IGameSystem::ServerGamePostSimulate> hook;
		hook.Hook(vtable, &ServerGamePostSimulate);
		_ServerGamePostSimulate = hook.GetOrigin();
	}

	ConVar_Register(FCVAR_RELEASE | FCVAR_SERVER_CAN_EXECUTE | FCVAR_GAMEDLL);

	std::filesystem::path baseDir(Plat_GetGameDirectory());
	baseDir /= PLUGIFY_GAME_NAME "/addons/plugify/";

	auto plugify = Plugify::CreateBuilder()
		.WithLogger(s_logger)
		.WithBaseDir(std::move(baseDir))
		.Build();

	if (!plugify) {
		plg::print("{}: {}", Colorize("Error", Colors::RED), plugify.error());
		return;
	}

	s_context = *plugify;

	if (auto result = s_context->Initialize()) {
		auto& manager = s_context->GetManager();
		if (auto initResult = manager.Initialize()) {
			plg::print("Plugin manager was reloaded.");
		} else {
			plg::print("{}: {}", Colorize("Error", Colors::RED), initResult.error());
		}
	} else {
		plg::print("{}: {}", Colorize("Error", Colors::RED), result.error());
	}
}

using OnAppSystemLoadedFn = void (*)(CAppSystemDict*);
OnAppSystemLoadedFn _OnAppSystemLoaded;
std::set<std::string> g_loadList;

void OnAppSystemLoaded(CAppSystemDict* pThis) {
	_OnAppSystemLoaded(pThis);

	if (s_context)
		return;

	constexpr std::string_view moduleName = PLUGIFY_GAME_START;

	for (const auto& module: pThis->m_Modules) {
		if (module.m_pModuleName) {
			auto [_, result] = g_loadList.insert(module.m_pModuleName);
			if (result) {
				if (module.m_pModuleName == moduleName) {
					InitializePlugify(pThis);
				}
			}
		}
	}
}

class FileLoggingListener final : public ILoggingListener {
public:
    static Result<std::unique_ptr<FileLoggingListener>> Create(const std::filesystem::path& filename, bool async = true) {
        std::error_code ec;
        std::filesystem::create_directories(filename.parent_path(), ec);

        std::ofstream file(filename, std::ios::app);
        if (!file.is_open()) {
            return MakeError("Failed to open log file: {} - {}", plg::as_string(filename), std::strerror(errno));
        }

        return std::make_unique<FileLoggingListener>(std::move(file), async);
    }

    FileLoggingListener(std::ofstream&& file, bool async = true)
        : _async(async)
        , _file(std::move(file)) {

        if (_async) {
            // Using jthread for automatic joining and stop_token support
        	_worker_thread = std::jthread([this](std::stop_token stop_token) {
				ProcessQueue(std::move(stop_token));
			});
        }
    }

    ~FileLoggingListener() {
        // jthread automatically requests stop and joins
        // No manual stop management needed
    }

    void Log(const LoggingContext_t* pContext, const tchar* pMessage) override {
        if (!pContext || (pContext->m_Flags & LCF_CONSOLE_ONLY) != 0) {
            return;
        }

        // Using string_view and removing trailing newline
        std::string_view message = pMessage;
        if (auto pos = message.find_last_not_of('\n'); pos != std::string_view::npos) {
            message = message.substr(0, pos + 1);
        } else if (message.find_first_not_of('\n') == std::string_view::npos) {
            return; // All newlines
        }

        if (message.empty()) {
            return;
        }

        // C++23 chrono improvements with zoned_time
        auto now = std::chrono::system_clock::now();
        auto seconds = std::chrono::floor<std::chrono::seconds>(now);

        // More robust timezone handling
        std::string formatted_message;
        try {
            std::chrono::zoned_time zt{std::chrono::current_zone(), seconds};
            formatted_message = std::format("[{:%Y%m%d_%H%M%S}] {}", zt, message);
        } catch (const std::exception&) {
            // Fallback to UTC if local timezone fails
            //auto time_t = std::chrono::system_clock::to_time_t(seconds);
            formatted_message = std::format("[{:%Y%m%d_%H%M%S}] {}", std::chrono::utc_clock::from_sys(seconds), message);
        }

        if (_async) {
            std::lock_guard lock(_queue_mutex);
            _message_queue.emplace(std::move(formatted_message));
            _condition.notify_one();
        } else {
            Write(formatted_message);
        }
    }

private:
    const bool _async;
    mutable std::mutex _queue_mutex;
    std::queue<std::string> _message_queue;
    std::condition_variable _condition;
    std::jthread _worker_thread;  // C++23: jthread for automatic management
    mutable std::mutex _file_mutex;
    std::ofstream _file;

    void Write(const std::string& message) {
        std::lock_guard lock(_file_mutex);
        // Using std::print for more efficient output (C++23)
        std::print(_file, "{}\n", message);
        _file.flush(); // Consider removing for better performance
    }

    void ProcessQueue(std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::unique_lock lock(_queue_mutex);

        	// C++23: Using stop_callback for clean shutdown
        	_condition.wait(lock, [this, &stop_token] {
				return !_message_queue.empty() || stop_token.stop_requested();
			});

        	// Process all queued messages
        	while (!_message_queue.empty()) {
                auto message = std::move(_message_queue.front());
                _message_queue.pop();
                lock.unlock();
                Write(message);
                lock.lock();
            }
        }

        // Drain remaining messages before stopping
        std::lock_guard lock(_queue_mutex);
        while (!_message_queue.empty()) {
            Write(_message_queue.front());
            _message_queue.pop();
        }
    }
};

using namespace plugify;
using namespace crashpad;

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

Result<void> InitializeCrashpad(const std::filesystem::path& exeDir, const std::filesystem::path& annotationsPath) {
	auto metadata = ReadJson<Metadata>(exeDir / annotationsPath);
	if (!metadata) {
		return MakeError(std::move(metadata.error()));
	}

	if (!metadata->enabled.value_or(false)) {
		return {};
	}

	base::FilePath handlerApp(exeDir / std::format(PLUGIFY_EXECUTABLE_PREFIX "{}" PLUGIFY_EXECUTABLE_SUFFIX, metadata->handlerApp));
	base::FilePath databaseDir(exeDir / metadata->databaseDir);
	base::FilePath metricsDir(exeDir / metadata->metricsDir);

	std::unique_ptr<CrashReportDatabase> database = CrashReportDatabase::Initialize(databaseDir);

	// File paths of attachments to uploaded with minidump file at crash time - default upload limit is 2MB
	std::vector<base::FilePath> attachments;
	attachments.reserve(metadata->attachments.size());
	for (const auto& attachment : metadata->attachments) {
		attachments.emplace_back(exeDir / attachment);
	}

	// Enable automated crash uploads
	Settings* settings = database->GetSettings();
	settings->SetUploadsEnabled(!metadata->url.empty());

	if (metadata->listen_console.value_or(false)) {
		auto now = std::chrono::system_clock::now();
		auto seconds = std::chrono::floor<std::chrono::seconds>(now);
		std::chrono::zoned_time zt{std::chrono::current_zone(), seconds};
		auto loggingPath = exeDir / metadata->logsDir / std::format("session_{:%Y%m%d_%H%M%S}.log", zt);
		auto listener = FileLoggingListener::Create(loggingPath);
		if (!listener) {
			return MakeError(std::move(listener.error()));
		}
		s_listener = std::move(*listener);

		std::filesystem::path path = "console.log=";
		path += loggingPath;
		attachments.emplace_back(std::move(path));
	}

	// Start crash handler
	auto* client = new CrashpadClient();
	client->StartHandler(
		handlerApp,
		databaseDir,
		metricsDir,
		metadata->url,
		metadata->annotations,
		metadata->arguments,
		metadata->restartable.value_or(false),
		metadata->asynchronous_start.value_or(false),
		attachments);

	return {};
}

std::optional<std::filesystem::path> ExecutablePath() {
	std::string execPath(260, '\0');

	while (true) {
#if PLUGIFY_PLATFORM_WINDOWS
		size_t len = GetModuleFileNameA(nullptr, execPath.data(), static_cast<DWORD>(execPath.length()));
		if (len == 0)
#elif PLUGIFY_PLATFORM_LINUX
		ssize_t len = readlink("/proc/self/exe", execPath.data(), execPath.length());
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

	return std::filesystem::path(execPath).parent_path();
}

int main(int argc, char* argv[]) {
	auto binary_path = ExecutablePath().value_or(std::filesystem::current_path());

	if (is_directory(binary_path) && binary_path.filename() == "game") {
		binary_path /= "bin/" PLUGIFY_BINARY;
	}
	auto engine_path = binary_path / PLUGIFY_LIBRARY_PREFIX "engine2" PLUGIFY_LIBRARY_SUFFIX;
	auto parent_path = binary_path.generic_string();

	if (!plg::is_debugger_present()) {
		if (auto result = InitializeCrashpad(binary_path, "crashpad.jsonc"); !result) {
			std::cerr << "Crashpad error: " << result.error() << std::endl;
		}
	}

#if PLUGIFY_PLATFORM_WINDOWS
	int flags = LOAD_WITH_ALTERED_SEARCH_PATH;
#else
	int flags = 0;
#endif

	DynLibUtils::CModule engine;
	engine.LoadFromPath(plg::as_string(engine_path), flags);
	if (!engine) {
		std::cerr << "Launcher error: " << engine.GetLastError() << std::endl;
		return 1;
	}

	s_logger = std::make_shared<S2Logger>("plugify");
	s_logger->SetLogLevel(Severity::Info);

	auto table = engine.GetVirtualTableByName("CMaterialSystem2AppSystemDict");
	DynLibUtils::CVirtualTable vtable(table);
	DynLibUtils::CVTFHookAuto<&CAppSystemDict::OnAppSystemLoaded> hook;
	hook.Hook(vtable, &OnAppSystemLoaded);
	_OnAppSystemLoaded = hook.GetOrigin();

	using Source2MainFn = int (*)(void* hInstance, void* hPrevInstance, const char* pszCmdLine, int nShowCmd, const char* pszBaseDir, const char* pszGame);
	auto Source2Main = engine.GetFunctionByName("Source2Main").RCast<Source2MainFn>();

	auto command_line = argc > 1 ? plg::join(std::span(argv + 1, argc - 1), " ") : "";
	int res = Source2Main(nullptr, nullptr, command_line.c_str(), 0, parent_path.c_str(), PLUGIFY_GAME_NAME);

	if (s_listener) {
		LoggingSystem_PopLoggingState();
	}

	s_context.reset();
	s_logger.reset();
	s_listener.reset();

	return res;
}
