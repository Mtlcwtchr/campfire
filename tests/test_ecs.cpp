#include "framework.hpp"

#include "game/ai/planner.hpp"
#include "game/ecs/render/extraction.hpp"
#include "game/ecs/life/components.hpp"
#include "game/ecs/resources/components.hpp"
#include "game/simulation/needs.hpp"
#include "game/simulation/inventory.hpp"
#include "game/simulation/world.hpp"
#include "support.hpp"

#include <algorithm>

namespace {

sim::WorldConfig ecsConfig() {
    sim::WorldConfig cfg;
    cfg.seed = 91;
    cfg.mapWidth = 64;
    cfg.mapHeight = 64;
    cfg.worldCells = 64;
    cfg.startingPopulation = 8;
    return cfg;
}

std::size_t aliveCount(const auto& values) {
    std::size_t result = 0;
    for (const auto& value : values)
        if (value.alive) ++result;
    return result;
}

} // namespace

TEST(world_builds_an_ecs_projection_for_render_facing_entities) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());

    const std::size_t expected = aliveCount(world.people()) + aliveCount(world.animals()) +
                                 aliveCount(world.buildings()) + aliveCount(world.nodes()) +
                                 aliveCount(world.stacks());
    CHECK_EQ(world.ecsEntityCount(), expected);

    auto view = world.ecs().view<sim::ecs::Identity, sim::ecs::Transform,
                                  sim::ecs::Renderable>();
    std::size_t projected = 0;
    view.each([&](const sim::ecs::Identity&, const sim::ecs::Transform&,
                  const sim::ecs::Renderable&) { ++projected; });
    CHECK_EQ(projected, expected);
}

TEST(ecs_snapshot_exposes_read_only_typed_access) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto snapshot = world.ecs().snapshot();
    const auto projected = snapshot.size<sim::ecs::Identity, sim::ecs::Transform>();
    CHECK_EQ(projected, world.ecsEntityCount());
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Person, 0);
    const bool hasIdentityAndTransform =
            snapshot.all_of<sim::ecs::Identity, sim::ecs::Transform>(entity);
    CHECK(hasIdentityAndTransform);
    CHECK_EQ(snapshot.get<const sim::ecs::Identity>(entity).legacyIndex, 0u);
}

TEST(world_refreshes_ecs_projection_after_a_tick) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    world.tick();

    const std::size_t expected = aliveCount(world.people()) + aliveCount(world.animals()) +
                                 aliveCount(world.buildings()) + aliveCount(world.nodes()) +
                                 aliveCount(world.stacks());
    CHECK_EQ(world.ecsEntityCount(), expected);
}

TEST(spawn_stack_publishes_ecs_state_immediately) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const sim::ItemStackId stack = world.spawnStack(db.itemByName("reed"), 2,
                                                    world.settlement(sim::SettlementId{0}).hearth,
                                                    sim::SettlementId{0});
    const auto* state = sim::ecsStackState(world, stack);
    CHECK(state != nullptr);
    if (state == nullptr) return;
    CHECK(state->alive);
    CHECK_EQ(state->count, 2);
}

TEST(destroy_stack_publishes_ecs_lifecycle_immediately) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const sim::ItemStackId stack = world.spawnStack(db.itemByName("reed"), 2,
                                                    world.settlement(sim::SettlementId{0}).hearth,
                                                    sim::SettlementId{0});
    world.destroyStack(stack);
    const auto* state = sim::ecsStackState(world, stack);
    CHECK(state != nullptr);
    if (state == nullptr) return;
    CHECK(!state->alive);
    CHECK_EQ(state->count, 0);
}

TEST(building_deliveries_are_projected_as_a_separate_ecs_component) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto def = db.buildingByName("campfire");
    const auto id = world.placeBlueprint(def, world.settlement(sim::SettlementId{0}).hearth,
                                         sim::SettlementId{0});
    world.syncEcs();
    world.building(id).delivered[0] = 3;
    world.syncEcs();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Building, id.value);
    CHECK(world.ecs().all_of<sim::ecs::DeliveredMaterials>(entity));
    const auto& delivered = world.ecs().get<const sim::ecs::DeliveredMaterials>(entity).values;
    CHECK_EQ(delivered[0], 3);
}

TEST(place_blueprint_publishes_building_ecs_state_immediately) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto id = world.placeBlueprint(db.buildingByName("campfire"),
                                         world.settlement(sim::SettlementId{0}).hearth,
                                         sim::SettlementId{0});
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Building, id.value);
    CHECK(entity != entt::null);
    CHECK(world.ecs().all_of<sim::ecs::Alive>(entity));
    CHECK(world.ecs().get<const sim::ecs::Alive>(entity).value);
    CHECK(world.ecs().all_of<sim::ecs::ConstructionProgress>(entity));
    CHECK(world.ecs().all_of<sim::ecs::DeliveredMaterials>(entity));
    CHECK_EQ(world.ecs().get<const sim::ecs::ConstructionProgress>(entity).phase,
             static_cast<std::uint8_t>(sim::BuildState::Blueprint));
}

