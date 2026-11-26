#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace drop {
namespace fs = std::filesystem;
using Args = std::vector<std::string>;
inline constexpr std::uintmax_t limit = 20000000;
struct Tools { fs::path ffmpeg, ffprobe; };
struct Hooks {
    std::atomic_bool& cancel;
    std::function<void(std::string, int)> progress;
    std::function<bool(std::string)> approve;
    void check() const;
};
struct Cancelled : std::exception {};
struct Declined : std::exception {};
struct Result {
    enum State { Saved, Skipped, Canceled, Failed } state = Failed;
    fs::path path;
    std::uintmax_t before = 0, after = 0;
    std::string message;
};
struct RunResult { int code; std::string diagnostic; };
using Line = std::function<void(const std::string&)>;
RunResult run(const fs::path& program, const Args& args, const Hooks& hooks, const Line& line);
Result compress(const fs::path& source, bool keepWav, const Tools& tools, const Hooks& hooks);
std::string sizeText(std::uintmax_t bytes);
#ifdef _WIN32
std::wstring wide(const std::string&);
std::string utf8(const std::wstring&);
#endif
}
