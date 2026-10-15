// Woodland mansions (WoodlandMansionStructure and WoodlandMansionPieces): a grid of corridors and rooms, walls and roofs placed from templates.
#include "template_piece.hpp"
#include <algorithm>

namespace pn::vg {

using namespace jc;

namespace {

const Rotation kRotNone = Rotation::NONE;
Rotation rot_add(Rotation a, Rotation b) { return Rotation((int(a) + int(b)) & 3); }   // Rotation.getRotated

Direction rot_dir(Rotation r, const Direction& d) {   // Rotation.rotate(Direction)
    if (d.axis() == 'y') return d;
    switch (r) {
        case Rotation::CLOCKWISE_180: return d.getOpposite();
        case Rotation::COUNTERCLOCKWISE_90: return d.getCounterClockWise();
        case Rotation::CLOCKWISE_90: return d.getClockWise();
        default: return d;
    }
}

BlockPos rel(const BlockPos& p, const Direction& d, int n) { return BlockPos(p.x + d.getStepX() * n, p.y + d.getStepY() * n, p.z + d.getStepZ() * n); }

BlockPos rotate_pos(const BlockPos& p, Rotation r) {   // BlockPos.rotate
    switch (r) {
        case Rotation::CLOCKWISE_90: return BlockPos(-p.z, p.y, p.x);
        case Rotation::CLOCKWISE_180: return BlockPos(-p.x, p.y, -p.z);
        case Rotation::COUNTERCLOCKWISE_90: return BlockPos(p.z, p.y, -p.x);
        default: return p;
    }
}

PlaceSettings mansion_settings(Mirror mirror, Rotation rotation) {
    PlaceSettings s;
    s.ignore_entities = true;
    s.rotation = rotation;
    s.mirror = mirror;
    s.processors.push_back(block_ignore_processor(false, true));
    return s;
}

class WoodlandMansionPiece : public TemplatePiece {
public:
    WoodlandMansionPiece(TemplateManager& tm, const std::string& templateName, const BlockPos& position, Rotation rotation, Mirror mirror = Mirror::NONE)
        : TemplatePiece("minecraft:wmp", 0, tm, "minecraft:woodland_mansion/" + templateName, mansion_settings(mirror, rotation), position) {}
protected:
    void handleDataMarker(const std::string& markerId, const BlockPos& position, JLevel& level, JRandom random, const BoundingBox& chunkBB) override {
        if (markerId.rfind("Chest", 0) == 0) {
            Rotation rot = settings.rotation;
            BS chestState = blk("minecraft:chest");
            if (markerId == "ChestWest") chestState = chestState.setValue("facing", rot_dir(rot, Direction::WEST));
            else if (markerId == "ChestEast") chestState = chestState.setValue("facing", rot_dir(rot, Direction::EAST));
            else if (markerId == "ChestSouth") chestState = chestState.setValue("facing", rot_dir(rot, Direction::SOUTH));
            else if (markerId == "ChestNorth") chestState = chestState.setValue("facing", rot_dir(rot, Direction::NORTH));
            createChest(level, chunkBB, random, position, "minecraft:chests/woodland_mansion", &chestState);
        } else {
            std::vector<const char*> mobs;
            if (markerId == "Mage") mobs.push_back("minecraft:evoker");
            else if (markerId == "Warrior") mobs.push_back("minecraft:vindicator");
            else if (markerId == "Group of Allays") {
                int n = levelRandom(level).nextInt(3) + 1;
                for (int i = 0; i < n; ++i) mobs.push_back("minecraft:allay");
            } else return;
            for (const char* m : mobs) {
                add_entity(level, m, double(position.x) + 0.5, position.y, double(position.z) + 0.5);
                level.L.set(position, blk("minecraft:air").id);
            }
        }
    }
};

struct SimpleGrid {
    int width, height, valueIfOutside;
    std::vector<std::vector<int>> grid;
    SimpleGrid(int w, int h, int outside) : width(w), height(h), valueIfOutside(outside), grid(size_t(w), std::vector<int>(size_t(h), 0)) {}
    void set(int x, int y, int value) { if (x >= 0 && x < width && y >= 0 && y < height) grid[size_t(x)][size_t(y)] = value; }
    void set(int x0, int y0, int x1, int y1, int value) { for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) set(x, y, value); }
    int get(int x, int y) const { return (x >= 0 && x < width && y >= 0 && y < height) ? grid[size_t(x)][size_t(y)] : valueIfOutside; }
    void setif(int x, int y, int ifValue, int value) { if (get(x, y) == ifValue) set(x, y, value); }
    bool edgesTo(int x, int y, int ifValue) const { return get(x - 1, y) == ifValue || get(x + 1, y) == ifValue || get(x, y + 1) == ifValue || get(x, y - 1) == ifValue; }
};

struct GridPos { int x, y; };

void shuffle_list(std::vector<GridPos>& list, JRandom random) {
    for (int i = int(list.size()); i > 1; --i) std::swap(list[size_t(i - 1)], list[size_t(random.nextInt(i))]);
}

struct MansionGrid {
    JRandom random;
    SimpleGrid baseGrid, thirdFloorGrid;
    std::vector<SimpleGrid> floorRooms;
    int entranceX, entranceY;

    static bool isHouse(const SimpleGrid& grid, int x, int y) { int v = grid.get(x, y); return v == 1 || v == 2 || v == 3 || v == 4; }