TEST(spawn_resource_node_publishes_ecs_state_immediately) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto id = world.spawnNode(db.resourceNodeByName("fallen_branch"),
                                    world.settlement(sim::SettlementId{0}).hearth);
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::ResourceNode, id.value);
    CHECK(entity != entt::null);
    CHECK(world.ecs().all_of<sim::ecs::ResourceState>(entity));
    CHECK(world.ecs().get<const sim::ecs::Alive>(entity).value);
}

TEST(spawn_animal_publishes_ecs_state_immediately) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto id = world.spawnAnimal(db.animalByName("sheep"),
                                      world.settlement(sim::SettlementId{0}).hearth,
                                      sim::SettlementId{0}, sim::Sex::Female, 100);
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Animal, id.value);
    CHECK(entity != entt::null);
    const bool hasAnimalState = world.ecs().all_of<sim::ecs::Alive, sim::ecs::Age,
                                                   sim::ecs::AnimalTimers>(entity);
    CHECK(hasAnimalState);
    CHECK(world.ecs().get<const sim::ecs::Alive>(entity).value);
}

TEST(reused_animal_slot_refreshes_existing_ecs_entity) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto first = world.spawnAnimal(db.animalByName("sheep"),
                                         world.settlement(sim::SettlementId{0}).hearth,
                                         sim::SettlementId{0}, sim::Sex::Female, 100);
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Animal, first.value);
    world.animals()[first.value].alive = false;
    const auto second = world.spawnAnimal(db.animalByName("cattle"),
                                          core::TilePos{2, 2}, sim::SettlementId{0},
                                          sim::Sex::Male, 200);
    CHECK_EQ(second.value, first.value);
    CHECK_EQ(world.ecsEntity(sim::ecs::Kind::Animal, second.value), entity);
    CHECK_EQ(world.ecs().get<const sim::ecs::Age>(entity).days, 200);
    const core::TilePos expectedTile{2, 2};
    CHECK_EQ(world.ecs().get<const sim::ecs::GrazingTarget>(entity).tile, expectedTile);
    const auto expectedCell = sim::ecs::cellForTile(expectedTile, sim::ecs::kSpatialCellExtent);
    CHECK_EQ(world.ecs().get<const sim::ecs::SpatialCell>(entity).x, expectedCell.x);
    CHECK_EQ(world.ecs().get<const sim::ecs::SpatialCell>(entity).y, expectedCell.y);
}

TEST(world_keeps_ecs_handles_stable_across_a_tick) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const entt::entity person = world.ecsEntity(sim::ecs::Kind::Person, 0);
    CHECK(person != entt::null);
    world.tick();
    CHECK_EQ(world.ecsEntity(sim::ecs::Kind::Person, 0), person);
}

TEST(world_population_report_reads_the_ecs_person_view) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    CHECK_EQ(world.report().population,
             static_cast<std::int32_t>(world.ecs().view<sim::ecs::Person>().size()));
    world.tick();
    CHECK_EQ(world.report().population,
             static_cast<std::int32_t>(world.ecs().view<sim::ecs::Person>().size()));
}

TEST(world_projects_person_state_for_the_next_simulation_slice) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    for (const entt::entity entity : world.ecs().view<sim::ecs::Person>()) {
        CHECK((world.ecs().all_of<sim::ecs::Identity, sim::ecs::Alive, sim::ecs::Health,
                                 sim::ecs::Hunger, sim::ecs::Thirst, sim::ecs::Fatigue,
                                 sim::ecs::SleepState>(entity)));
    }
    world.tick();
    for (const entt::entity entity : world.ecs().view<sim::ecs::Person>()) {
        CHECK((world.ecs().all_of<sim::ecs::Health, sim::ecs::Hunger, sim::ecs::Thirst,
                                 sim::ecs::Fatigue>(entity)));
    }
}

TEST(legacy_person_mutation_is_imported_before_first_tick) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto& person = world.people().front();
    person.satiety = core::Fixed::ratio(1, 5);
    person.ailment = sim::Person::Ailment::Wound;
    person.ailmentSeverity = core::Fixed::ratio(3, 5);
    world.tick();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Person, person.id.value);
    CHECK(entity != entt::null);
    CHECK_EQ(world.ecs().get<const sim::ecs::Hunger>(entity).value, person.satiety);
    CHECK_EQ(world.ecs().get<const sim::ecs::AilmentState>(entity).severity,
             person.ailmentSeverity);
}

TEST(world_projects_compact_job_state_for_each_person) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto view = world.ecs().view<const sim::ecs::Identity, const sim::ecs::JobState,
                                  const sim::ecs::JobCategory, const sim::ecs::JobLinks>();
    std::size_t checked = 0;
    view.each([&](const sim::ecs::Identity& identity, const sim::ecs::JobState& state,
                  const sim::ecs::JobCategory& category, const sim::ecs::JobLinks& links) {
        const auto& person = world.person(sim::PersonId{identity.legacyIndex});
        CHECK_EQ(state.kind, static_cast<std::uint8_t>(person.job.kind));
        CHECK_EQ(state.phase, static_cast<std::uint8_t>(person.job.phase));
        CHECK_EQ(state.active, person.job.valid());
        CHECK_EQ(category.value, static_cast<std::uint8_t>(person.job.category));
        CHECK_EQ(links.stack, person.job.stack.value);
        CHECK_EQ(links.building, person.job.building.value);
        CHECK_EQ(links.node, person.job.node.value);
        CHECK_EQ(links.animal, person.job.animal.value);
        CHECK_EQ(links.person, person.job.person.value);
        CHECK_EQ(links.target, person.job.target);
        ++checked;
    });
    CHECK_EQ(checked, world.ecs().view<sim::ecs::Person>().size());
    auto progress = world.ecs().view<const sim::ecs::Person, const sim::ecs::JobProgress>();
    CHECK_EQ(progress.size_hint(), world.ecs().view<sim::ecs::Person>().size());
}

