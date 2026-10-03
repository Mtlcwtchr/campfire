// The animator (engine/animation): pose maths, blend spaces, the state
// machine, two-bone IK and the procedural humanoid graph, on hand-made
// skeletons - nothing here needs a character file.
#include "framework.hpp"

#include <cmath>

#include "engine/animation/animator.hpp"

namespace {
using namespace engine::animation;

// A leg: hip at the origin, knee 0.5 m down, ankle 0.5 m below that.
Skeleton leg() {
    Skeleton s;
    s.joints.push_back({"hip", -1, {{}, {0, 0, 1}, 1}, {}});
    s.joints.push_back({"knee", 0, {{}, {0, 0, -0.5f}, 1}, {}});
    s.joints.push_back({"ankle", 1, {{}, {0, 0, -0.5f}, 1}, {}});
    s.finish();
    return s;
}

// The mannequin's names, in a T-pose, enough of them for the humanoid graph.
Skeleton mannequin() {
    Skeleton s;
    const auto add = [&](const char* name, int parent, Vec3 at) {
        s.joints.push_back({name, parent, {{}, at, 1}, {}});
        return int(s.joints.size()) - 1;
    };
    const int pelvis = add("pelvis", -1, {0, 0, 1.0f});
    const int spine = add("spine_01", pelvis, {0, 0, 0.1f});
    const int chest = add("spine_03", spine, {0, 0, 0.2f});
    const int top = add("spine_05", chest, {0, 0, 0.2f});
    const int neck = add("neck_01", top, {0, 0, 0.1f});
    add("head", neck, {0, 0, 0.1f});
    for (int side = 0; side < 2; ++side) {
        const float y = side == 0 ? 1.0f : -1.0f;
        const std::string sfx = side == 0 ? "_l" : "_r";
        const int clav = add(("clavicle" + sfx).c_str(), top, {0, 0.1f * y, 0});
        const int upper = add(("upperarm" + sfx).c_str(), clav, {0, 0.1f * y, 0});
        const int lower = add(("lowerarm" + sfx).c_str(), upper, {0, 0.3f * y, 0});
        add(("hand" + sfx).c_str(), lower, {0, 0.25f * y, 0});
        const int thigh = add(("thigh" + sfx).c_str(), pelvis, {0, 0.1f * y, -0.05f});
        const int calf = add(("calf" + sfx).c_str(), thigh, {0, 0, -0.45f});
        add(("foot" + sfx).c_str(), calf, {0, 0, -0.45f});
    }
    s.finish();
    return s;
}

bool finite(const Pose& p) {
    for (const auto& t : p.local)
        if (!std::isfinite(t.rotation.x + t.rotation.y + t.rotation.z + t.rotation.w + t.translation.x +
                           t.translation.y + t.translation.z))
            return false;
    return true;
}
} // namespace

TEST(animation_rest_pose_forward_kinematics_is_the_rest_model) {
    const Skeleton s = leg();
    std::vector<Transform> model;
    Pose::rest(s).model(s, model);
    CHECK(std::abs(model[2].translation.z - 0.0f) < 1e-6f);
    CHECK(std::abs(s.joints[2].restModel.translation.z - 0.0f) < 1e-6f);
}

TEST(animation_rest_frame_rotation_swings_what_hangs_below) {
    const Skeleton s = leg();
    Pose p = Pose::rest(s);
    // A negative pitch about +y swings a hanging bone's end forward (+x).
    p.rotateInRestFrame(s, 0, {0, 1, 0}, -0.5f);
    std::vector<Transform> model;
    p.model(s, model);
    CHECK(model[2].translation.x > 0.4f);
    // The child's own rest-frame rotation still bends about the body's axis.
    p.rotateInRestFrame(s, 1, {0, 1, 0}, 0.5f);
    p.model(s, model);
    CHECK(std::abs(model[2].translation.x - (std::sin(0.5f) * 0.5f)) < 1e-3f);
}

TEST(animation_blend_space_1d_weights_are_piecewise_and_clamped) {
    BlendSpace1D space("speed");
    const auto idle = std::make_shared<IdleMotion>(HumanoidRig{}, Stance{});
    space.add(0, idle);
    space.add(2, idle);
    space.add(6, idle);
    std::vector<float> w;
    space.weights(1, w);
    CHECK(std::abs(w[0] - 0.5f) < 1e-6f && std::abs(w[1] - 0.5f) < 1e-6f && w[2] == 0);
    space.weights(-3, w);
    CHECK(w[0] == 1);
    space.weights(9, w);
    CHECK(w[2] == 1);
}

