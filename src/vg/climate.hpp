// Mojang's multi-noise biome search: climate parameter points kept in an R-tree, queried with quantised climate samples.
#pragma once
#include <map>
#include <mutex>
#include <thread>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pn::vg {

struct Parameter {
    int64_t min = 0, max = 0;
    int64_t distance(int64_t target) const { int64_t above = target - max, below = min - target; return above > 0 ? above : (below > 0 ? below : 0); }
    Parameter span(const Parameter* other) const { return other ? Parameter{std::min(min, other->min), std::max(max, other->max)} : *this; }
};

struct ParameterPoint {
    Parameter temperature, humidity, continentalness, erosion, depth, weirdness;
    int64_t offset = 0;
    std::array<Parameter, 7> space() const { return {temperature, humidity, continentalness, erosion, depth, weirdness, Parameter{offset, offset}}; }
};

struct TargetPoint {
    int64_t temperature, humidity, continentalness, erosion, depth, weirdness;
    std::array<int64_t, 7> params() const { return {temperature, humidity, continentalness, erosion, depth, weirdness, 0}; }
};

int64_t quantize_coord(float v);

class ParameterList {
public:
    ParameterList(const std::vector<ParameterPoint>& points, const std::vector<int>& values, int children_per_node = 6);   // value = index into the caller's biome table
    ~ParameterList();
    struct Node;
    int find_value(const TargetPoint& t) const;
    int find_value_brute_force(const TargetPoint& t) const;
    // Climate.RTree keeps the last result per thread and only replaces it on a strictly smaller distance, so exact ties go to what the thread found before (26.3 parity)
    void set_remember_last(bool on) { remember_last_ = on; }
    size_t size() const { return points_.size(); }
    const ParameterPoint& point(size_t i) const { return points_[i]; }
    int value(size_t i) const { return values_[i]; }
private:
    std::vector<ParameterPoint> points_;
    std::vector<int> values_;
    std::vector<std::unique_ptr<Node>> pool_;
    Node* root_ = nullptr;
    bool remember_last_ = false;
    mutable std::mutex last_mu_;
    mutable std::map<std::thread::id, const Node*> last_;
    Node* build(std::vector<Node*>& children);
    Node* make_subtree(std::vector<Node*> children);
    friend struct Builder;
};

// reads the dump written by tools/oracle/DumpClimate.java (one preset); returns the biome names in value order
struct ClimateData {
    std::vector<std::string> biomes;
    std::unique_ptr<ParameterList> list;
};
ClimateData load_climate_dump(const std::string& path, const std::string& preset, int children_per_node = 6);

}  // namespace pn::vg