TEST(world_projects_atomic_inventory_components_for_each_person) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto view = world.ecs().view<const sim::ecs::Identity, const sim::ecs::Carrying,
                                  const sim::ecs::EquippedTool, const sim::ecs::WornItems>();
    std::size_t checked = 0;
    view.each([&](const sim::ecs::Identity& identity, const sim::ecs::Carrying& carrying,
                  const sim::ecs::EquippedTool& tool, const sim::ecs::WornItems& worn) {
        const auto& person = world.person(sim::PersonId{identity.legacyIndex});
        CHECK_EQ(carrying.stack, person.carrying.value);
        CHECK_EQ(tool.stack, person.equippedTool.value);
        CHECK_EQ(worn.stacks.size(), person.worn.size());
        for (std::size_t i = 0; i < worn.stacks.size(); ++i)
            CHECK_EQ(worn.stacks[i], person.worn[i].value);
        ++checked;
    });
    CHECK_EQ(checked, world.ecs().view<sim::ecs::Person>().size());
}

TEST(world_projects_atomic_social_components_for_each_person) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto view = world.ecs().view<const sim::ecs::Identity, const sim::ecs::SettlementMember,
                                  const sim::ecs::HouseholdMember, const sim::ecs::Parentage,
                                  const sim::ecs::Spouse>();
    std::size_t checked = 0;
    view.each([&](const sim::ecs::Identity& identity, const sim::ecs::SettlementMember& settlement,
                  const sim::ecs::HouseholdMember& household, const sim::ecs::Parentage& parentage,
                  const sim::ecs::Spouse& spouse) {
        const auto& person = world.person(sim::PersonId{identity.legacyIndex});
        CHECK_EQ(settlement.settlement, person.settlement.value);
        CHECK_EQ(household.household, person.household.value);
        CHECK_EQ(parentage.mother, person.mother.value);
        CHECK_EQ(parentage.father, person.father.value);
        CHECK_EQ(spouse.person, person.spouse.value);
        ++checked;
    });
    CHECK_EQ(checked, world.ecs().view<sim::ecs::Person>().size());
}

TEST(save_load_rebuilds_the_same_ecs_entity_projection) {
    const auto& db = testing::sharedContent();
    sim::World original(db, ecsConfig());
    original.tick();

    const entt::entity animal = original.ecs().view<sim::ecs::Animal>().begin() !=
                                        original.ecs().view<sim::ecs::Animal>().end()
                                    ? *original.ecs().view<sim::ecs::Animal>().begin()
                                    : entt::null;
    if (animal != entt::null) {
        original.ecs().get<sim::ecs::Hunger>(animal).value = core::Fixed::ratio(3, 7);
        original.ecs().get<sim::ecs::Thirst>(animal).value = core::Fixed::ratio(5, 9);
        original.ecs().get<sim::ecs::Fatigue>(animal).value = core::Fixed::ratio(2, 11);
    }

    core::BinaryWriter out;
    original.write(out);
    core::BinaryReader in(out.data());
    sim::World restored(db, ecsConfig(), sim::World::FromSave{});
    CHECK(restored.read(in));
    CHECK(in.ok());
    CHECK_EQ(restored.ecsEntityCount(), original.ecsEntityCount());
    CHECK_EQ(restored.report().population, original.report().population);
    if (animal != entt::null) {
        const auto& identity = original.ecs().get<sim::ecs::Identity>(animal);
        const entt::entity restoredAnimal = restored.ecsEntity(sim::ecs::Kind::Animal,
                                                               identity.legacyIndex);
        CHECK_EQ(restored.ecs().get<sim::ecs::Hunger>(restoredAnimal).value,
                 original.ecs().get<sim::ecs::Hunger>(animal).value);
        CHECK_EQ(restored.ecs().get<sim::ecs::Thirst>(restoredAnimal).value,
                 original.ecs().get<sim::ecs::Thirst>(animal).value);
        CHECK_EQ(restored.ecs().get<sim::ecs::Fatigue>(restoredAnimal).value,
                 original.ecs().get<sim::ecs::Fatigue>(animal).value);
    }
}

