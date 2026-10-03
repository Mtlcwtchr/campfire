#include "engine/animation/motion.hpp"

#include <algorithm>
#include <cmath>

namespace engine::animation {

void Parameters::set(std::string_view name, float value) {
    for (auto& [n, v] : values_)
        if (n == name) { v = value; return; }
    values_.emplace_back(std::string(name), value);
}
float Parameters::get(std::string_view name, float otherwise) const {
    for (const auto& [n, v] : values_)
        if (n == name) return v;
    return otherwise;
}

void Clip::bind(const Skeleton& s) {
    joints_.clear();
    for (const auto& t : tracks) joints_.push_back(s.find(t.joint));
}

void Clip::sample(const Skeleton& s, float phase, const Parameters&, Pose& out) const {
    if (out.local.size() != s.size()) out = Pose::rest(s);
    const float time = std::clamp(phase, 0.0f, 1.0f) * length;
    for (std::size_t i = 0; i < tracks.size() && i < joints_.size(); ++i) {
        const int j = joints_[i];
        const auto& t = tracks[i];
        if (j < 0 || t.times.empty()) continue;
        const auto it = std::upper_bound(t.times.begin(), t.times.end(), time);
        const std::size_t b = std::min<std::size_t>(std::size_t(it - t.times.begin()), t.times.size() - 1);
        const std::size_t a = b == 0 ? 0 : b - 1;
        const float span = t.times[b] - t.times[a];
        const float k = span > 1e-6f ? std::clamp((time - t.times[a]) / span, 0.0f, 1.0f) : 0.0f;
        auto& local = out.local[std::size_t(j)];
        if (a < t.rotations.size() && b < t.rotations.size()) local.rotation = nlerp(t.rotations[a], t.rotations[b], k);
        if (a < t.translations.size() && b < t.translations.size())
            local.translation = lerp(t.translations[a], t.translations[b], k);
    }
}

namespace {
float advance(float phase, float seconds, float duration, bool loops, bool* done = nullptr) {
    if (!(duration > 1e-4f)) return phase;
    phase += seconds / duration;
    if (loops) return phase - std::floor(phase);
    if (phase >= 1) { if (done) *done = true; return 1; }
    return phase;
}
} // namespace

void MotionNode::update(float seconds, const Parameters& p) {
    if (!motion_) return;
    phase_ = advance(phase_, seconds * rate_, motion_->duration(p), motion_->loops(), &done_);
}
void MotionNode::evaluate(const Skeleton& s, const Parameters& p, Pose& out) const {
    if (motion_) motion_->sample(s, phase_, p, out);
}

void BlendSpace1D::add(float at, std::shared_ptr<const Motion> motion) {
    samples_.emplace_back(at, std::move(motion));
    std::sort(samples_.begin(), samples_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
}
void BlendSpace1D::weights(float value, std::vector<float>& out) const {
    out.assign(samples_.size(), 0.0f);
    if (samples_.empty()) return;
    if (value <= samples_.front().first) { out.front() = 1; return; }
    if (value >= samples_.back().first) { out.back() = 1; return; }
    for (std::size_t i = 0; i + 1 < samples_.size(); ++i) {
        const float a = samples_[i].first, b = samples_[i + 1].first;
        if (value >= a && value <= b) {
            const float t = b > a ? (value - a) / (b - a) : 0.0f;
            out[i] = 1 - t; out[i + 1] = t;
            return;
        }
    }
}
void BlendSpace1D::update(float seconds, const Parameters& p) {
    const float driven = phaseParameter_.empty() ? -1 : p.get(phaseParameter_, -1);
    if (std::isfinite(driven) && driven >= 0) {
        phase_ = driven - std::floor(driven);
        return;
    }
    auto& w = weights_;
    weights(p.get(parameter_), w);
    // The cycle length is the weighted one: the blended stride.
    float duration = 0;
    for (std::size_t i = 0; i < samples_.size(); ++i) duration += w[i] * samples_[i].second->duration(p);
    phase_ = advance(phase_, seconds, duration, true);
}
void BlendSpace1D::evaluate(const Skeleton& s, const Parameters& p, Pose& out) const {
    auto& w = weights_;
    weights(p.get(parameter_), w);
    poses_.resize(samples_.size());
    refs_.clear(); used_.clear();
    refs_.reserve(samples_.size()); used_.reserve(samples_.size());
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        if (w[i] <= 1e-4f) continue;
        poses_[i].reset(s);
        samples_[i].second->sample(s, phase_, p, poses_[i]);
        refs_.push_back(&poses_[i]); used_.push_back(w[i]);
    }
    blendMany(refs_, used_, out);
}

void BlendSpace2D::add(float x, float y, std::shared_ptr<const Motion> motion) {
    samples_.push_back({x, y, std::move(motion)});
}
void BlendSpace2D::weights(float x, float y, std::vector<float>& out) const {
    out.assign(samples_.size(), 0.0f);
    float total = 0;
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        float w = 1;
        const float px = x - samples_[i].x, py = y - samples_[i].y;
        for (std::size_t j = 0; j < samples_.size(); ++j) {
            if (i == j) continue;
            const float dx = samples_[j].x - samples_[i].x, dy = samples_[j].y - samples_[i].y;
            const float len = dx * dx + dy * dy;
            if (len < 1e-8f) continue;
            w = std::min(w, 1 - (px * dx + py * dy) / len);
        }
        out[i] = std::max(0.0f, w);
        total += out[i];
    }
    if (total > 0) for (auto& w : out) w /= total;
}
void BlendSpace2D::update(float seconds, const Parameters& p) {
    auto& w = weights_;
    weights(p.get(x_), p.get(y_), w);
    float duration = 0;
    for (std::size_t i = 0; i < samples_.size(); ++i) duration += w[i] * samples_[i].motion->duration(p);
    phase_ = advance(phase_, seconds, duration, true);
}
void BlendSpace2D::evaluate(const Skeleton& s, const Parameters& p, Pose& out) const {
    auto& w = weights_;
    weights(p.get(x_), p.get(y_), w);
    poses_.resize(samples_.size());
    refs_.clear(); used_.clear();
    refs_.reserve(samples_.size()); used_.reserve(samples_.size());
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        if (w[i] <= 1e-4f) continue;
        poses_[i].reset(s);
        samples_[i].motion->sample(s, phase_, p, poses_[i]);
        refs_.push_back(&poses_[i]); used_.push_back(w[i]);
    }
    blendMany(refs_, used_, out);
}

