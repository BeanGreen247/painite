// Ruined portals (RuinedPortalStructure and RuinedPortalPiece): a portal template buried, on the ground, in a mountain or in the nether, with its
// netherrack spreading and optional vines and leaves.
#include "legacy_piece.hpp"
#include "template.hpp"
#include <algorithm>

namespace pn::vg {

using namespace jc;

namespace {

enum class VerticalPlacement { ON_LAND_SURFACE, PARTLY_BURIED, ON_OCEAN_FLOOR, IN_MOUNTAIN, UNDERGROUND, IN_NETHER };

struct PortalProperties { bool cold; float mossiness; bool airPocket; bool overgrown; bool vines; bool replaceWithBlackstone; };

struct PortalSetup { VerticalPlacement placement; float airPocketProbability, mossiness; bool overgrown, vines, canBeCold, replaceWithBlackstone; float weight; };

HeightmapType heightmap_of(VerticalPlacement vp) { return vp == VerticalPlacement::ON_OCEAN_FLOOR ? OCEAN_FLOOR_WG : WORLD_SURFACE_WG; }

PlaceSettings portal_settings(TemplateManager& tm, Mirror mirror, Rotation rotation, VerticalPlacement verticalPlacement, const BlockPos& pivot, const PortalProperties& properties) {
    PlaceSettings s;
    s.mirror = mirror;
    s.rotation = rotation;
    s.pivot = pivot;
    s.processors.push_back(block_ignore_processor(!properties.airPocket, true));
    std::vector<SimpleRule> rules;
    rules.push_back({"minecraft:gold_block", 0.3f, "minecraft:air"});
    if (verticalPlacement == VerticalPlacement::ON_OCEAN_FLOOR) rules.push_back({"minecraft:lava", -1.0f, "minecraft:magma_block"});
    else if (properties.cold) rules.push_back({"minecraft:lava", -1.0f, "minecraft:netherrack"});
    else rules.push_back({"minecraft:lava", 0.2f, "minecraft:magma_block"});
    if (!properties.cold) rules.push_back({"minecraft:netherrack", 0.07f, "minecraft:magma_block"});
    s.processors.push_back(block_replace_rules_processor(tm.bd(), rules));
    s.processors.push_back(block_age_processor(properties.mossiness));
    s.processors.push_back(protected_blocks_processor("#minecraft:features_cannot_replace"));
    s.processors.push_back(lava_submerged_processor());
    if (properties.replaceWithBlackstone) s.processors.push_back(blackstone_replace_processor());
    return s;
}

class RuinedPortalPiece : public LegacyPiece {
public:
    std::string templateName;
    const StructureTemplate* tmpl = nullptr;
    PlaceSettings settings;
    BlockPos templatePosition;
    VerticalPlacement verticalPlacement;
    PortalProperties properties;

    RuinedPortalPiece(TemplateManager& tm, const BlockPos& position, VerticalPlacement vp, const PortalProperties& props, const std::string& location, Rotation rotation, Mirror mirror, const BlockPos& pivot)
        : verticalPlacement(vp), properties(props) {
        type = "minecraft:rupo";
        gen_depth = 0;
        tmpl = tm.get(location);
        templateName = location;
        settings = portal_settings(tm, mirror, rotation, vp, pivot, props);
        templatePosition = position;
        if (tmpl) bb = tmpl->bounding_box(position, rotation, pivot, mirror);
        setOrientation(Direction::NORTH);
        this->rotation = rotation;
    }