TEST(ecs_batches_have_stable_spatial_ownership_and_boundary_phase) {
    sim::ecs::Registry registry;
    const entt::entity centre = registry.create();
    registry.emplace<sim::ecs::SpatialCell>(centre, 0, 0);
    const entt::entity edge = registry.create();
    registry.emplace<sim::ecs::SpatialCell>(edge, 1, 0);
    const entt::entity outer = registry.create();
    registry.emplace<sim::ecs::SpatialCell>(outer, 2, 2);

    const auto schedule = sim::ecs::BatchScheduler::build(registry, {0, 0}, 1);
    CHECK_EQ(schedule.local.size(), std::size_t{1});
    CHECK_EQ(schedule.boundary.size(), std::size_t{1});
    CHECK_EQ(schedule.local.front().entities.front(), centre);
    CHECK_EQ(schedule.boundary.front().entities.front(), edge);

    const auto rings = sim::ecs::BatchScheduler::build(registry, {0, 0}, 2);
    CHECK_EQ(rings.local.size(), std::size_t{2});
    CHECK_EQ(rings.local.front().cell, (sim::ecs::CellId{0, 0}));
    CHECK_EQ(rings.local.back().cell, (sim::ecs::CellId{1, 0}));

    const auto buffers = sim::ecs::BatchScheduler::runLocalCommands(
        schedule, 2, [](const sim::ecs::Batch& batch, sim::ecs::CommandBuffer& commands) {
            for (const entt::entity entity : batch.entities)
                commands.push(sim::ecs::Command{sim::ecs::SetPosition{entt::to_entity(entity), {}}});
        });
    const auto merged = sim::ecs::BatchScheduler::mergeCommands(buffers);
    CHECK_EQ(merged.commands().size(), std::size_t{1});
}

TEST(ecs_world_exposes_scheduled_component_batches) {
    sim::ecs::EcsWorld world;
    const entt::entity animal = world.writeRegistry().create();
    world.writeRegistry().emplace<sim::ecs::SpatialCell>(animal, 0, 0);
    world.writeRegistry().emplace<sim::ecs::Animal>(animal);
    const entt::entity person = world.writeRegistry().create();
    world.writeRegistry().emplace<sim::ecs::SpatialCell>(person, 0, 0);
    world.writeRegistry().emplace<sim::ecs::Person>(person);

    const auto animals = world.scheduleFor<sim::ecs::Animal>({0, 0}, 1);
    CHECK_EQ(animals.local.size(), std::size_t{1});
    CHECK_EQ(animals.local.front().entities.front(), animal);
    const auto all = world.schedule({0, 0}, 1);
    CHECK_EQ(all.local.size(), std::size_t{1});
    CHECK_EQ(all.local.front().entities.size(), std::size_t{2});
}

TEST(ecs_world_facade_runs_typed_local_and_boundary_batches) {
    sim::ecs::EcsWorld world;
    const entt::entity local = world.writeRegistry().create();
    world.writeRegistry().emplace<sim::ecs::SpatialCell>(local, 0, 0);
    world.writeRegistry().emplace<sim::ecs::Animal>(local);
    const entt::entity edge = world.writeRegistry().create();
    world.writeRegistry().emplace<sim::ecs::SpatialCell>(edge, 1, 0);
    world.writeRegistry().emplace<sim::ecs::Animal>(edge);
    std::atomic<int> localCount = 0;
    world.forEachLocal<sim::ecs::Animal>({0, 0}, 1, 2,
        [&](const sim::ecs::Batch& batch) { localCount += static_cast<int>(batch.entities.size()); });
    int boundaryCount = 0;
    world.forEachBoundary<sim::ecs::Animal>({0, 0}, 1,
        [&](const sim::ecs::Batch& batch) { boundaryCount += static_cast<int>(batch.entities.size()); });
    CHECK_EQ(localCount.load(), 1);
    CHECK_EQ(boundaryCount, 1);
}

TEST(ecs_world_applies_worker_buffers_in_stable_order) {
    sim::ecs::EcsWorld world;
    std::vector<sim::ecs::CommandBuffer> buffers(2);
    buffers[0].push(sim::ecs::Command{sim::ecs::SetPosition{1, {}}});
    buffers[1].push(sim::ecs::Command{sim::ecs::SetPosition{2, {}}});
    std::vector<std::uint32_t> order;
    world.apply(buffers, [&](sim::ecs::Registry&, const sim::ecs::Command& command) {
        order.push_back(std::get<sim::ecs::SetPosition>(command).entity);
    });
    CHECK_EQ(order.size(), std::size_t{2});
    CHECK_EQ(order[0], std::uint32_t{1});
    CHECK_EQ(order[1], std::uint32_t{2});
}

TEST(ecs_cell_floor_division_is_stable_for_negative_coordinates) {
    CHECK_EQ(sim::ecs::cellForTile({-1, -33}, 32), (sim::ecs::CellId{-1, -2}));
    CHECK_EQ(sim::ecs::cellForTile({0, 0}, 32), (sim::ecs::CellId{0, 0}));
}

TEST(ecs_building_health_survives_projection_refresh) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto buildings = world.ecs().view<const sim::ecs::Identity, sim::ecs::Building,
                                       sim::ecs::Health>();
    if (buildings.begin() == buildings.end()) return;
    const entt::entity entity = *buildings.begin();
    auto& health = buildings.get<sim::ecs::Health>(entity);
    health.current = core::Fixed::ratio(1, 3);
    world.tick();
    CHECK_EQ(world.ecs().get<const sim::ecs::Health>(entity).current,
             core::Fixed::ratio(1, 3));
}

