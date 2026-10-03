#pragma once
// What produces a pose, and the graph that combines them.
//
//   Motion       a pose as a function of normalised time: a keyframed Clip,
//                or a procedural generator (procedural.hpp)
//   Node         something in the graph that advances with time and writes a
//                pose: a Motion played (MotionNode), a BlendSpace1D/2D over
//                several motions kept in step, a StateMachine of nodes
//   Parameters   the named numbers the graph reads: speed, direction,
//                grounded, vertical speed, turn rate...
//
// Blend spaces keep their samples in phase - every sample is sampled at the
// same normalised time, advanced by the weighted cycle length - so a walk
// blending into a run puts the same foot down at the same moment instead of
// shuffling. The state machine cross-fades between whole nodes over a
// transition's duration; the outgoing node keeps playing while it fades.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/animation/pose.hpp"

namespace engine::animation {

class Parameters {
public:
    void set(std::string_view name, float value);
    [[nodiscard]] float get(std::string_view name, float otherwise = 0) const;
    [[nodiscard]] bool flag(std::string_view name) const { return get(name) > 0.5f; }
private:
    std::vector<std::pair<std::string, float>> values_;
};

class Motion {
public:
    virtual ~Motion() = default;
    // Seconds a cycle takes at rate 1 (may depend on the parameters: a gait's
    // cycle is its stride over its speed).
    [[nodiscard]] virtual float duration(const Parameters& p) const = 0;
    [[nodiscard]] virtual bool loops() const { return true; }
    // `phase` in [0, 1): where in the cycle.
    virtual void sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const = 0;
};

// Keyframed: one track a joint (rotation and translation keys, linear), the
// joints found by name, so a clip made for any skeleton with the same naming
// (the UE4 mannequin's) plays on this one. Rotation keys are absolute local
// rotations; with `retarget` the clip's own rest is subtracted and this
// skeleton's added, which is what moves a clip between bodies of different
// proportions without stretching them.
class Clip : public Motion {
public:
    struct Track {
        std::string joint;
        std::vector<float> times;
        std::vector<Quat> rotations;
        std::vector<Vec3> translations;   // empty: the joint's own rest translation
    };
    std::string name;
    float length = 1;
    bool looping = true;
    std::vector<Track> tracks;

    [[nodiscard]] float duration(const Parameters&) const override { return length; }
    [[nodiscard]] bool loops() const override { return looping; }
    void sample(const Skeleton& s, float phase, const Parameters& p, Pose& out) const override;
    // Resolves track names against a skeleton; sampling uses the result.
    void bind(const Skeleton& s);
private:
    std::vector<int> joints_;
};

class Node {
public:
    virtual ~Node() = default;
    virtual void reset() {}
    virtual void update(float seconds, const Parameters& p) = 0;
    virtual void evaluate(const Skeleton& s, const Parameters& p, Pose& out) const = 0;
    // Normalised time of a cyclic node (for syncing what follows it), -1 none.
    [[nodiscard]] virtual float phase() const { return -1; }
    // Starts a cyclic node at a phase (a walk entering a run in step).
    virtual void syncTo(float) {}
    // A non-looping node that has played to its end.
    [[nodiscard]] virtual bool finished() const { return false; }
};

class MotionNode : public Node {
public:
    explicit MotionNode(std::shared_ptr<const Motion> motion, float rate = 1) : motion_(std::move(motion)), rate_(rate) {}
    void reset() override { phase_ = 0; done_ = false; }
    void update(float seconds, const Parameters& p) override;
    void evaluate(const Skeleton& s, const Parameters& p, Pose& out) const override;
    [[nodiscard]] float phase() const override { return motion_ && motion_->loops() ? phase_ : -1; }
    void syncTo(float phase) override { phase_ = phase; }
    [[nodiscard]] bool finished() const override { return done_; }
private:
    std::shared_ptr<const Motion> motion_;
    float rate_ = 1, phase_ = 0;
    bool done_ = false;
};

// Samples along one parameter, kept in phase.
class BlendSpace1D : public Node {
public:
    BlendSpace1D(std::string parameter) : parameter_(std::move(parameter)) {}
    void add(float at, std::shared_ptr<const Motion> motion);
    void reset() override { phase_ = 0; }
    void update(float seconds, const Parameters& p) override;
    void evaluate(const Skeleton& s, const Parameters& p, Pose& out) const override;
    [[nodiscard]] float phase() const override { return phase_; }
    void syncTo(float phase) override { phase_ = phase; }
    // Which samples and how much, at a parameter value (for tests and tools).
    void weights(float value, std::vector<float>& out) const;
    // Optional phase supplied by actual displacement, rather than elapsed
    // time. Other blend spaces retain their authored cycle duration.
    void phaseParameter(std::string name) { phaseParameter_ = std::move(name); }
private:
    std::string parameter_;
    std::string phaseParameter_;
    std::vector<std::pair<float, std::shared_ptr<const Motion>>> samples_;
    float phase_ = 0;
    mutable std::vector<float> weights_, used_;
    mutable std::vector<Pose> poses_;
    mutable std::vector<const Pose*> refs_;
};

// Samples scattered over two parameters, weighted by gradient-band
// interpolation (each sample's influence falls off along the direction to
// every other sample): exact at the samples, no triangulation, any layout.
class BlendSpace2D : public Node {
public:
    BlendSpace2D(std::string x, std::string y) : x_(std::move(x)), y_(std::move(y)) {}
    void add(float x, float y, std::shared_ptr<const Motion> motion);
    void reset() override { phase_ = 0; }
    void update(float seconds, const Parameters& p) override;
    void evaluate(const Skeleton& s, const Parameters& p, Pose& out) const override;
    [[nodiscard]] float phase() const override { return phase_; }
    void syncTo(float phase) override { phase_ = phase; }
    void weights(float x, float y, std::vector<float>& out) const;
private:
    std::string x_, y_;
    struct Sample { float x, y; std::shared_ptr<const Motion> motion; };
    std::vector<Sample> samples_;
    float phase_ = 0;
    mutable std::vector<float> weights_, used_;
    mutable std::vector<Pose> poses_;
    mutable std::vector<const Pose*> refs_;
};

class StateMachine : public Node {
public:
    using Condition = std::function<bool(const Parameters&, const Node& current)>;
    int addState(std::string name, std::shared_ptr<Node> node);
    // From `from` (-1: from any state) to `to` when `when` holds, cross-fading
    // over `seconds`. Earlier transitions win.
    void addTransition(int from, int to, float seconds, Condition when);
    void start(int state);
    void reset() override;
    void update(float seconds, const Parameters& p) override;
    void evaluate(const Skeleton& s, const Parameters& p, Pose& out) const override;
    [[nodiscard]] int current() const { return current_; }
    [[nodiscard]] const std::string& currentName() const {
        static const std::string empty;
        return states_.empty() ? empty : states_[std::size_t(current_)].name;
    }
    [[nodiscard]] float phase() const override;
    // Called with the state entered, for events (footstep sounds, effects).
    std::function<void(int from, int to)> onEnter;
private:
    struct State { std::string name; std::shared_ptr<Node> node; float time = 0; };
    struct Transition { int from, to; float seconds; Condition when; };
    std::vector<State> states_;
    std::vector<Transition> transitions_;
    int current_ = 0, previous_ = -1;
    float fade_ = 1, fadeSeconds_ = 0;
    mutable Pose scratch_;
    mutable Pose lastOutput_;
    Pose frozenSource_;
    bool interrupted_ = false;
};

} // namespace engine::animation