    explicit MansionGrid(JRandom r) : random(r), baseGrid(11, 11, 5), thirdFloorGrid(11, 11, 5) {
        entranceX = 7;
        entranceY = 4;
        baseGrid.set(entranceX, entranceY, entranceX + 1, entranceY + 1, 3);
        baseGrid.set(entranceX - 1, entranceY, entranceX - 1, entranceY + 1, 2);
        baseGrid.set(entranceX + 2, entranceY - 2, entranceX + 3, entranceY + 3, 5);
        baseGrid.set(entranceX + 1, entranceY - 2, entranceX + 1, entranceY - 1, 1);
        baseGrid.set(entranceX + 1, entranceY + 2, entranceX + 1, entranceY + 3, 1);
        baseGrid.set(entranceX - 1, entranceY - 1, 1);
        baseGrid.set(entranceX - 1, entranceY + 2, 1);
        baseGrid.set(0, 0, 11, 1, 5);
        baseGrid.set(0, 9, 11, 11, 5);
        recursiveCorridor(baseGrid, entranceX, entranceY - 2, Direction::WEST, 6);
        recursiveCorridor(baseGrid, entranceX, entranceY + 3, Direction::WEST, 6);
        recursiveCorridor(baseGrid, entranceX - 2, entranceY - 1, Direction::WEST, 3);
        recursiveCorridor(baseGrid, entranceX - 2, entranceY + 2, Direction::WEST, 3);
        while (cleanEdges(baseGrid)) {}
        floorRooms.assign(3, SimpleGrid(11, 11, 5));
        identifyRooms(baseGrid, floorRooms[0]);
        identifyRooms(baseGrid, floorRooms[1]);
        floorRooms[0].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 0x800000);
        floorRooms[1].set(entranceX + 1, entranceY, entranceX + 1, entranceY + 1, 0x800000);
        thirdFloorGrid = SimpleGrid(baseGrid.width, baseGrid.height, 5);
        setupThirdFloor();
        identifyRooms(thirdFloorGrid, floorRooms[2]);
    }

    bool isRoomId(int x, int y, int floor, int roomId) const { return (floorRooms[size_t(floor)].get(x, y) & 0xFFFF) == roomId; }

    bool get1x2RoomDirection(int x, int y, int floorNum, int roomId, Direction& out) const {
        for (const Direction& direction : Direction::HORIZONTAL) {
            if (!isRoomId(x + direction.getStepX(), y + direction.getStepZ(), floorNum, roomId)) continue;
            out = direction;
            return true;
        }
        return false;
    }

    void recursiveCorridor(SimpleGrid& grid, int x, int y, const Direction& heading, int depth) {
        if (depth <= 0) return;
        grid.set(x, y, 1);
        grid.setif(x + heading.getStepX(), y + heading.getStepZ(), 0, 1);
        for (int attempts = 0; attempts < 8; ++attempts) {
            Direction nextDir = Direction::from2DDataValue(random.nextInt(4));
            if (nextDir == heading.getOpposite() || (nextDir == Direction::EAST && random.nextBoolean())) continue;
            int nx = x + heading.getStepX();
            int ny = y + heading.getStepZ();
            if (grid.get(nx + nextDir.getStepX(), ny + nextDir.getStepZ()) != 0 || grid.get(nx + nextDir.getStepX() * 2, ny + nextDir.getStepZ() * 2) != 0) continue;
            recursiveCorridor(grid, x + heading.getStepX() + nextDir.getStepX(), y + heading.getStepZ() + nextDir.getStepZ(), nextDir, depth - 1);
            break;
        }
        Direction cw = heading.getClockWise();
        Direction ccw = heading.getCounterClockWise();
        grid.setif(x + cw.getStepX(), y + cw.getStepZ(), 0, 2);
        grid.setif(x + ccw.getStepX(), y + ccw.getStepZ(), 0, 2);
        grid.setif(x + heading.getStepX() + cw.getStepX(), y + heading.getStepZ() + cw.getStepZ(), 0, 2);
        grid.setif(x + heading.getStepX() + ccw.getStepX(), y + heading.getStepZ() + ccw.getStepZ(), 0, 2);
        grid.setif(x + heading.getStepX() * 2, y + heading.getStepZ() * 2, 0, 2);
        grid.setif(x + cw.getStepX() * 2, y + cw.getStepZ() * 2, 0, 2);
        grid.setif(x + ccw.getStepX() * 2, y + ccw.getStepZ() * 2, 0, 2);
    }

    bool cleanEdges(SimpleGrid& grid) {
        bool touched = false;
        for (int y = 0; y < grid.height; ++y)
            for (int x = 0; x < grid.width; ++x) {
                if (grid.get(x, y) != 0) continue;
                int directNeighbors = 0;
                directNeighbors += isHouse(grid, x + 1, y) ? 1 : 0;
                directNeighbors += isHouse(grid, x - 1, y) ? 1 : 0;
                directNeighbors += isHouse(grid, x, y + 1) ? 1 : 0;
                directNeighbors += isHouse(grid, x, y - 1) ? 1 : 0;
                if (directNeighbors >= 3) { grid.set(x, y, 2); touched = true; continue; }
                if (directNeighbors != 2) continue;
                int diagonalNeighbors = 0;
                diagonalNeighbors += isHouse(grid, x + 1, y + 1) ? 1 : 0;
                diagonalNeighbors += isHouse(grid, x - 1, y + 1) ? 1 : 0;
                diagonalNeighbors += isHouse(grid, x + 1, y - 1) ? 1 : 0;
                diagonalNeighbors += isHouse(grid, x - 1, y - 1) ? 1 : 0;
                if (diagonalNeighbors > 1) continue;
                grid.set(x, y, 2);
                touched = true;
            }
        return touched;
    }