TEST(ecs_projects_independent_needs_components_for_bodies_and_buildings) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());

    auto people = world.ecs().view<const sim::ecs::Person, const sim::ecs::Health,
                                    const sim::ecs::Hunger, const sim::ecs::Thirst,
                                    const sim::ecs::Fatigue, const sim::ecs::SleepState>();
    std::size_t personCount = 0;
    for (const entt::entity ignored : people) { (void)ignored; ++personCount; }
    CHECK_EQ(personCount, world.ecs().view<sim::ecs::Person>().size());

    auto animals = world.ecs().view<const sim::ecs::Animal, const sim::ecs::Health,
                                     const sim::ecs::Hunger, const sim::ecs::Thirst>();
    std::size_t animalCount = 0;
    for (const entt::entity ignored : animals) { (void)ignored; ++animalCount; }
    CHECK_EQ(animalCount, aliveCount(world.animals()));

    auto buildings = world.ecs().view<const sim::ecs::Building, const sim::ecs::Health>();
    std::size_t buildingCount = 0;
    for (const entt::entity ignored : buildings) { (void)ignored; ++buildingCount; }
    CHECK_EQ(buildingCount, aliveCount(world.buildings()));
    auto construction = world.ecs().view<const sim::ecs::Building,
                                          const sim::ecs::ConstructionProgress>();
    CHECK_EQ(construction.size_hint(), buildingCount);
}

TEST(eat_from_updates_ecs_authoritative_needs) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto& person = world.people().front();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Person, person.id.value);
    CHECK(entity != entt::null);

    const core::Fixed initial = core::Fixed::ratio(1, 5);
    person.satiety = initial;
    world.ecs().get<sim::ecs::Hunger>(entity).value = initial;
    const auto food = world.spawnStack(db.itemByName("berries"), 4, person.tile, person.settlement);
    CHECK(food.valid());

    sim::eatFrom(world, person, food);

    const auto& ecsHunger = world.ecs().get<const sim::ecs::Hunger>(entity);
    CHECK(ecsHunger.value > initial);
    CHECK_EQ(ecsHunger.value, person.satiety);
}

TEST(sync_ecs_needs_does_not_overwrite_other_domains) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto& person = world.people().front();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Person, person.id.value);
    CHECK(entity != entt::null);
    const auto before = world.ecs().get<const sim::ecs::JobState>(entity);

    person.satiety = core::Fixed::ratio(2, 7);
    person.health = core::Fixed::ratio(5, 6);
    world.syncEcsNeeds(person.id);

    CHECK_EQ(world.ecs().get<const sim::ecs::Hunger>(entity).value, person.satiety);
    CHECK_EQ(world.ecs().get<const sim::ecs::Health>(entity).current, person.health);
    const auto& after = world.ecs().get<const sim::ecs::JobState>(entity);
    CHECK_EQ(after.kind, before.kind);
    CHECK_EQ(after.phase, before.phase);
    CHECK_EQ(after.active, before.active);
}

TEST(ecs_needs_control_work_eligibility) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto& person = world.people().front();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Person, person.id.value);
    CHECK(entity != entt::null);
    person.ageYears = 30;
    person.alive = true;
    person.asleep = false;
    person.ailmentSeverity = core::kZero;
    world.syncEcsNeeds(person.id);
    CHECK(sim::canWork(world, person));

    world.ecs().get<sim::ecs::AilmentState>(entity).severity = core::Fixed::ratio(3, 5);
    CHECK(!sim::canWork(world, person));
}

TEST(ecs_sync_preserves_animal_hot_components) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const entt::entity entity = world.ecs().view<sim::ecs::Animal>().front();
    const core::Fixed condition = core::Fixed::ratio(2, 3);
    world.ecs().get<sim::ecs::Health>(entity).current = condition;
    world.ecs().get<sim::ecs::SleepState>(entity).asleep = true;

    world.syncEcs();

    const auto& identity = world.ecs().get<const sim::ecs::Identity>(entity);
    const auto& animal = world.animal(sim::AnimalId{identity.legacyIndex});
    CHECK_EQ(world.ecs().get<const sim::ecs::Health>(entity).current, condition);
    CHECK_EQ(world.ecs().get<const sim::ecs::SleepState>(entity).asleep, true);
    CHECK_EQ(animal.condition, condition);
    CHECK_EQ(animal.penned, true);
}

TEST(ecs_animal_age_is_component_authoritative) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto animals = world.ecs().view<const sim::ecs::Identity, sim::ecs::Animal,
                                     sim::ecs::Age>();
    CHECK(animals.begin() != animals.end());
    const entt::entity entity = *animals.begin();
    const auto identity = animals.get<const sim::ecs::Identity>(entity);
    auto& age = animals.get<sim::ecs::Age>(entity);
    age.days = 123;
    world.tick();
    CHECK_EQ(world.animal(sim::AnimalId{identity.legacyIndex}).ageDays, age.days);
    CHECK(age.days >= 123);
}

TEST(ecs_animal_grazing_target_is_component_authoritative) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto animals = world.ecs().view<const sim::ecs::Identity, sim::ecs::Animal,
                                     sim::ecs::GrazingTarget>();
    CHECK(animals.begin() != animals.end());
    const entt::entity entity = *animals.begin();
    const auto identity = animals.get<const sim::ecs::Identity>(entity);
    auto& target = animals.get<sim::ecs::GrazingTarget>(entity);
    target.tile = {7, 9};
    world.tick();
    CHECK_EQ(world.animal(sim::AnimalId{identity.legacyIndex}).grazeTarget, target.tile);
}

