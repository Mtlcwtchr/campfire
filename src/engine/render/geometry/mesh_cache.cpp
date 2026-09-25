#include "engine/render/geometry/mesh_cache.hpp"

namespace engine {

void MeshCache::begin(std::uint64_t frame) {
    frame_ = frame;
    drawing_.clear();
    uploaded_ = 0;
}

void MeshCache::forget(std::uint64_t keepFrames) {
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        if (it->second.lastUsed + keepFrames >= frame_) {
            ++it;
            continue;
        }
        if (it->second.instances && it->second.instanceBytes > 0 &&
            instancePoolBytes_ + it->second.instanceBytes <= kInstancePoolBudget) {
            instancePoolBytes_ += it->second.instanceBytes;
            instancePool_.push_back({std::move(it->second.instances), it->second.instanceBytes});
        }
        it = meshes_.erase(it);
    }
}

void MeshCache::clear() {
    meshes_.clear();
    instancePool_.clear();
    instancePoolBytes_ = 0;
    drawing_.clear();
}

} // namespace engine