    void setupThirdFloor() {
        std::vector<GridPos> potentialRooms;
        SimpleGrid& floor = floorRooms[1];
        for (int y = 0; y < thirdFloorGrid.height; ++y)
            for (int x = 0; x < thirdFloorGrid.width; ++x) {
                int roomData = floor.get(x, y);
                int roomType = roomData & 0xF0000;
                if (roomType != 131072 || (roomData & 0x200000) != 0x200000) continue;
                potentialRooms.push_back({x, y});
            }
        if (potentialRooms.empty()) { thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5); return; }
        GridPos roomPos = potentialRooms[size_t(random.nextInt(int(potentialRooms.size())))];
        int roomData = floor.get(roomPos.x, roomPos.y);
        floor.set(roomPos.x, roomPos.y, roomData | 0x400000);
        Direction roomDir;
        get1x2RoomDirection(roomPos.x, roomPos.y, 1, roomData & 0xFFFF, roomDir);
        int roomEndX = roomPos.x + roomDir.getStepX();
        int roomEndY = roomPos.y + roomDir.getStepZ();
        for (int y = 0; y < thirdFloorGrid.height; ++y)
            for (int x = 0; x < thirdFloorGrid.width; ++x) {
                if (!isHouse(baseGrid, x, y)) { thirdFloorGrid.set(x, y, 5); continue; }
                if (x == roomPos.x && y == roomPos.y) { thirdFloorGrid.set(x, y, 3); continue; }
                if (x != roomEndX || y != roomEndY) continue;
                thirdFloorGrid.set(x, y, 3);
                floorRooms[2].set(x, y, 0x800000);
            }
        std::vector<Direction> potentialCorridors;
        for (const Direction& direction : Direction::HORIZONTAL) {
            if (thirdFloorGrid.get(roomEndX + direction.getStepX(), roomEndY + direction.getStepZ()) != 0) continue;
            potentialCorridors.push_back(direction);
        }
        if (potentialCorridors.empty()) {
            thirdFloorGrid.set(0, 0, thirdFloorGrid.width, thirdFloorGrid.height, 5);
            floor.set(roomPos.x, roomPos.y, roomData);
            return;
        }
        Direction corridorDir = potentialCorridors[size_t(random.nextInt(int(potentialCorridors.size())))];
        recursiveCorridor(thirdFloorGrid, roomEndX + corridorDir.getStepX(), roomEndY + corridorDir.getStepZ(), corridorDir, 4);
        while (cleanEdges(thirdFloorGrid)) {}
    }

    void identifyRooms(SimpleGrid& fromGrid, SimpleGrid& roomGrid) {
        std::vector<GridPos> roomPos;
        for (int y = 0; y < fromGrid.height; ++y)
            for (int x = 0; x < fromGrid.width; ++x) {
                if (fromGrid.get(x, y) != 2) continue;
                roomPos.push_back({x, y});
            }
        shuffle_list(roomPos, random);
        int roomId = 10;
        for (GridPos pos : roomPos) {
            int x = pos.x, y = pos.y;
            if (roomGrid.get(x, y) != 0) continue;
            int x0 = x, x1 = x, y0 = y, y1 = y;
            int type = 65536;
            if (roomGrid.get(x + 1, y) == 0 && roomGrid.get(x, y + 1) == 0 && roomGrid.get(x + 1, y + 1) == 0 && fromGrid.get(x + 1, y) == 2 && fromGrid.get(x, y + 1) == 2 && fromGrid.get(x + 1, y + 1) == 2) {
                ++x1; ++y1; type = 262144;
            } else if (roomGrid.get(x - 1, y) == 0 && roomGrid.get(x, y + 1) == 0 && roomGrid.get(x - 1, y + 1) == 0 && fromGrid.get(x - 1, y) == 2 && fromGrid.get(x, y + 1) == 2 && fromGrid.get(x - 1, y + 1) == 2) {
                --x0; ++y1; type = 262144;
            } else if (roomGrid.get(x - 1, y) == 0 && roomGrid.get(x, y - 1) == 0 && roomGrid.get(x - 1, y - 1) == 0 && fromGrid.get(x - 1, y) == 2 && fromGrid.get(x, y - 1) == 2 && fromGrid.get(x - 1, y - 1) == 2) {
                --x0; --y0; type = 262144;
            } else if (roomGrid.get(x + 1, y) == 0 && fromGrid.get(x + 1, y) == 2) {
                ++x1; type = 131072;
            } else if (roomGrid.get(x, y + 1) == 0 && fromGrid.get(x, y + 1) == 2) {
                ++y1; type = 131072;
            } else if (roomGrid.get(x - 1, y) == 0 && fromGrid.get(x - 1, y) == 2) {
                --x0; type = 131072;
            } else if (roomGrid.get(x, y - 1) == 0 && fromGrid.get(x, y - 1) == 2) {
                --y0; type = 131072;
            }
            int doorX = random.nextBoolean() ? x0 : x1;
            int doorY = random.nextBoolean() ? y0 : y1;
            int doorFlag = 0x200000;
            if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                doorX = doorX == x0 ? x1 : x0;
                doorY = doorY == y0 ? y1 : y0;
                if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                    doorY = doorY == y0 ? y1 : y0;
                    if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                        doorX = doorX == x0 ? x1 : x0;
                        doorY = doorY == y0 ? y1 : y0;
                        if (!fromGrid.edgesTo(doorX, doorY, 1)) {
                            doorFlag = 0;
                            doorX = x0;
                            doorY = y0;
                        }
                    }
                }
            }
            for (int ry = y0; ry <= y1; ++ry)
                for (int rx = x0; rx <= x1; ++rx) {
                    if (rx == doorX && ry == doorY) { roomGrid.set(rx, ry, 0x100000 | doorFlag | type | roomId); continue; }
                    roomGrid.set(rx, ry, type | roomId);
                }
            ++roomId;
        }
    }
};

struct FloorRooms {
    int floor;   // 0 first, 1 second, 2 third (the third uses the second's names)
    std::string get1x1(JRandom r) const { return floor == 0 ? "1x1_a" + std::to_string(r.nextInt(5) + 1) : "1x1_b" + std::to_string(r.nextInt(5) + 1); }
    std::string get1x1Secret(JRandom r) const { return "1x1_as" + std::to_string(r.nextInt(4) + 1); }
    std::string get1x2SideEntrance(JRandom r, bool stairs) const {
        if (floor == 0) return "1x2_a" + std::to_string(r.nextInt(9) + 1);
        if (stairs) return "1x2_c_stairs";
        return "1x2_c" + std::to_string(r.nextInt(4) + 1);
    }
    std::string get1x2FrontEntrance(JRandom r, bool stairs) const {
        if (floor == 0) return "1x2_b" + std::to_string(r.nextInt(5) + 1);
        if (stairs) return "1x2_d_stairs";
        return "1x2_d" + std::to_string(r.nextInt(5) + 1);
    }
    std::string get1x2Secret(JRandom r) const { return floor == 0 ? "1x2_s" + std::to_string(r.nextInt(2) + 1) : "1x2_se" + std::to_string(r.nextInt(1) + 1); }
    std::string get2x2(JRandom r) const { return floor == 0 ? "2x2_a" + std::to_string(r.nextInt(4) + 1) : "2x2_b" + std::to_string(r.nextInt(5) + 1); }
    std::string get2x2Secret(JRandom) const { return "2x2_s1"; }
};

