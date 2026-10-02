#pragma once
// A value per macro cell of a world, kept only where it is not the sea's.
//
// A world is mostly sea, and the sea has nothing to say about most of what a
// cell carries - its rock, its rivers, its soil, its biome. Held as a dense
// array per field, the fifty-odd fields of WorldMapData were a hundred and
// ninety bytes a cell whatever the cell was: a gigabyte for a world eight
// hundred by two thousand kilometres, most of it the same "open sea" written
// out millions of times.
//
// So a field is held in chunks of kChunk cells along a row (a region is 256
// cells wide, so a chunk never straddles two regions), and a chunk exists only
// once something is written into it. A chunk that does not exist reads as the
// field's fill - what the sea holds. Chunks are shared between copies of the
// world and unshared by the first write into one (copy on write): a snapshot,
// a save or a composed world copies nothing it does not change.
//
// The interface is a vector's, as far as the code that reads these needs one:
// a size, indexing, reading iteration, assignment from and conversion to a
// std::vector. Writing is by index only; there is no writing iterator,
// because a pass that walks a whole field writing would make every chunk.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace generation {

template <class T>
class CellField {
public:
    static constexpr std::size_t kChunk = 256;
    using value_type = T;
    using Chunk = std::array<T, kChunk>;

    CellField() = default;
    CellField(std::size_t count, const T& fill) { assign(count, fill); }
    CellField(const std::vector<T>& dense) { *this = dense; }   // NOLINT: a field is a vector to its readers

    // --- a vector's shape ---------------------------------------------------
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    // Every cell `fill`, and nothing held: what a field is before anything
    // but the sea is written into it.
    void assign(std::size_t count, const T& fill) {
        size_ = count;
        fill_ = fill;
        chunks_.assign((count + kChunk - 1) / kChunk, nullptr);
    }
    void resize(std::size_t count, const T& fill = T{}) {
        if (count == size_) return;
        if (size_ == 0) { assign(count, fill); return; }
        std::vector<T> copy = dense();
        copy.resize(count, fill);
        *this = copy;
    }
    void clear() { size_ = 0; chunks_.clear(); }

    const T& operator[](std::size_t i) const {
        const auto& chunk = chunks_[i / kChunk];
        return chunk ? (*chunk)[i % kChunk] : fill_;
    }
    T& operator[](std::size_t i) { return (*writable(i / kChunk))[i % kChunk]; }
    // A write that does not make a chunk to say what the chunk already says.
    void set(std::size_t i, const T& value) {
        auto& chunk = chunks_[i / kChunk];
        if (!chunk && value == fill_) return;
        (*writable(i / kChunk))[i % kChunk] = value;
    }

    CellField& operator=(const std::vector<T>& dense) {
        assign(dense.size(), T{});
        for (std::size_t c = 0; c < chunks_.size(); ++c) {
            auto chunk = std::make_shared<Chunk>();
            for (std::size_t k = 0; k < kChunk; ++k) {
                const std::size_t i = c * kChunk + k;
                (*chunk)[k] = i < dense.size() ? dense[i] : fill_;
            }
            chunks_[c] = std::move(chunk);
        }
        return *this;
    }
    // Explicit: a copy of every cell of the world, and it used to happen
    // unseen wherever a field was passed to something taking a vector - once
    // per call, per sample, sixty-seven megabytes a time on a world 2000 km a
    // side. Where a whole dense copy is really wanted, say so (dense()).
    explicit operator std::vector<T>() const {
        return dense();
    }
    std::vector<T> dense() const {
        std::vector<T> dense;
        dense.reserve(size_);
        for (std::size_t i = 0; i < size_; ++i) dense.push_back((*this)[i]);
        return dense;
    }

