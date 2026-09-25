// The fixed step, which is the only clock the world is allowed to have.
#include "framework.hpp"

#include <cstdint>

#include "engine/pipeline/clock.hpp"

using engine::FixedClock;

TEST(a_tenth_of_a_second_is_one_step_and_nothing_is_lost) {
    FixedClock clock(0.1);
    CHECK(clock.advance(0.1) == 1u);
    CHECK(clock.alpha() < 0.001f);
}

TEST(frames_that_do_not_divide_by_the_step_still_add_up) {
    // Sixty frames a second against ten steps a second: no frame is a whole
    // step, and rounding the remainder away every frame is how a simulation
    // quietly runs at nine tenths speed for ever.
    FixedClock clock(0.1);
    std::uint32_t steps = 0;
    for (int frame = 0; frame < 600; ++frame) steps += clock.advance(1.0 / 60.0);
    // Ten seconds of frames is a hundred steps, give or take the one in hand.
    CHECK(steps >= 99 && steps <= 100);
}

TEST(a_long_frame_is_capped_and_the_rest_is_dropped) {
    // A second-long hitch asks for ten steps. Running them takes longer than a
    // second, which asks for eleven: that is the spiral, and it looks like a
    // hang rather than like slowness.
    FixedClock clock(0.1, 5);
    CHECK(clock.advance(1.0) == 5u);
    // And what could not be run is not carried: the world is behind, and it
    // stays behind rather than spending the next minute catching up.
    CHECK(clock.advance(0.0) == 0u);
    CHECK(clock.held() == 0.0);
}

TEST(alpha_says_how_far_between_two_steps_the_world_is) {
    FixedClock clock(0.1);
    clock.advance(0.15);            // one step run, half a step held
    CHECK(clock.alpha() > 0.45f && clock.alpha() < 0.55f);
}
