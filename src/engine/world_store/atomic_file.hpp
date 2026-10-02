#pragma once
// Replacing a file so that nobody ever sees half of it.
//
// Written beside the destination under a temporary name, flushed to the disk,
// then renamed over it. A rename within one directory is atomic, so a reader -
// or the next run after a crash - finds the old file or the new one, never a
// mixture, and the worst a crash leaves behind is a stray temporary, which the
// world root sweeps up the next time it is opened.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::world_store {

// The suffix every temporary carries, so a sweep can recognise them.
inline constexpr std::string_view kTemporarySuffix = ".tmp";

bool writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                     std::string* why = nullptr);
bool writeFileAtomic(const std::filesystem::path& path, std::string_view text, std::string* why = nullptr);
std::optional<std::vector<std::uint8_t>> readFileBytes(const std::filesystem::path& path,
                                                       std::string* why = nullptr);

} // namespace engine::world_store