TEST(ecs_animal_timers_are_component_authoritative) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto animals = world.ecs().view<const sim::ecs::Identity, sim::ecs::Animal,
                                     sim::ecs::AnimalTimers>();
    CHECK(animals.begin() != animals.end());
    const entt::entity entity = *animals.begin();
    const auto identity = animals.get<const sim::ecs::Identity>(entity);
    auto& timers = animals.get<sim::ecs::AnimalTimers>(entity);
    timers.nextBreedTick = 777;
    timers.nextShearTick = 778;
    timers.nextMilkTick = 779;
    world.tick();
    const auto& animal = world.animal(sim::AnimalId{identity.legacyIndex});
    CHECK_EQ(animal.nextBreedTick, timers.nextBreedTick);
    CHECK_EQ(animal.nextShearTick, timers.nextShearTick);
    CHECK_EQ(animal.nextMilkTick, timers.nextMilkTick);
}

TEST(ecs_building_progress_survives_projection_refresh) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto buildings = world.ecs().view<const sim::ecs::Identity, sim::ecs::Building,
                                       sim::ecs::ConstructionProgress>();
    if (buildings.begin() == buildings.end()) return;
    const entt::entity entity = *buildings.begin();
    auto& progress = buildings.get<sim::ecs::ConstructionProgress>(entity);
    const core::Fixed before = progress.done;
    progress.done = before + core::Fixed::ratio(1, 10);
    world.tick();
    CHECK(world.ecs().get<const sim::ecs::ConstructionProgress>(entity).done >= progress.done);
}

TEST(ecs_building_phase_is_authoritative) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    world.placeBlueprint(db.buildingByName("campfire"),
                         world.settlement(sim::SettlementId{0}).hearth,
                         sim::SettlementId{0});
    auto buildings = world.ecs().view<const sim::ecs::Identity, sim::ecs::Building,
                                       sim::ecs::ConstructionProgress>();
    CHECK(buildings.begin() != buildings.end());
    const entt::entity entity = *buildings.begin();
    const auto identity = buildings.get<const sim::ecs::Identity>(entity);
    auto& progress = buildings.get<sim::ecs::ConstructionProgress>(entity);
    progress.phase = static_cast<std::uint8_t>(sim::BuildState::Building);
    progress.complete = false;
    CHECK_EQ(world.buildingPhase(sim::BuildingId{identity.legacyIndex}),
             static_cast<std::uint8_t>(sim::BuildState::Building));
}

TEST(ecs_resource_state_survives_projection_refresh) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    auto nodes = world.ecs().view<const sim::ecs::Identity, sim::ecs::ResourceNode,
                                  sim::ecs::ResourceState>();
    CHECK(nodes.begin() != nodes.end());
    const entt::entity entity = *nodes.begin();
    auto& state = nodes.get<sim::ecs::ResourceState>(entity);
    state.workDone = core::Fixed::ratio(1, 4);
    state.depleted = true;
    state.regrowAtTick = 9876;
    world.tick();
    const auto& restored = world.ecs().get<const sim::ecs::ResourceState>(entity);
    CHECK_EQ(restored.workDone, state.workDone);
    CHECK_EQ(restored.depleted, state.depleted);
    CHECK_EQ(restored.regrowAtTick, state.regrowAtTick);
}

TEST(ecs_item_stack_state_tracks_inventory_projection) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto person = world.people().front();
    const auto item = db.itemByName("berries");
    const auto stack = world.spawnStack(item, 7, person.tile, person.settlement);
    world.tick();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::ItemStack, stack.value);
    CHECK(entity != entt::null);
    auto& legacy = world.stack(stack);
    legacy.count = 3;
    legacy.tile.x += 1;
    world.syncEcs();
    const auto& state = world.ecs().get<const sim::ecs::ItemStackState>(entity);
    CHECK_EQ(state.count, 3);
    CHECK_EQ(state.tileX, legacy.tile.x);
}

TEST(ecs_checksum_reads_authoritative_stack_state) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto person = world.people().front();
    const auto stack = world.spawnStack(db.itemByName("berries"), 7,
                                        person.tile, person.settlement);
    const auto before = world.checksum();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::ItemStack, stack.value);
    world.ecs().get<sim::ecs::ItemStackState>(entity).count = 2;
    CHECK(world.checksum() != before);
}

TEST(ecs_capacity_reads_hot_needs_components) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    CHECK(!world.people().empty());
    const sim::Person& person = world.people().front();
    const entt::entity entity = world.ecsEntity(sim::ecs::Kind::Person, person.id.value);
    CHECK(entity != entt::null);
    const sim::Fixed normal = sim::workCapacity(world, person);
    world.ecs().get<sim::ecs::Hunger>(entity).value = core::kZero;
    const sim::Fixed hungry = sim::workCapacity(world, person);
    CHECK(normal > hungry);
}