int StateMachine::addState(std::string name, std::shared_ptr<Node> node) {
    states_.push_back({std::move(name), std::move(node)});
    return int(states_.size()) - 1;
}
void StateMachine::addTransition(int from, int to, float seconds, Condition when) {
    transitions_.push_back({from, to, seconds, std::move(when)});
}
void StateMachine::start(int state) {
    current_ = states_.empty() ? 0 : std::clamp(state, 0, int(states_.size()) - 1);
    previous_ = -1; fade_ = 1; interrupted_ = false;
    lastOutput_.local.clear(); frozenSource_.local.clear();
    if (!states_.empty()) states_[std::size_t(current_)].node->reset();
}
void StateMachine::reset() { start(0); }
float StateMachine::phase() const {
    return states_.empty() ? -1 : states_[std::size_t(current_)].node->phase();
}
void StateMachine::update(float seconds, const Parameters& p) {
    if (states_.empty()) return;
    for (const auto& t : transitions_) {
        if (t.to == current_ || (t.from >= 0 && t.from != current_)) continue;
        if (!t.when(p, *states_[std::size_t(current_)].node)) continue;
        // A landing can interrupt a takeoff/fall cross-fade. Continue from
        // the displayed pose instead of suddenly resurrecting a full state.
        interrupted_ = previous_ >= 0 && fade_ < 1 && !lastOutput_.local.empty();
        if (interrupted_) frozenSource_ = lastOutput_;
        previous_ = current_;
        current_ = t.to;
        fadeSeconds_ = std::max(0.0f, t.seconds);
        fade_ = fadeSeconds_ > 0 ? 0.0f : 1.0f;
        auto& entered = states_[std::size_t(current_)];
        entered.node->reset();
        entered.time = 0;
        // A cycle entering a cycle starts in step with it (walk -> run).
        if (const float ph = states_[std::size_t(previous_)].node->phase(); ph >= 0 && entered.node->phase() >= 0)
            entered.node->syncTo(ph);
        if (onEnter) onEnter(previous_, current_);
        break;
    }
    auto& now = states_[std::size_t(current_)];
    now.node->update(seconds, p);
    now.time += seconds;
    if (previous_ >= 0 && fade_ < 1) {
        states_[std::size_t(previous_)].node->update(seconds, p);
        fade_ = fadeSeconds_ > 0 ? std::min(1.0f, fade_ + seconds / fadeSeconds_) : 1.0f;
        if (fade_ >= 1) previous_ = -1;
    }
}
void StateMachine::evaluate(const Skeleton& s, const Parameters& p, Pose& out) const {
    if (states_.empty()) return;
    if (previous_ >= 0 && fade_ < 1) {
        out.reset(s);
        if (interrupted_ && frozenSource_.local.size() == s.size()) out = frozenSource_;
        else states_[std::size_t(previous_)].node->evaluate(s, p, out);
        scratch_.reset(s);
        states_[std::size_t(current_)].node->evaluate(s, p, scratch_);
        // Smoothstep: a linear cross-fade starts and stops with a jolt.
        const float t = fade_ * fade_ * (3 - 2 * fade_);
        out.blend(scratch_, t);
        lastOutput_ = out;
        return;
    }
    out.reset(s);
    states_[std::size_t(current_)].node->evaluate(s, p, out);
    lastOutput_ = out;
}

} // namespace engine::animation
