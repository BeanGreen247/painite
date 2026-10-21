#include "entity_manager.hpp"
#include "../blocks.hpp"
#include <fstream>
#include <sstream>

namespace pn::sim {

const std::vector<AABB> BlockCollisionShapes::empty_;

BlockCollisionShapes& BlockCollisionShapes::get() {
    static BlockCollisionShapes shapes;
    return shapes;
}

bool BlockCollisionShapes::load(const std::string& states_path, const std::string& shapes_path, std::string& err) {
    std::ifstream states(states_path), shapes(shapes_path);
    if (!states || !shapes) {
        err = "could not open collision shape data";
        return false;
    }

    std::unordered_map<int, std::vector<AABB>> loaded_shapes;
    std::string line;
    while (std::getline(shapes, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        int id = -1;
        try { id = std::stoi(line.substr(0, tab)); } catch (const std::exception&) { continue; }
        std::vector<AABB> boxes;
        std::stringstream parts(line.substr(tab + 1));
        std::string part;
        while (std::getline(parts, part, ';')) {
            std::istringstream values(part);
            double x0, y0, z0, x1, y1, z1;
            if (values >> x0 >> y0 >> z0 >> x1 >> y1 >> z1)
                boxes.emplace_back(x0, y0, z0, x1, y1, z1);
        }
        loaded_shapes.emplace(id, std::move(boxes));
    }

    std::unordered_map<uint16_t, int> loaded_states;
    while (std::getline(states, line)) {
        size_t first = line.find('\t');
        size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) continue;
        int shape = -1;
        try { shape = std::stoi(line.substr(second + 1)); } catch (const std::exception&) { continue; }
        const std::string name = line.substr(0, first);
        const std::string props = line.substr(first + 1, second - first - 1);
        const uint16_t state = Blocks::get().find(name, props);
        if (state != Blocks::get().air() || name == "minecraft:air") loaded_states[state] = shape;
    }
    if (loaded_shapes.empty() || loaded_states.empty()) {
        err = "collision shape data contained no usable shapes or states";
        return false;
    }
    shapes_ = std::move(loaded_shapes);
    state_shapes_ = std::move(loaded_states);
    size_t top = 0;
    for (auto& kv : state_shapes_) top = std::max<size_t>(top, kv.first);
    flat_.assign(top + 1, nullptr);
    flat_full_.assign(top + 1, 0);
    for (auto& kv : state_shapes_) {
        auto it = shapes_.find(kv.second);
        if (it == shapes_.end() || it->second.empty()) continue;
        flat_[kv.first] = &it->second;
        const auto& sh = it->second;
        flat_full_[kv.first] = sh.size() == 1 && sh[0].min_x == 0.0 && sh[0].min_y == 0.0 && sh[0].min_z == 0.0 && sh[0].max_x == 1.0 && sh[0].max_y == 1.0 && sh[0].max_z == 1.0;
    }
    return true;
}

}  // namespace pn::sim