TEST(animation_blend_space_2d_is_exact_at_its_samples_and_sums_to_one) {
    BlendSpace2D space("x", "y");
    const auto idle = std::make_shared<IdleMotion>(HumanoidRig{}, Stance{});
    space.add(0, 0, idle);
    space.add(1, 0, idle);
    space.add(0, 1, idle);
    space.add(-1, 0, idle);
    std::vector<float> w;
    space.weights(1, 0, w);
    CHECK(std::abs(w[1] - 1) < 1e-5f);
    space.weights(0.3f, 0.4f, w);
    float total = 0;
    for (const float v : w) { CHECK(v >= 0); total += v; }
    CHECK(std::abs(total - 1) < 1e-5f);
}

TEST(animation_state_machine_cross_fades_and_follows_conditions) {
    const Skeleton s = mannequin();
    const HumanoidRig rig = HumanoidRig::find(s);
    auto machine = std::make_shared<StateMachine>();
    const int a = machine->addState("idle", std::make_shared<MotionNode>(std::make_shared<IdleMotion>(rig, Stance{})));
    const int b = machine->addState("land", std::make_shared<MotionNode>(
            std::make_shared<AirMotion>(rig, Stance{}, AirMotion::Kind::Land)));
    machine->addTransition(a, b, 0.2f, [](const Parameters& p, const Node&) { return p.flag("go"); });
    machine->addTransition(b, a, 0.1f, [](const Parameters&, const Node& n) { return n.finished(); });
    machine->start(a);
    Parameters p;
    machine->update(0.016f, p);
    CHECK(machine->currentName() == "idle");
    p.set("go", 1);
    machine->update(0.016f, p);
    CHECK(machine->currentName() == "land");
    p.set("go", 0);
    Pose out = Pose::rest(s);
    machine->evaluate(s, p, out);
    CHECK(finite(out));
    // The landing is a third of a second; then back to idle by itself.
    for (int i = 0; i < 40; ++i) machine->update(0.016f, p);
    CHECK(machine->currentName() == "idle");
}

TEST(animation_two_bone_ik_reaches_and_keeps_the_knee_forward) {
    const Skeleton s = leg();
    Pose p = Pose::rest(s);
    const Vec3 target{0.2f, 0.0f, 0.3f};
    twoBoneIk(s, p, 0, 1, 2, target, {1, 0, 0}, 1);
    std::vector<Transform> model;
    p.model(s, model);
    CHECK(length(model[2].translation - target) < 0.01f);
    CHECK(model[1].translation.x > 0.0f);   // the knee bent towards the pole
}

TEST(animation_humanoid_graph_runs_its_legs_in_opposition) {
    auto animator = buildHumanoid(mannequin());
    const auto& s = animator->skeleton();
    const HumanoidRig rig = HumanoidRig::find(s);
    animator->parameters().set("speed", 4.2f);
    animator->parameters().set("grounded", 1);
    std::vector<Transform> model;
    float apart = 0, together = 0, leftMean = 0, rightMean = 0;
    std::vector<std::pair<float, float>> feet;
    for (int frame = 0; frame < 60; ++frame) {
        animator->update(1.0f / 60);
        const Pose& pose = animator->evaluate();
        CHECK(finite(pose));
        pose.model(s, model);
        const float left = model[std::size_t(rig.foot[0])].translation.x;
        const float right = model[std::size_t(rig.foot[1])].translation.x;
        apart = std::max(apart, std::abs(left - right));
        feet.push_back({left, right});
        leftMean += left / 60; rightMean += right / 60;
    }
    // Over the cycle the feet move against each other.
    for (const auto& [l, r] : feet) together += (l - leftMean) * (r - rightMean);
    CHECK(together < 0);
    CHECK(apart > 0.3f);
    CHECK(animator->baseMachine()->currentName() == "locomotion");
    // A jump goes through take-off into the air and lands back on its feet.
    animator->parameters().set("jump", 1);
    animator->parameters().set("grounded", 0);
    animator->update(1.0f / 60);
    animator->parameters().set("jump", 0);
    CHECK(animator->baseMachine()->currentName() == "takeoff");
    animator->parameters().set("vz", -1);
    for (int i = 0; i < 30; ++i) animator->update(1.0f / 60);
    CHECK(animator->baseMachine()->currentName() == "fall");
    animator->parameters().set("grounded", 1);
    animator->update(1.0f / 60);
    CHECK(animator->baseMachine()->currentName() == "land");
}