struct PlacementData {
    Rotation rotation = Rotation::NONE;
    BlockPos position;
    std::string wallType;
};

class MansionPiecePlacer {
public:
    TemplateManager& tm;
    JRandom random;
    int startX = 0, startY = 0;
    MansionPiecePlacer(TemplateManager& t, JRandom r) : tm(t), random(r) {}
    using Pieces = std::vector<std::shared_ptr<StructurePiece>>;

    void add(Pieces& pieces, const std::string& name, const BlockPos& pos, Rotation rot, Mirror mirror = Mirror::NONE) {
        pieces.push_back(std::make_shared<WoodlandMansionPiece>(tm, name, pos, rot, mirror));
    }

    void createMansion(const BlockPos& origin, Rotation rotation, Pieces& pieces, MansionGrid& mansion) {
        PlacementData data;
        data.position = origin;
        data.rotation = rotation;
        data.wallType = "wall_flat";
        PlacementData secondData;
        entrance(pieces, data);
        secondData.position = data.position.above(8);
        secondData.rotation = data.rotation;
        secondData.wallType = "wall_window";
        SimpleGrid& baseGrid = mansion.baseGrid;
        SimpleGrid& thirdGrid = mansion.thirdFloorGrid;
        startX = mansion.entranceX + 1;
        startY = mansion.entranceY + 1;
        int endX = mansion.entranceX + 1;
        int endY = mansion.entranceY;
        traverseOuterWalls(pieces, data, baseGrid, Direction::SOUTH, startX, startY, endX, endY);
        traverseOuterWalls(pieces, secondData, baseGrid, Direction::SOUTH, startX, startY, endX, endY);
        PlacementData thirdData;
        thirdData.position = data.position.above(19);
        thirdData.rotation = data.rotation;
        thirdData.wallType = "wall_window";
        bool done = false;
        for (int y = 0; y < thirdGrid.height && !done; ++y)
            for (int x = thirdGrid.width - 1; x >= 0 && !done; --x) {
                if (!MansionGrid::isHouse(thirdGrid, x, y)) continue;
                thirdData.position = rel(thirdData.position, rot_dir(rotation, Direction::SOUTH), 8 + (y - startY) * 8);
                thirdData.position = rel(thirdData.position, rot_dir(rotation, Direction::EAST), (x - startX) * 8);
                traverseWallPiece(pieces, thirdData);
                traverseOuterWalls(pieces, thirdData, thirdGrid, Direction::SOUTH, x, y, x, y);
                done = true;
            }
        createRoof(pieces, origin.above(16), rotation, baseGrid, &thirdGrid);
        createRoof(pieces, origin.above(27), rotation, thirdGrid, nullptr);
        FloorRooms collections[3] = {{0}, {1}, {1}};
        for (int floorNum = 0; floorNum < 3; ++floorNum) {
            BlockPos floorOrigin = origin.above(8 * floorNum + (floorNum == 2 ? 3 : 0));
            SimpleGrid& rooms = mansion.floorRooms[size_t(floorNum)];
            SimpleGrid& grid = floorNum == 2 ? thirdGrid : baseGrid;
            std::string southPiece = floorNum == 0 ? "carpet_south_1" : "carpet_south_2";
            std::string westPiece = floorNum == 0 ? "carpet_west_1" : "carpet_west_2";
            for (int y = 0; y < grid.height; ++y)
                for (int x = 0; x < grid.width; ++x) {
                    if (grid.get(x, y) != 1) continue;
                    BlockPos pos = rel(floorOrigin, rot_dir(rotation, Direction::SOUTH), 8 + (y - startY) * 8);
                    pos = rel(pos, rot_dir(rotation, Direction::EAST), (x - startX) * 8);
                    add(pieces, "corridor_floor", pos, rotation);
                    if (grid.get(x, y - 1) == 1 || (rooms.get(x, y - 1) & 0x800000) == 0x800000) add(pieces, "carpet_north", rel(pos, rot_dir(rotation, Direction::EAST), 1).above(), rotation);
                    if (grid.get(x + 1, y) == 1 || (rooms.get(x + 1, y) & 0x800000) == 0x800000) add(pieces, "carpet_east", rel(rel(pos, rot_dir(rotation, Direction::SOUTH), 1), rot_dir(rotation, Direction::EAST), 5).above(), rotation);
                    if (grid.get(x, y + 1) == 1 || (rooms.get(x, y + 1) & 0x800000) == 0x800000) add(pieces, southPiece, rel(rel(pos, rot_dir(rotation, Direction::SOUTH), 5), rot_dir(rotation, Direction::WEST), 1), rotation);
                    if (grid.get(x - 1, y) != 1 && (rooms.get(x - 1, y) & 0x800000) != 0x800000) continue;
                    add(pieces, westPiece, rel(rel(pos, rot_dir(rotation, Direction::WEST), 1), rot_dir(rotation, Direction::NORTH), 1), rotation);
                }
            std::string wallPiece = floorNum == 0 ? "indoors_wall_1" : "indoors_wall_2";
            std::string doorPiece = floorNum == 0 ? "indoors_door_1" : "indoors_door_2";
            std::vector<Direction> doorDirs;
            for (int y = 0; y < grid.height; ++y)
                for (int x = 0; x < grid.width; ++x) {
                    bool thirdFloorStartRoom = floorNum == 2 && grid.get(x, y) == 3;
                    if (grid.get(x, y) != 2 && !thirdFloorStartRoom) continue;
                    int roomData = rooms.get(x, y);
                    int roomType = roomData & 0xF0000;
                    int roomId = roomData & 0xFFFF;
                    thirdFloorStartRoom = thirdFloorStartRoom && (roomData & 0x800000) == 0x800000;
                    doorDirs.clear();
                    if ((roomData & 0x200000) == 0x200000)
                        for (const Direction& direction : Direction::HORIZONTAL) {
                            if (grid.get(x + direction.getStepX(), y + direction.getStepZ()) != 1) continue;
                            doorDirs.push_back(direction);
                        }
                    bool hasDoorDir = false;
                    Direction doorDir;
                    if (!doorDirs.empty()) { doorDir = doorDirs[size_t(random.nextInt(int(doorDirs.size())))]; hasDoorDir = true; }
                    else if ((roomData & 0x100000) == 0x100000) { doorDir = Direction::UP; hasDoorDir = true; }
                    BlockPos roomPos = rel(floorOrigin, rot_dir(rotation, Direction::SOUTH), 8 + (y - startY) * 8);
                    roomPos = rel(roomPos, rot_dir(rotation, Direction::EAST), -1 + (x - startX) * 8);
                    if (MansionGrid::isHouse(grid, x - 1, y) && !mansion.isRoomId(x - 1, y, floorNum, roomId))
                        add(pieces, hasDoorDir && doorDir == Direction::WEST ? doorPiece : wallPiece, roomPos, rotation);
                    if (grid.get(x + 1, y) == 1 && !thirdFloorStartRoom) {
                        BlockPos pos = rel(roomPos, rot_dir(rotation, Direction::EAST), 8);
                        add(pieces, hasDoorDir && doorDir == Direction::EAST ? doorPiece : wallPiece, pos, rotation);
                    }
                    if (MansionGrid::isHouse(grid, x, y + 1) && !mansion.isRoomId(x, y + 1, floorNum, roomId)) {
                        BlockPos pos = rel(roomPos, rot_dir(rotation, Direction::SOUTH), 7);
                        pos = rel(pos, rot_dir(rotation, Direction::EAST), 7);
                        add(pieces, hasDoorDir && doorDir == Direction::SOUTH ? doorPiece : wallPiece, pos, rot_add(rotation, Rotation::CLOCKWISE_90));
                    }
                    if (grid.get(x, y - 1) == 1 && !thirdFloorStartRoom) {
                        BlockPos pos = rel(roomPos, rot_dir(rotation, Direction::NORTH), 1);
                        pos = rel(pos, rot_dir(rotation, Direction::EAST), 7);
                        add(pieces, hasDoorDir && doorDir == Direction::NORTH ? doorPiece : wallPiece, pos, rot_add(rotation, Rotation::CLOCKWISE_90));
                    }
                    if (roomType == 65536) { addRoom1x1(pieces, roomPos, rotation, hasDoorDir, doorDir, collections[floorNum]); continue; }
                    if (roomType == 131072 && hasDoorDir) {
                        Direction roomDir;
                        mansion.get1x2RoomDirection(x, y, floorNum, roomId, roomDir);
                        bool isStairsRoom = (roomData & 0x400000) == 0x400000;
                        addRoom1x2(pieces, roomPos, rotation, roomDir, doorDir, collections[floorNum], isStairsRoom);
                        continue;
                    }
                    if (roomType == 262144 && hasDoorDir && doorDir != Direction::UP) {
                        Direction roomDir = doorDir.getClockWise();
                        if (!mansion.isRoomId(x + roomDir.getStepX(), y + roomDir.getStepZ(), floorNum, roomId)) roomDir = roomDir.getOpposite();
                        addRoom2x2(pieces, roomPos, rotation, roomDir, doorDir, collections[floorNum]);
                        continue;
                    }
                    if (roomType != 262144 || !hasDoorDir || doorDir != Direction::UP) continue;
                    addRoom2x2Secret(pieces, roomPos, rotation, collections[floorNum]);
                }
        }
    }

