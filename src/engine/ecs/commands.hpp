#pragma once
// A worker's write buffer, with no opinion about what a write is.
//
// The engine owns the mechanism - one buffer per worker, never shared, merged
// by the scheduler in a deterministic commit phase - and the game owns the
// vocabulary. A buffer that knew about animal births and building blueprints
// would be an engine that knows what an animal is, and then every game built on
// it inherits somebody else's nouns.

#include <utility>
#include <vector>

namespace engine::ecs {

template <class Command>
class CommandBuffer {
public:
    void push(Command command) { commands_.push_back(std::move(command)); }
    const std::vector<Command>& commands() const { return commands_; }
    std::vector<Command>& commands() { return commands_; }
    void clear() { commands_.clear(); }
    bool empty() const { return commands_.empty(); }
    std::size_t size() const { return commands_.size(); }

private:
    std::vector<Command> commands_;
};

} // namespace engine::ecs