TEST(ai_planner_uses_lod_intervals_and_emits_data_only_intents) {
    sim::ecs::Registry registry;
    const entt::entity near = registry.create();
    registry.emplace<sim::ai::Controlled>(near, sim::ai::Lod::Near, 0);
    registry.emplace<sim::ecs::Fatigue>(near, core::Fixed::ratio(1, 8), core::kZero);
    const entt::entity far = registry.create();
    registry.emplace<sim::ai::Controlled>(far, sim::ai::Lod::Far, 32);

    sim::ai::Planner planner;
    std::vector<sim::ai::Intent> intents;
    planner.plan(sim::ecs::Snapshot{registry}, {1, 1}, intents);
    CHECK_EQ(intents.size(), std::size_t{1});
    CHECK(std::holds_alternative<sim::ai::Sleep>(intents.front()));
    planner.execute(registry, intents, 1);
    CHECK_EQ(registry.get<sim::ai::LastDecision>(near).kind, sim::ai::Decision::Sleep);
    CHECK_EQ(registry.get<sim::ai::LastDecision>(near).tick, std::int64_t{1});
    CHECK_EQ(registry.get<sim::ai::LastDecision>(near).elapsedTicks, std::int64_t{2});
    CHECK_EQ(sim::ai::Planner::interval(sim::ai::Lod::Near), std::int64_t{2});
    CHECK_EQ(sim::ai::Planner::interval(sim::ai::Lod::Far), std::int64_t{32});

    intents.clear();
    planner.plan(sim::ecs::Snapshot{registry}, {300, 1}, intents);
    CHECK_EQ(intents.size(), std::size_t{2});
    planner.execute(registry, intents, 300);
    CHECK_EQ(registry.get<sim::ai::LastDecision>(far).elapsedTicks, std::int64_t{300});
}

TEST(ai_lod_is_assigned_from_spatial_distance) {
    sim::ecs::Registry registry;
    const entt::entity focus = registry.create();
    registry.emplace<sim::ai::Controlled>(focus, sim::ai::Lod::Far, 100);
    registry.emplace<sim::ecs::SpatialCell>(focus, 0, 0);
    const entt::entity mid = registry.create();
    registry.emplace<sim::ai::Controlled>(mid, sim::ai::Lod::Near, 100);
    registry.emplace<sim::ecs::SpatialCell>(mid, 2, 0);
    const entt::entity dormant = registry.create();
    registry.emplace<sim::ai::Controlled>(dormant, sim::ai::Lod::Near, 100);
    registry.emplace<sim::ecs::SpatialCell>(dormant, 20, 0);

    sim::ai::Planner::assignLod(registry, {0, 0}, 1, 3, 8, 10);
    CHECK_EQ(registry.get<sim::ai::Controlled>(focus).lod, sim::ai::Lod::Focus);
    CHECK_EQ(registry.get<sim::ai::Controlled>(mid).lod, sim::ai::Lod::Mid);
    CHECK_EQ(registry.get<sim::ai::Controlled>(dormant).lod, sim::ai::Lod::Dormant);
    CHECK_EQ(registry.get<sim::ai::Controlled>(dormant).nextThinkTick, std::int64_t{10});
}

TEST(ai_planner_parallel_batches_preserve_overdue_elapsed_time) {
    sim::ecs::Registry registry;
    for (int i = 0; i < 6; ++i) {
        const entt::entity entity = registry.create();
        registry.emplace<sim::ai::Controlled>(entity, sim::ai::Lod::Far, i == 0 ? 4 : 0);
        registry.emplace<sim::ecs::SpatialCell>(entity, i % 3, i / 3);
    }
    sim::ai::Planner planner;
    std::vector<sim::ai::Intent> serial;
    std::vector<sim::ai::Intent> parallel;
    planner.plan(sim::ecs::Snapshot{registry}, {40, 1}, serial);
    planner.planParallel(sim::ecs::Snapshot{registry}, {40, 1}, {0, 0}, 4, 3, parallel);
    CHECK_EQ(parallel.size(), serial.size());
    std::vector<std::int64_t> serialElapsed;
    std::vector<std::int64_t> parallelElapsed;
    for (const auto& intent : serial)
        serialElapsed.push_back(std::get<sim::ai::Wander>(intent).elapsedTicks);
    for (const auto& intent : parallel)
        parallelElapsed.push_back(std::get<sim::ai::Wander>(intent).elapsedTicks);
    std::sort(serialElapsed.begin(), serialElapsed.end());
    std::sort(parallelElapsed.begin(), parallelElapsed.end());
    CHECK_EQ(parallelElapsed, serialElapsed);
}

TEST(world_attaches_ai_control_to_simulated_bodies) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const std::size_t bodies = world.ecs().view<sim::ecs::Person>().size() +
                                world.ecs().view<sim::ecs::Animal>().size();
    auto controlled = world.ecs().view<const sim::ai::Controlled>();
    CHECK_EQ(controlled.size(), bodies);
    world.tick();
    auto afterTick = world.ecs().view<const sim::ai::Controlled>();
    const std::size_t afterBodies = world.ecs().view<sim::ecs::Person>().size() +
                                    world.ecs().view<sim::ecs::Animal>().size();
    CHECK_EQ(afterTick.size(), afterBodies);
}

TEST(ecs_world_accepts_explicit_ai_focus_cell) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const entt::entity person = world.ecs().view<const sim::ecs::Person>().front();
    const auto& transform = world.ecs().get<const sim::ecs::Transform>(person);
    const auto focus = sim::ecs::cellForTile(core::toTile(transform.value),
                                             sim::ecs::kSpatialCellExtent);
    world.setAiFocus(focus);
    world.tick();
    CHECK_EQ(world.ecs().get<const sim::ai::Controlled>(person).lod, sim::ai::Lod::Focus);
}