    void traverseOuterWalls(Pieces& pieces, PlacementData& data, const SimpleGrid& grid, Direction gridDirection, int startX_, int startY_, int endX, int endY) {
        int gridX = startX_, gridY = startY_;
        Direction startDirection = gridDirection;
        do {
            if (!MansionGrid::isHouse(grid, gridX + gridDirection.getStepX(), gridY + gridDirection.getStepZ())) {
                traverseTurn(pieces, data);
                gridDirection = gridDirection.getClockWise();
                if (gridX == endX && gridY == endY && startDirection == gridDirection) continue;
                traverseWallPiece(pieces, data);
                continue;
            }
            if (MansionGrid::isHouse(grid, gridX + gridDirection.getStepX(), gridY + gridDirection.getStepZ()) &&
                MansionGrid::isHouse(grid, gridX + gridDirection.getStepX() + gridDirection.getCounterClockWise().getStepX(), gridY + gridDirection.getStepZ() + gridDirection.getCounterClockWise().getStepZ())) {
                traverseInnerTurn(pieces, data);
                gridX += gridDirection.getStepX();
                gridY += gridDirection.getStepZ();
                gridDirection = gridDirection.getCounterClockWise();
                continue;
            }
            gridX += gridDirection.getStepX();
            gridY += gridDirection.getStepZ();
            if (gridX == endX && gridY == endY && startDirection == gridDirection) continue;
            traverseWallPiece(pieces, data);
        } while (gridX != endX || gridY != endY || startDirection != gridDirection);
    }