    // Reading only, cell by cell, fill included.
    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;
        const_iterator() = default;
        const_iterator(const CellField* field, std::size_t at) : field_(field), at_(at) {}
        reference operator*() const { return (*field_)[at_]; }
        pointer operator->() const { return &(*field_)[at_]; }
        reference operator[](difference_type n) const { return (*field_)[std::size_t(difference_type(at_) + n)]; }
        const_iterator& operator++() { ++at_; return *this; }
        const_iterator operator++(int) { auto was = *this; ++at_; return was; }
        const_iterator& operator--() { --at_; return *this; }
        const_iterator operator--(int) { auto was = *this; --at_; return was; }
        const_iterator& operator+=(difference_type n) { at_ = std::size_t(difference_type(at_) + n); return *this; }
        const_iterator& operator-=(difference_type n) { at_ = std::size_t(difference_type(at_) - n); return *this; }
        friend const_iterator operator+(const_iterator a, difference_type n) { return a += n; }
        friend const_iterator operator+(difference_type n, const_iterator a) { return a += n; }
        friend const_iterator operator-(const_iterator a, difference_type n) { return a -= n; }
        friend difference_type operator-(const const_iterator& a, const const_iterator& b) {
            return difference_type(a.at_) - difference_type(b.at_);
        }
        friend bool operator==(const const_iterator& a, const const_iterator& b) { return a.at_ == b.at_; }
        friend bool operator!=(const const_iterator& a, const const_iterator& b) { return a.at_ != b.at_; }
        friend bool operator<(const const_iterator& a, const const_iterator& b) { return a.at_ < b.at_; }
        friend bool operator>(const const_iterator& a, const const_iterator& b) { return a.at_ > b.at_; }
        friend bool operator<=(const const_iterator& a, const const_iterator& b) { return a.at_ <= b.at_; }
        friend bool operator>=(const const_iterator& a, const const_iterator& b) { return a.at_ >= b.at_; }
    private:
        const CellField* field_ = nullptr;
        std::size_t at_ = 0;
    };
    const_iterator begin() const { return {this, 0}; }
    const_iterator end() const { return {this, size_}; }

    bool operator==(const CellField& other) const {
        if (size_ != other.size_) return false;
        for (std::size_t i = 0; i < size_; ++i)
            if (!((*this)[i] == other[i])) return false;
        return true;
    }

    // A dense array as a field whose chunks exist only where something other
    // than `fill` is: what a pass that works densely hands on to be kept.
    static CellField sparse(const std::vector<T>& dense, const T& fill) {
        CellField field(dense.size(), fill);
        for (std::size_t c = 0; c < field.chunks_.size(); ++c) {
            const std::size_t from = c * kChunk, to = std::min(dense.size(), from + kChunk);
            bool held = false;
            for (std::size_t i = from; i < to && !held; ++i) held = !(dense[i] == fill);
            if (!held) continue;
            auto chunk = std::make_shared<Chunk>();
            chunk->fill(fill);
            std::copy(dense.begin() + std::ptrdiff_t(from), dense.begin() + std::ptrdiff_t(to), chunk->begin());
            field.chunks_[c] = std::move(chunk);
        }
        return field;
    }
    // Chunks that hold what `other`'s do are shared with it: two planes that
    // agree over most of the world cost one where they agree.
    void shareEqualChunks(const CellField& other) {
        if (other.size_ != size_ || !(other.fill_ == fill_)) return;
        for (std::size_t c = 0; c < chunks_.size(); ++c)
            if (chunks_[c] && other.chunks_[c] && chunks_[c] != other.chunks_[c] && *chunks_[c] == *other.chunks_[c])
                chunks_[c] = other.chunks_[c];
    }
    // The lowest and highest value held, the fill included when any of the
    // field is not held: a range without walking the sea.
    std::pair<T, T> range() const {
        std::pair<T, T> r{fill_, fill_};
        bool any = false, absent = false;
        for (const auto& chunk : chunks_) {
            if (!chunk) { absent = true; continue; }
            for (const auto& v : *chunk) {
                if (!any) { r = {v, v}; any = true; }
                if (v < r.first) r.first = v;
                if (r.second < v) r.second = v;
            }
        }
        if (absent || !any) {
            if (!any) return {fill_, fill_};
            if (fill_ < r.first) r.first = fill_;
            if (r.second < fill_) r.second = fill_;
        }
        return r;
    }

    // --- what it holds -----------------------------------------------------
    const T& fill() const { return fill_; }
    // Chunk by chunk, in order: `held(index, chunk)` for a chunk that exists,
    // `absent(index)` for one that reads as the fill. What a pass that only
    // cares about the land walks instead of every cell - a hash, a count -
    // so its cost follows what is held, not the size of the world.
    template <class Held, class Absent>
    void visitChunks(Held&& held, Absent&& absent) const {
        for (std::size_t c = 0; c < chunks_.size(); ++c) {
            if (chunks_[c]) held(c, static_cast<const Chunk&>(*chunks_[c]));
            else absent(c);
        }
    }
    std::size_t chunkCount() const { return chunks_.size(); }
    std::size_t chunksHeld() const {
        std::size_t n = 0;
        for (const auto& c : chunks_) n += c ? 1 : 0;
        return n;
    }
    std::size_t bytes() const { return chunksHeld() * sizeof(Chunk) + chunks_.size() * sizeof(chunks_[0]); }

private:
    Chunk* writable(std::size_t c) {
        auto& chunk = chunks_[c];
        if (!chunk) {
            chunk = std::make_shared<Chunk>();
            chunk->fill(fill_);
        } else if (chunk.use_count() > 1) {
            chunk = std::make_shared<Chunk>(*chunk);
        }
        return chunk.get();
    }

    std::size_t size_ = 0;
    T fill_{};
    std::vector<std::shared_ptr<Chunk>> chunks_;
};

// A dense vector of whatever holds cells, for the few places (a whole-map
// file, a test) that really want every cell written out.
template <class T> std::vector<T> denseOf(const CellField<T>& field) { return field.dense(); }
template <class T> const std::vector<T>& denseOf(const std::vector<T>& values) { return values; }

} // namespace generation