TEST(ecs_render_extraction_builds_sorted_neutral_batches) {
    sim::ecs::Registry registry;
    const entt::entity high = registry.create();
    registry.emplace<sim::ecs::Position>(high, core::WorldPos{});
    registry.emplace<sim::ecs::SpriteRender>(high, core::DefId{}, std::uint8_t{3}, true);
    const entt::entity low = registry.create();
    registry.emplace<sim::ecs::Position>(low, core::WorldPos{});
    registry.emplace<sim::ecs::SpriteRender>(low, core::DefId{}, std::uint8_t{1}, true);
    const entt::entity hidden = registry.create();
    registry.emplace<sim::ecs::Position>(hidden, core::WorldPos{});
    registry.emplace<sim::ecs::SpriteRender>(hidden, core::DefId{}, std::uint8_t{2}, false);

    const auto frame = sim::ecs::render::extract(registry);
    CHECK_EQ(frame.sprites.size(), std::size_t{2});
    CHECK_EQ(frame.sprites[0].id, static_cast<sim::ecs::render::RenderEntityId>(entt::to_entity(low)));
    CHECK_EQ(frame.sprites[1].id, static_cast<sim::ecs::render::RenderEntityId>(entt::to_entity(high)));
}

TEST(ecs_render_extraction_uses_stable_domain_ids) {
    sim::ecs::Registry registry;
    const entt::entity animal = registry.create();
    registry.emplace<sim::ecs::Identity>(animal, sim::ecs::Kind::Animal, std::uint32_t{7});
    registry.emplace<sim::ecs::Position>(animal, core::WorldPos{});
    registry.emplace<sim::ecs::SpriteRender>(animal, core::DefId{}, std::uint8_t{1}, true);

    const auto frame = sim::ecs::render::extract(registry);
    CHECK_EQ(frame.sprites.size(), std::size_t{1});
    CHECK_EQ(frame.sprites.front().id,
             game::render::makeRenderEntityId(game::render::EntityClass::Animal, 7));
}

TEST(world_exposes_presentation_frame_without_exposing_registry_to_renderer) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto frame = world.extractPresentationFrame();
    CHECK_EQ(frame.sprites.size(), world.ecsEntityCount());
    CHECK(frame.meshes.empty());
}

TEST(reservation_intents_resolve_conflicts_deterministically) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    constexpr std::uint64_t key = 0x1234;
    sim::ecs::CommandBuffer intents;
    intents.push(sim::ecs::Command{sim::ecs::ReserveIntent{key, sim::PersonId{7}}});
    intents.push(sim::ecs::Command{sim::ecs::ReserveIntent{key, sim::PersonId{2}}});
    world.applyReservationIntents(intents);
    CHECK(world.isReserved(key));
    CHECK(!world.isReserved(key, sim::PersonId{2}));
    CHECK(world.isReserved(key, sim::PersonId{7}));

    sim::ecs::CommandBuffer releaseAndClaim;
    releaseAndClaim.push(sim::ecs::Command{
        sim::ecs::ReleaseReservationIntent{key, sim::PersonId{2}}});
    releaseAndClaim.push(sim::ecs::Command{
        sim::ecs::ReleaseReservationIntent{key, sim::PersonId{7}}});
    releaseAndClaim.push(sim::ecs::Command{sim::ecs::ReserveIntent{key, sim::PersonId{3}}});
    world.applyReservationIntents(releaseAndClaim);
    CHECK(!world.isReserved(key, sim::PersonId{3}));

    const std::vector<std::uint64_t> pair{0x2001, 0x2002};
    CHECK(world.reserveAll(pair, sim::PersonId{4}));
    CHECK(!world.reserveAll({0x2001, 0x2003}, sim::PersonId{5}));
    CHECK(!world.isReserved(0x2003));
    CHECK(!world.reserveAll({0x2004}, sim::PersonId{}));
}

TEST(placement_intents_commit_in_stable_order) {
    const auto& db = testing::sharedContent();
    sim::World world(db, ecsConfig());
    const auto def = db.buildingByName("campfire");
    if (!def.valid()) return;
    const std::size_t before = world.buildings().size();
    sim::ecs::CommandBuffer intents;
    intents.push(sim::ecs::PlaceBlueprintIntent{def, {20, 20}, sim::SettlementId{0}, {}});
    world.applyPlacementIntents(intents);
    CHECK_EQ(world.buildings().size(), before + 1);
    const auto id = sim::BuildingId{static_cast<std::uint32_t>(before)};
    CHECK_EQ(world.building(id).origin, (core::TilePos{20, 20}));
    CHECK(world.ecsEntity(sim::ecs::Kind::Building, id.value) != entt::null);

    sim::ecs::CommandBuffer conflicting;
    conflicting.push(sim::ecs::PlaceBlueprintIntent{def, {20, 20}, sim::SettlementId{0}, {}});
    conflicting.push(sim::ecs::PlaceBlueprintIntent{def, {20, 20}, sim::SettlementId{0}, {}});
    world.applyPlacementIntents(conflicting);
    CHECK_EQ(world.buildings().size(), before + 1);
}