    void createRoof(Pieces& pieces, const BlockPos& roofOrigin, Rotation rotation, const SimpleGrid& grid, const SimpleGrid* aboveGrid) {
        auto S = [&](const BlockPos& p, int n) { return rel(p, rot_dir(rotation, Direction::SOUTH), n); };
        auto E = [&](const BlockPos& p, int n) { return rel(p, rot_dir(rotation, Direction::EAST), n); };
        auto W = [&](const BlockPos& p, int n) { return rel(p, rot_dir(rotation, Direction::WEST), n); };
        auto N = [&](const BlockPos& p, int n) { return rel(p, rot_dir(rotation, Direction::NORTH), n); };
        for (int y = 0; y < grid.height; ++y)
            for (int x = 0; x < grid.width; ++x) {
                BlockPos position = S(roofOrigin, 8 + (y - startY) * 8);
                position = E(position, (x - startX) * 8);
                bool isAbove = aboveGrid && MansionGrid::isHouse(*aboveGrid, x, y);
                if (!MansionGrid::isHouse(grid, x, y) || isAbove) continue;
                add(pieces, "roof", position.above(3), rotation);
                BlockPos p2;
                if (!MansionGrid::isHouse(grid, x + 1, y)) { p2 = E(position, 6); add(pieces, "roof_front", p2, rotation); }
                if (!MansionGrid::isHouse(grid, x - 1, y)) { p2 = E(position, 0); p2 = S(p2, 7); add(pieces, "roof_front", p2, rot_add(rotation, Rotation::CLOCKWISE_180)); }
                if (!MansionGrid::isHouse(grid, x, y - 1)) { p2 = W(position, 1); add(pieces, "roof_front", p2, rot_add(rotation, Rotation::COUNTERCLOCKWISE_90)); }
                if (MansionGrid::isHouse(grid, x, y + 1)) continue;
                p2 = E(position, 6); p2 = S(p2, 6);
                add(pieces, "roof_front", p2, rot_add(rotation, Rotation::CLOCKWISE_90));
            }
        if (aboveGrid) {
            for (int y = 0; y < grid.height; ++y)
                for (int x = 0; x < grid.width; ++x) {
                    BlockPos position = S(roofOrigin, 8 + (y - startY) * 8);
                    position = E(position, (x - startX) * 8);
                    bool isAbove = MansionGrid::isHouse(*aboveGrid, x, y);
                    if (!MansionGrid::isHouse(grid, x, y) || !isAbove) continue;
                    BlockPos p2;
                    if (!MansionGrid::isHouse(grid, x + 1, y)) { p2 = E(position, 7); add(pieces, "small_wall", p2, rotation); }
                    if (!MansionGrid::isHouse(grid, x - 1, y)) { p2 = W(position, 1); p2 = S(p2, 6); add(pieces, "small_wall", p2, rot_add(rotation, Rotation::CLOCKWISE_180)); }
                    if (!MansionGrid::isHouse(grid, x, y - 1)) { p2 = W(position, 0); p2 = N(p2, 1); add(pieces, "small_wall", p2, rot_add(rotation, Rotation::COUNTERCLOCKWISE_90)); }
                    if (!MansionGrid::isHouse(grid, x, y + 1)) { p2 = E(position, 6); p2 = S(p2, 7); add(pieces, "small_wall", p2, rot_add(rotation, Rotation::CLOCKWISE_90)); }
                    if (!MansionGrid::isHouse(grid, x + 1, y)) {
                        if (!MansionGrid::isHouse(grid, x, y - 1)) { p2 = E(position, 7); p2 = N(p2, 2); add(pieces, "small_wall_corner", p2, rotation); }
                        if (!MansionGrid::isHouse(grid, x, y + 1)) { p2 = E(position, 8); p2 = S(p2, 7); add(pieces, "small_wall_corner", p2, rot_add(rotation, Rotation::CLOCKWISE_90)); }
                    }
                    if (MansionGrid::isHouse(grid, x - 1, y)) continue;
                    if (!MansionGrid::isHouse(grid, x, y - 1)) { p2 = W(position, 2); p2 = N(p2, 1); add(pieces, "small_wall_corner", p2, rot_add(rotation, Rotation::COUNTERCLOCKWISE_90)); }
                    if (MansionGrid::isHouse(grid, x, y + 1)) continue;
                    p2 = W(position, 1); p2 = S(p2, 8);
                    add(pieces, "small_wall_corner", p2, rot_add(rotation, Rotation::CLOCKWISE_180));
                }
        }
        for (int y = 0; y < grid.height; ++y)
            for (int x = 0; x < grid.width; ++x) {
                BlockPos position = S(roofOrigin, 8 + (y - startY) * 8);
                position = E(position, (x - startX) * 8);
                bool isAbove = aboveGrid && MansionGrid::isHouse(*aboveGrid, x, y);
                if (!MansionGrid::isHouse(grid, x, y) || isAbove) continue;
                BlockPos p2, p3;
                if (!MansionGrid::isHouse(grid, x + 1, y)) {
                    p2 = E(position, 6);
                    if (!MansionGrid::isHouse(grid, x, y + 1)) { p3 = S(p2, 6); add(pieces, "roof_corner", p3, rotation); }
                    else if (MansionGrid::isHouse(grid, x + 1, y + 1)) { p3 = S(p2, 5); add(pieces, "roof_inner_corner", p3, rotation); }
                    if (!MansionGrid::isHouse(grid, x, y - 1)) add(pieces, "roof_corner", p2, rot_add(rotation, Rotation::COUNTERCLOCKWISE_90));
                    else if (MansionGrid::isHouse(grid, x + 1, y - 1)) { p3 = E(position, 9); p3 = N(p3, 2); add(pieces, "roof_inner_corner", p3, rot_add(rotation, Rotation::CLOCKWISE_90)); }
                }
                if (MansionGrid::isHouse(grid, x - 1, y)) continue;
                p2 = E(position, 0);
                p2 = S(p2, 0);
                if (!MansionGrid::isHouse(grid, x, y + 1)) { p3 = S(p2, 6); add(pieces, "roof_corner", p3, rot_add(rotation, Rotation::CLOCKWISE_90)); }
                else if (MansionGrid::isHouse(grid, x - 1, y + 1)) { p3 = S(p2, 8); p3 = W(p3, 3); add(pieces, "roof_inner_corner", p3, rot_add(rotation, Rotation::COUNTERCLOCKWISE_90)); }
                if (!MansionGrid::isHouse(grid, x, y - 1)) { add(pieces, "roof_corner", p2, rot_add(rotation, Rotation::CLOCKWISE_180)); continue; }
                if (!MansionGrid::isHouse(grid, x - 1, y - 1)) continue;
                p3 = S(p2, 1);
                add(pieces, "roof_inner_corner", p3, rot_add(rotation, Rotation::CLOCKWISE_180));
            }
    }

