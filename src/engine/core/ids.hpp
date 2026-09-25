#pragma once
// Strongly typed handles. Mixing up a PersonId and an ItemStackId is the kind of
// bug that only shows up 40 000 ticks into a run, so the compiler catches it.

#include <cstdint>
#include <functional>
#include <limits>

namespace core {

template <typename Tag>
struct Handle {
    using Value = std::uint32_t;
    static constexpr Value kInvalid = std::numeric_limits<Value>::max();

    Value value = kInvalid;

    constexpr bool valid() const { return value != kInvalid; }
    constexpr explicit operator bool() const { return valid(); }

    friend constexpr bool operator==(Handle a, Handle b) { return a.value == b.value; }
    friend constexpr bool operator!=(Handle a, Handle b) { return a.value != b.value; }
    friend constexpr bool operator<(Handle a, Handle b) { return a.value < b.value; }
};

struct PersonTag {};
struct SettlementTag {};
struct ItemStackTag {};
struct BuildingTag {};
struct ResourceNodeTag {};
struct ZoneTag {};
struct JobTag {};
struct HouseholdTag {};
struct FamilyTag {};
struct AnimalTag {};

using PersonId = Handle<PersonTag>;
using SettlementId = Handle<SettlementTag>;
using ItemStackId = Handle<ItemStackTag>;
using BuildingId = Handle<BuildingTag>;
using ResourceNodeId = Handle<ResourceNodeTag>;
using ZoneId = Handle<ZoneTag>;
using JobId = Handle<JobTag>;
using HouseholdId = Handle<HouseholdTag>;
using AnimalId = Handle<AnimalTag>;

// Index into a content table (item defs, recipes, building defs, ...). Content is
// loaded once at startup and never mutated, so a plain index is safe and cheap.
struct DefTag {};
using DefId = Handle<DefTag>;

} // namespace core

namespace std {
template <typename Tag>
struct hash<core::Handle<Tag>> {
    size_t operator()(core::Handle<Tag> h) const noexcept { return hash<uint32_t>{}(h.value); }
};
} // namespace std
