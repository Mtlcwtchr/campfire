#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace engine {
struct IndexRange { std::uint32_t first = 0, count = 0; };

// Immutable representation hierarchy, independent of the GPU and asset type.
// Children replace their parent TOGETHER. Each node's range is a complete
// representation of its region, not an additional overlay on its parent.
class SmartMesh {
public:
    enum class ErrorMetric { Conservative, EstimatedSurfaceDistance };
    struct Node {
        IndexRange indices;
        double error = 0; // object metres; nondecreasing towards the root
        std::vector<std::uint32_t> children;
    };
    struct Cut { std::vector<std::uint32_t> nodes; bool complete = true; };
    SmartMesh(std::vector<Node> nodes, std::uint32_t indexCount,
              ErrorMetric metric = ErrorMetric::Conservative)
        : nodes_(std::move(nodes)), metric_(metric) {
        if (nodes_.empty()) throw std::invalid_argument("empty smart mesh hierarchy");
        std::vector<unsigned> parents(nodes_.size());
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const auto& n = nodes_[i];
            if (!std::isfinite(n.error) || n.error < 0 || n.indices.count == 0 ||
                n.indices.count % 3 || std::uint64_t(n.indices.first) + n.indices.count > indexCount)
                throw std::invalid_argument("invalid smart mesh range/error");
            for (auto child : n.children) {
                if (child >= nodes_.size() || child == i || ++parents[child] != 1 ||
                    nodes_[child].error > n.error)
                    throw std::invalid_argument("invalid smart mesh replacement family");
            }
        }
        for (std::uint32_t i = 0; i < nodes_.size(); ++i) if (!parents[i]) roots_.push_back(i);
        std::vector<bool> visited(nodes_.size());
        std::vector<unsigned> depth(nodes_.size());
        std::vector<std::uint32_t> pending = roots_;
        while (!pending.empty()) {
            const auto i = pending.back(); pending.pop_back();
            if (visited[i]) throw std::invalid_argument("cyclic smart mesh hierarchy");
            visited[i] = true;
            for (auto child : nodes_[i].children) {
                depth[child] = depth[i] + 1;
                if (depth[child] > 64) throw std::invalid_argument("smart mesh hierarchy exceeds depth limit");
                pending.push_back(child);
            }
        }
        for (bool seen : visited) if (!seen) throw std::invalid_argument("unreachable smart mesh nodes");
        chain_ = roots_.size() == 1;
        for (const auto& node : nodes_) chain_ = chain_ && node.children.size() <= 1;
    }
    // Adapter for generated/imported whole-mesh chains sharing one vertex buffer.
    // It is a degenerate hierarchy, not a claim that the asset contains a DAG.
    static SmartMesh chain(std::span<const IndexRange> ranges, std::span<const float> errors,
                           std::uint32_t indexCount, ErrorMetric metric) {
        if (ranges.size() != errors.size()) throw std::invalid_argument("smart mesh chain mismatch");
        std::vector<Node> nodes;
        for (std::uint32_t i = 0; i < ranges.size(); ++i)
            nodes.push_back({ranges[i], errors[i], i ? std::vector<std::uint32_t>{i - 1} : std::vector<std::uint32_t>{}});
        return SmartMesh(std::move(nodes), indexCount, metric);
    }
    Cut select(double pixelsPerMetre, double pixelError,
               std::span<const std::uint8_t> resident = {}) const {
        if (!std::isfinite(pixelsPerMetre) || pixelsPerMetre < 0 ||
            !std::isfinite(pixelError) || pixelError < 0 ||
            (!resident.empty() && resident.size() != nodes_.size()))
            throw std::invalid_argument("invalid smart mesh selection");
        Cut cut;
        const auto available = [&](auto i) { return resident.empty() || resident[i] != 0; };
        const auto visit = [&](auto&& self, std::uint32_t i) -> bool {
            const auto& node = nodes_[i];
            if (available(i) && (node.children.empty() || node.error * pixelsPerMetre <= pixelError)) {
                cut.nodes.push_back(i); return true;
            }
            const auto start = cut.nodes.size();
            bool complete = !node.children.empty();
            for (auto child : node.children) complete = self(self, child) && complete;
            if (complete) return true;
            cut.nodes.resize(start); // never publish half a replacement family
            if (!available(i)) return false;
            cut.nodes.push_back(i); return true;
        };
        for (auto root : roots_) if (!visit(visit, root)) cut.complete = false;
        if (!cut.complete) cut.nodes.clear();
        return cut;
    }
    const Node& node(std::uint32_t i) const { return nodes_.at(i); }
    std::uint32_t selectChain(double pixelsPerMetre, double pixelError) const {
        if (!chain_ || !std::isfinite(pixelsPerMetre) || pixelsPerMetre < 0 ||
            !std::isfinite(pixelError) || pixelError < 0)
            throw std::invalid_argument("invalid smart mesh chain selection");
        auto i = roots_.front();
        while (!nodes_[i].children.empty() && nodes_[i].error * pixelsPerMetre > pixelError)
            i = nodes_[i].children.front();
        return i;
    }
    std::size_t size() const { return nodes_.size(); }
    ErrorMetric errorMetric() const { return metric_; }
private:
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> roots_;
    ErrorMetric metric_;
    bool chain_ = false;
};
} // namespace engine