    void entrance(Pieces& pieces, PlacementData& data) {
        Direction west = rot_dir(data.rotation, Direction::WEST);
        add(pieces, "entrance", rel(data.position, west, 9), data.rotation);
        data.position = rel(data.position, rot_dir(data.rotation, Direction::SOUTH), 16);
    }
    void traverseWallPiece(Pieces& pieces, PlacementData& data) {
        add(pieces, data.wallType, rel(data.position, rot_dir(data.rotation, Direction::EAST), 7), data.rotation);
        data.position = rel(data.position, rot_dir(data.rotation, Direction::SOUTH), 8);
    }
    void traverseTurn(Pieces& pieces, PlacementData& data) {
        data.position = rel(data.position, rot_dir(data.rotation, Direction::SOUTH), -1);
        add(pieces, "wall_corner", data.position, data.rotation);
        data.position = rel(data.position, rot_dir(data.rotation, Direction::SOUTH), -7);
        data.position = rel(data.position, rot_dir(data.rotation, Direction::WEST), -6);
        data.rotation = rot_add(data.rotation, Rotation::CLOCKWISE_90);
    }
    void traverseInnerTurn(Pieces&, PlacementData& data) {
        data.position = rel(data.position, rot_dir(data.rotation, Direction::SOUTH), 6);
        data.position = rel(data.position, rot_dir(data.rotation, Direction::EAST), 8);
        data.rotation = rot_add(data.rotation, Rotation::COUNTERCLOCKWISE_90);
    }

    void addRoom1x1(Pieces& pieces, const BlockPos& roomPos, Rotation rotation, bool hasDoorDir, const Direction& doorDir, const FloorRooms& rooms) {
        Rotation pieceRot = Rotation::NONE;
        std::string roomType = rooms.get1x1(random);
        if (!(hasDoorDir && doorDir == Direction::EAST)) {
            if (hasDoorDir && doorDir == Direction::NORTH) pieceRot = rot_add(pieceRot, Rotation::COUNTERCLOCKWISE_90);
            else if (hasDoorDir && doorDir == Direction::WEST) pieceRot = rot_add(pieceRot, Rotation::CLOCKWISE_180);
            else if (hasDoorDir && doorDir == Direction::SOUTH) pieceRot = rot_add(pieceRot, Rotation::CLOCKWISE_90);
            else roomType = rooms.get1x1Secret(random);
        }
        BlockPos orientation = zero_position_with_transform(BlockPos(1, 0, 0), Mirror::NONE, pieceRot, 7, 7);
        pieceRot = rot_add(pieceRot, rotation);
        orientation = rotate_pos(orientation, rotation);
        BlockPos pos = roomPos.offset(orientation.x, 0, orientation.z);
        add(pieces, roomType, pos, pieceRot);
    }

    void addRoom1x2(Pieces& pieces, const BlockPos& roomPos, Rotation rotation, const Direction& roomDir, const Direction& doorDir, const FloorRooms& rooms, bool isStairsRoom) {
        auto E = [&](const BlockPos& p, int n) { return rel(p, rot_dir(rotation, Direction::EAST), n); };
        auto S = [&](const BlockPos& p, int n) { return rel(p, rot_dir(rotation, Direction::SOUTH), n); };
        auto side = [&]() { return rooms.get1x2SideEntrance(random, isStairsRoom); };
        auto front = [&]() { return rooms.get1x2FrontEntrance(random, isStairsRoom); };
        const Direction E_ = Direction::EAST, W_ = Direction::WEST, N_ = Direction::NORTH, S_ = Direction::SOUTH, U_ = Direction::UP;
        if (doorDir == E_ && roomDir == S_) { BlockPos pos = E(roomPos, 1); add(pieces, side(), pos, rotation); }
        else if (doorDir == E_ && roomDir == N_) { BlockPos pos = E(roomPos, 1); pos = S(pos, 6); add(pieces, side(), pos, rotation, Mirror::LEFT_RIGHT); }
        else if (doorDir == W_ && roomDir == N_) { BlockPos pos = E(roomPos, 7); pos = S(pos, 6); add(pieces, side(), pos, rot_add(rotation, Rotation::CLOCKWISE_180)); }
        else if (doorDir == W_ && roomDir == S_) { BlockPos pos = E(roomPos, 7); add(pieces, side(), pos, rotation, Mirror::FRONT_BACK); }
        else if (doorDir == S_ && roomDir == E_) { BlockPos pos = E(roomPos, 1); add(pieces, side(), pos, rot_add(rotation, Rotation::CLOCKWISE_90), Mirror::LEFT_RIGHT); }
        else if (doorDir == S_ && roomDir == W_) { BlockPos pos = E(roomPos, 7); add(pieces, side(), pos, rot_add(rotation, Rotation::CLOCKWISE_90)); }
        else if (doorDir == N_ && roomDir == W_) { BlockPos pos = E(roomPos, 7); pos = S(pos, 6); add(pieces, side(), pos, rot_add(rotation, Rotation::CLOCKWISE_90), Mirror::FRONT_BACK); }
        else if (doorDir == N_ && roomDir == E_) { BlockPos pos = E(roomPos, 1); pos = S(pos, 6); add(pieces, side(), pos, rot_add(rotation, Rotation::COUNTERCLOCKWISE_90)); }
        else if (doorDir == S_ && roomDir == N_) { BlockPos pos = E(roomPos, 1); pos = rel(pos, rot_dir(rotation, Direction::NORTH), 8); add(pieces, front(), pos, rotation); }
        else if (doorDir == N_ && roomDir == S_) { BlockPos pos = E(roomPos, 7); pos = S(pos, 14); add(pieces, front(), pos, rot_add(rotation, Rotation::CLOCKWISE_180)); }
        else if (doorDir == W_ && roomDir == E_) { BlockPos pos = E(roomPos, 15); add(pieces, front(), pos, rot_add(rotation, Rotation::CLOCKWISE_90)); }
        else if (doorDir == E_ && roomDir == W_) { BlockPos pos = rel(roomPos, rot_dir(rotation, Direction::WEST), 7); pos = S(pos, 6); add(pieces, front(), pos, rot_add(rotation, Rotation::COUNTERCLOCKWISE_90)); }
        else if (doorDir == U_ && roomDir == E_) { BlockPos pos = E(roomPos, 15); add(pieces, rooms.get1x2Secret(random), pos, rot_add(rotation, Rotation::CLOCKWISE_90)); }
        else if (doorDir == U_ && roomDir == S_) { BlockPos pos = E(roomPos, 1); pos = rel(pos, rot_dir(rotation, Direction::NORTH), 0); add(pieces, rooms.get1x2Secret(random), pos, rotation); }
    }