    void post_process(PieceCtx& ctx, const BlockPos& pivot) override {
        if (!tmpl) return;
        BoundingBox box = tmpl->bounding_box(templatePosition, settings.rotation, settings.pivot, settings.mirror);
        if (!ctx.chunk_bb.inside(box.center())) return;
        ctx.chunk_bb.encapsulate(box);
        JLevel level(ctx.level, ctx);
        JRandom random(ctx.random);
        settings.has_bb = true;
        settings.bb = ctx.chunk_bb;
        settings.level_seed = level.getSeed();
        bb = tmpl->bounding_box(templatePosition, settings.rotation, settings.pivot, settings.mirror);
        if (tmpl->place_with(level.L, jc::bd(), ctx.senv.templates, templatePosition, pivot, settings, random.r())) {
            for (auto& jig : tmpl->filter_blocks(jc::bd(), templatePosition, settings, "minecraft:jigsaw", true)) {
                if (!jig.nbt) continue;
                std::string fs = jig.nbt->gets("final_state");
                if (fs.empty()) fs = "minecraft:air";
                level.L.set(jig.pos, parse_state_string(jc::bd(), fs));
            }
        }
        spreadNetherrack(random, level);
        addNetherrackDripColumnsBelowPortal(random, level);
        if (properties.vines || properties.overgrown) {
            // BlockPos.betweenClosedStream(box): x fastest, then y, then z
            for (int z = bb.z0; z <= bb.z1; ++z)
                for (int y = bb.y0; y <= bb.y1; ++y)
                    for (int x = bb.x0; x <= bb.x1; ++x) {
                        BlockPos pos(x, y, z);
                        if (properties.vines) maybeAddVines(random, level, pos);
                        if (properties.overgrown) maybeAddLeavesAbove(random, level, pos);
                    }
        }
    }
    void postProcess(JLevel&, JRandom, const BoundingBox&, const ChunkPos&, const BlockPos&) override {}

private:
    void maybeAddVines(JRandom random, JLevel& level, const BlockPos& pos) {

        BS state = level.getBlockState(pos);
        if (state.isAir() || state.is(BlockRef("minecraft:vine"))) {
            return;
        }
        Direction direction = Direction::randomHorizontal(random.r());
        BlockPos neighbourPos(pos.x + direction.getStepX(), pos.y, pos.z + direction.getStepZ());
        BS neighourState = level.getBlockState(neighbourPos);
        if (!neighourState.isAir()) {
            return;
        }
        if (!jc::bd().collision_face_full(state.id, direction.v)) {
            return;
        }
        std::string vineDir = direction.getOpposite().getName();
        level.setBlock(neighbourPos, blk("minecraft:vine").setValue(vineDir, true), 3);
    }
    void maybeAddLeavesAbove(JRandom random, JLevel& level, const BlockPos& pos) {

        if (random.nextFloat() < 0.5f && level.getBlockState(pos).is(BlockRef("minecraft:netherrack")) && level.getBlockState(BlockPos(pos.x, pos.y + 1, pos.z)).isAir()) {
            level.setBlock(BlockPos(pos.x, pos.y + 1, pos.z), blk("minecraft:jungle_leaves").setValue("persistent", true), 3);
        }
    }
    void addNetherrackDripColumnsBelowPortal(JRandom random, JLevel& level) {

        for (int x = bb.x0 + 1; x < bb.x1; ++x) {
            for (int z = bb.z0 + 1; z < bb.z1; ++z) {
                BlockPos pos = BlockPos(x, bb.y0, z);
                if (!level.getBlockState(pos).is(BlockRef("minecraft:netherrack"))) continue;
                addNetherrackDripColumn(random, level, pos.below());
            }
        }
    }
    void addNetherrackDripColumn(JRandom random, JLevel& level, const BlockPos& pos_in) {
        BlockPos currentPos = pos_in;
        placeNetherrackOrMagma(random, level, currentPos);
        for (int remainingCap = 8; remainingCap > 0 && random.nextFloat() < 0.5f; --remainingCap) {
            currentPos = currentPos.below();
            placeNetherrackOrMagma(random, level, currentPos);
        }
    }
    void spreadNetherrack(JRandom random, JLevel& level) {

        bool followGroundSurface = verticalPlacement == VerticalPlacement::ON_LAND_SURFACE || verticalPlacement == VerticalPlacement::ON_OCEAN_FLOOR;
        BlockPos center = bb.center();
        int centerX = center.x;
        int centerZ = center.z;
        const float netherrackProbabilityByDistance[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.9f, 0.9f, 0.8f, 0.7f, 0.6f, 0.4f, 0.2f};
        int maxDistance = 14;
        int averageWidth = (bb.x_span() + bb.z_span()) / 2;
        int distanceAdjustment = random.nextInt(std::max(1, 8 - averageWidth / 2));
        int maxYDiff = 3;
        BlockPos pos = BlockPos(0, 0, 0);
        for (int x = centerX - maxDistance; x <= centerX + maxDistance; ++x) {
            for (int z = centerZ - maxDistance; z <= centerZ + maxDistance; ++z) {
                int distance = std::abs(x - centerX) + std::abs(z - centerZ);
                int adjustedDistance = std::max(0, distance + distanceAdjustment);
                if (adjustedDistance >= maxDistance) continue;
                float probabilityOfNetherrack = netherrackProbabilityByDistance[adjustedDistance];
                if (!(random.nextDouble() < (double)probabilityOfNetherrack)) continue;
                int surfaceY = getSurfaceY(level, x, z, verticalPlacement);
                int y = followGroundSurface ? surfaceY : std::min(bb.y0, surfaceY);
                pos = BlockPos(x, y, z);
                if (std::abs(y - bb.y0) > 3 || !canBlockBeReplacedByNetherrackOrMagma(level, pos)) continue;
                placeNetherrackOrMagma(random, level, pos);
                if (properties.overgrown) {
                    maybeAddLeavesAbove(random, level, pos);
                }
                addNetherrackDripColumn(random, level, pos.below());
            }
        }
    }
    bool canBlockBeReplacedByNetherrackOrMagma(JLevel& level, const BlockPos& pos) {

        BS state = level.getBlockState(pos);
        return !state.is("minecraft:air") && !state.is(BlockRef("minecraft:obsidian")) && !state.is(Tag("#minecraft:features_cannot_replace")) && (verticalPlacement == VerticalPlacement::IN_NETHER || !state.is(BlockRef("minecraft:lava")));
    }
    void placeNetherrackOrMagma(JRandom random, JLevel& level, const BlockPos& pos) {

        if (!properties.cold && random.nextFloat() < 0.07f) {
            level.setBlock(pos, blk("minecraft:magma_block"), 3);
        } else {
            level.setBlock(pos, blk("minecraft:netherrack"), 3);
        }
    }
    static int getSurfaceY(JLevel& level, int x, int z, VerticalPlacement vp) { return level.getHeight(heightmap_of(vp), x, z) - 1; }
};

class RuinedPortalStructure : public Structure {
public:
    std::vector<PortalSetup> setups;
    static bool sample(Rng& random, float limit) {
        if (limit == 0.0f) return false;
        if (limit == 1.0f) return true;
        return random.next_float() < limit;
    }
    static int getRandomWithinInterval(JRandom random, int minPreferred, int max) {
        if (minPreferred < max) return random.nextIntBetweenInclusive(minPreferred, max);
        return max;
    }
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        JRandom random(ctx.random);
        const PortalSetup* chosen = nullptr;
        if (setups.size() > 1) {
            float total = 0.0f;
            for (auto& s : setups) total += s.weight;
            float pick = random.nextFloat();
            for (auto& s : setups) {
                if (!((pick -= s.weight / total) < 0.0f)) continue;
                chosen = &s;
                break;
            }
        } else chosen = &setups[0];
        if (!chosen) return std::nullopt;
        PortalSetup setup = *chosen;
        bool airPocket = sample(ctx.random, setup.airPocketProbability);
        std::string location;
        if (random.nextFloat() < 0.05f) location = "minecraft:ruined_portal/giant_portal_" + std::to_string(random.nextInt(3) + 1);
        else location = "minecraft:ruined_portal/portal_" + std::to_string(random.nextInt(10) + 1);
        TemplateManager& tm = *ctx.env.templates;
        const StructureTemplate* tmpl = tm.get(location);
        if (!tmpl) return std::nullopt;
        static const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
        Rotation rotation = kRots[random.nextInt(4)];
        Mirror mirror = random.nextFloat() < 0.5f ? Mirror::NONE : Mirror::FRONT_BACK;
        BlockPos pivot(tmpl->size_x / 2, 0, tmpl->size_z / 2);
        BlockPos basePosition(ctx.chunk.x * 16, 0, ctx.chunk.z * 16);
        BoundingBox boundingBox = tmpl->bounding_box(basePosition, rotation, pivot, mirror);
        BlockPos center = boundingBox.center();
        int surfaceY = ctx.env.base_height(center.x, center.z, heightmap_of(setup.placement)) - 1;
        int projectedY = findSuitableY(ctx, random, setup.placement, airPocket, surfaceY, boundingBox.y_span(), boundingBox);
        BlockPos origin(basePosition.x, projectedY, basePosition.z);
        GenerationStub stub;
        stub.position = origin;
        stub.generate = [&ctx, setup, origin, airPocket, location, rotation, mirror, pivot](std::vector<std::shared_ptr<StructurePiece>>& out) {
            PortalProperties props;
            bool cold = false;
            if (setup.canBeCold) {
                int b = ctx.env.biome_at(origin.x >> 2, origin.y >> 2, origin.z >> 2);
                cold = biome_cold_enough_to_snow((*ctx.env.biome_infos)[size_t(b)], origin.x, origin.y, origin.z, ctx.env.sea_level);
            }
            props = {cold, setup.mossiness, airPocket, setup.overgrown, setup.vines, setup.replaceWithBlackstone};
            out.push_back(std::make_shared<RuinedPortalPiece>(*ctx.env.templates, origin, setup.placement, props, location, rotation, mirror, pivot));
        };
        return stub;
    }
    static int findSuitableY(const StructureContext& ctx, JRandom random, VerticalPlacement vp, bool airPocket, int surfaceYAtCenter, int ySpan, const BoundingBox& box) {
        int minY = ctx.env.min_y + 15;
        int newY;
        if (vp == VerticalPlacement::IN_NETHER) {
            newY = airPocket ? random.nextIntBetweenInclusive(32, 100) : (random.nextFloat() < 0.5f ? random.nextIntBetweenInclusive(27, 29) : random.nextIntBetweenInclusive(29, 100));
        } else if (vp == VerticalPlacement::IN_MOUNTAIN) {
            int maxY = surfaceYAtCenter - ySpan;
            newY = getRandomWithinInterval(random, 70, maxY);
        } else if (vp == VerticalPlacement::UNDERGROUND) {
            int maxY = surfaceYAtCenter - ySpan;
            newY = getRandomWithinInterval(random, minY, maxY);
        } else {
            newY = vp == VerticalPlacement::PARTLY_BURIED ? surfaceYAtCenter - ySpan + random.nextIntBetweenInclusive(2, 8) : surfaceYAtCenter;
        }
        BlockPos corners[4] = {BlockPos(box.x0, 0, box.z0), BlockPos(box.x1, 0, box.z0), BlockPos(box.x0, 0, box.z1), BlockPos(box.x1, 0, box.z1)};
        std::vector<uint16_t> columns[4];
        for (int i = 0; i < 4; i++) ctx.env.base_column(corners[i].x, corners[i].z, columns[i]);
        const uint8_t* flags = ctx.env.bd->chunk_flags();
        HeightmapType hm = vp == VerticalPlacement::ON_OCEAN_FLOOR ? OCEAN_FLOOR_WG : WORLD_SURFACE_WG;
        int projectedY;
        for (projectedY = newY; projectedY > minY; --projectedY) {
            int cornersOnSolidGround = 0;
            bool done = false;
            for (auto& column : columns) {
                int idx = projectedY - ctx.env.min_y;
                uint16_t st = idx >= 0 && size_t(idx) < column.size() ? column[size_t(idx)] : uint16_t(0);
                bool opaque = hm == WORLD_SURFACE_WG ? !(flags[st] & kFlagAir) : (flags[st] & kFlagBlocksMotion) != 0;
                if (!opaque || ++cornersOnSolidGround != 3) continue;
                done = true;
                break;
            }
            if (done) break;
        }
        return projectedY;
    }
};

struct RegisterPortal {
    RegisterPortal() {
        structure_factories()["minecraft:ruined_portal"] = [](const Json& j, const Registry&) -> std::unique_ptr<Structure> {
            auto s = std::make_unique<RuinedPortalStructure>();
            for (auto& sj : j.at("setups").arr) {
                PortalSetup p;
                std::string pl = sj.get_string("placement");
                p.placement = pl == "partly_buried" ? VerticalPlacement::PARTLY_BURIED : pl == "on_ocean_floor" ? VerticalPlacement::ON_OCEAN_FLOOR : pl == "in_mountain" ? VerticalPlacement::IN_MOUNTAIN :
                              pl == "underground" ? VerticalPlacement::UNDERGROUND : pl == "in_nether" ? VerticalPlacement::IN_NETHER : VerticalPlacement::ON_LAND_SURFACE;
                p.airPocketProbability = sj.at("air_pocket_probability").float_value();
                p.mossiness = sj.at("mossiness").float_value();
                p.overgrown = sj.get_bool("overgrown", false);
                p.vines = sj.get_bool("vines", false);
                p.canBeCold = sj.get_bool("can_be_cold", false);
                p.replaceWithBlackstone = sj.get_bool("replace_with_blackstone", false);
                p.weight = sj.at("weight").float_value();
                s->setups.push_back(p);
            }
            return s;
        };
    }
} register_portal;

}  // namespace

}  // namespace pn::vg
