#include "engine/world_store/atomic_file.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace engine::world_store {
namespace {

void fail(std::string* why, std::string what) {
    if (why) *why = std::move(what);
}

// Unique per process and per call, so two savers writing two files of one
// directory at once never share a temporary.
std::filesystem::path temporaryFor(const std::filesystem::path& path) {
    static std::atomic<std::uint64_t> counter{0};
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto name = path.filename().string();
    name += "." + std::to_string(thread % 1000003) + "." + std::to_string(counter.fetch_add(1));
    name += kTemporarySuffix;
    return path.parent_path() / name;
}

} // namespace

bool writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string* why) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { fail(why, "cannot create " + path.parent_path().string() + ": " + ec.message()); return false; }
    const auto staging = temporaryFor(path);
    std::FILE* out = std::fopen(staging.string().c_str(), "wb");
    if (!out) { fail(why, "cannot write " + staging.string()); return false; }
    bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    ok = std::fflush(out) == 0 && ok;
    // Flushed to the disk and not only to the operating system: the rename
    // below is what makes the new file visible, and it must not be able to
    // outrun the bytes it points at.
#if defined(_WIN32)
    ok = _commit(_fileno(out)) == 0 && ok;
#else
    ok = ::fsync(fileno(out)) == 0 && ok;
#endif
    ok = std::fclose(out) == 0 && ok;
    if (!ok) {
        std::filesystem::remove(staging, ec);
        fail(why, "short write to " + staging.string());
        return false;
    }
    std::filesystem::rename(staging, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        fail(why, "cannot replace " + path.string() + ": " + ec.message());
        return false;
    }
    return true;
}

bool writeFileAtomic(const std::filesystem::path& path, std::string_view text, std::string* why) {
    return writeFileAtomic(path, std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), why);
}

std::optional<std::vector<std::uint8_t>> readFileBytes(const std::filesystem::path& path, std::string* why) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { fail(why, "missing " + path.string()); return std::nullopt; }
    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in || (!bytes.empty() && !in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))) {
        fail(why, "cannot read " + path.string());
        return std::nullopt;
    }
    return bytes;
}

} // namespace engine::world_store