    void addRoom2x2(Pieces& pieces, const BlockPos& roomPos, Rotation rotation, const Direction& roomDir, const Direction& doorDir, const FloorRooms& rooms) {
        int east = 0, south = 0;
        Rotation rot = rotation;
        Mirror mirror = Mirror::NONE;
        const Direction E_ = Direction::EAST, W_ = Direction::WEST, N_ = Direction::NORTH, S_ = Direction::SOUTH;
        if (doorDir == E_ && roomDir == S_) { east = -7; }
        else if (doorDir == E_ && roomDir == N_) { east = -7; south = 6; mirror = Mirror::LEFT_RIGHT; }
        else if (doorDir == N_ && roomDir == E_) { east = 1; south = 14; rot = rot_add(rotation, Rotation::COUNTERCLOCKWISE_90); }
        else if (doorDir == N_ && roomDir == W_) { east = 7; south = 14; rot = rot_add(rotation, Rotation::COUNTERCLOCKWISE_90); mirror = Mirror::LEFT_RIGHT; }
        else if (doorDir == S_ && roomDir == W_) { east = 7; south = -8; rot = rot_add(rotation, Rotation::CLOCKWISE_90); }
        else if (doorDir == S_ && roomDir == E_) { east = 1; south = -8; rot = rot_add(rotation, Rotation::CLOCKWISE_90); mirror = Mirror::LEFT_RIGHT; }
        else if (doorDir == W_ && roomDir == N_) { east = 15; south = 6; rot = rot_add(rotation, Rotation::CLOCKWISE_180); }
        else if (doorDir == W_ && roomDir == S_) { east = 15; mirror = Mirror::FRONT_BACK; }
        BlockPos pos = rel(roomPos, rot_dir(rotation, Direction::EAST), east);
        pos = rel(pos, rot_dir(rotation, Direction::SOUTH), south);
        add(pieces, rooms.get2x2(random), pos, rot, mirror);
    }

    void addRoom2x2Secret(Pieces& pieces, const BlockPos& roomPos, Rotation rotation, const FloorRooms& rooms) {
        BlockPos pos = rel(roomPos, rot_dir(rotation, Direction::EAST), 1);
        add(pieces, rooms.get2x2Secret(random), pos, rotation, Mirror::NONE);
    }
};

class WoodlandMansionStructure : public Structure {
public:
    std::optional<GenerationStub> find_generation_point(const StructureContext& ctx) const override {
        static const Rotation kRots[4] = {Rotation::NONE, Rotation::CLOCKWISE_90, Rotation::CLOCKWISE_180, Rotation::COUNTERCLOCKWISE_90};
        Rotation rotation = kRots[ctx.random.next_int(4)];
        BlockPos startPos = structure_lowest_y_in_5by5_box_offset_7(ctx, rotation);
        if (startPos.y < 60) return std::nullopt;
        GenerationStub stub;
        stub.position = startPos;
        stub.generate = [&ctx, startPos, rotation](std::vector<std::shared_ptr<StructurePiece>>& out) {
            JRandom random(ctx.random);
            MansionGrid grid(random);
            MansionPiecePlacer placer(*ctx.env.templates, random);
            placer.createMansion(startPos, rotation, out, grid);
        };
        return stub;
    }
    void after_place(PieceCtx& ctx, const StructureStart& start) const override {
        int minY = ctx.level.min_y();
        BoundingBox boundingBox = start.piece_bounds();
        int yStart = boundingBox.y0;
        const BoundingBox& chunkBB = ctx.chunk_bb;
        // every column that starts with a block of a piece is filled down to the ground with cobblestone
        for (int x = chunkBB.x0; x <= chunkBB.x1; ++x)
            for (int z = chunkBB.z0; z <= chunkBB.z1; ++z) {
                BlockPos pos(x, yStart, z);
                if (jc::bd().air(ctx.level.get(pos)) || !boundingBox.inside(pos)) continue;
                bool inside = false;
                for (auto& p : start.pieces) if (p->bb.inside(pos)) { inside = true; break; }
                if (!inside) continue;
                for (int y = yStart - 1; y > minY; --y) {
                    uint16_t st = ctx.level.get(x, y, z);
                    if (!jc::bd().air(st) && !(jc::bd().flags(st) & BF_LIQUID)) break;
                    ctx.level.set(x, y, z, jc::bd().default_state("minecraft:cobblestone"));
                }
            }
    }
};

struct RegisterMansion {
    RegisterMansion() {
        structure_factories()["minecraft:woodland_mansion"] = [](const Json&, const Registry&) -> std::unique_ptr<Structure> { return std::make_unique<WoodlandMansionStructure>(); };
    }
} register_mansion;

}  // namespace

}  // namespace pn::vg
