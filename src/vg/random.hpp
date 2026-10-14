// Mojang's random number generators (java.util.Random's LCG and Xoroshiro128++), bit for bit, with the positional factories world generation seeds noise from.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace pn::vg {

class Rng;

struct PosFactory {
    virtual ~PosFactory() = default;
    virtual std::unique_ptr<Rng> at(int x, int y, int z) const = 0;
    virtual std::unique_ptr<Rng> from_hash_of(const std::string& name) const = 0;
    virtual std::unique_ptr<Rng> from_seed(int64_t seed) const = 0;
};

class Rng {
public:
    virtual ~Rng() = default;
    virtual std::unique_ptr<Rng> fork() = 0;
    virtual std::unique_ptr<PosFactory> fork_positional() = 0;
    virtual void set_seed(int64_t seed) = 0;
    virtual int32_t next_int() = 0;
    virtual int32_t next_int(int32_t bound) = 0;
    virtual int64_t next_long() = 0;
    virtual bool next_boolean() = 0;
    virtual float next_float() = 0;
    virtual double next_double() = 0;
    virtual double next_gaussian();
    int32_t next_int(int32_t origin, int32_t bound) { return origin + next_int(bound - origin); }
    int32_t next_int_between_inclusive(int32_t lo, int32_t hi) { return next_int(hi - lo + 1) + lo; }
    double triangle(double mean, double spread) { double a = next_double(); double b = next_double(); return mean + spread * (a - b); }
    float triangle(float mean, float spread) { float a = next_float(); float b = next_float(); return mean + spread * (a - b); }
    virtual void consume_count(int n) { for (int i = 0; i < n; i++) next_int(); }
private:
    double next_next_gaussian_ = 0;
    bool have_next_gaussian_ = false;
protected:
    void reset_gaussian() { have_next_gaussian_ = false; }
};

// java.util.Random as Mojang's LegacyRandomSource (48-bit LCG)
class LegacyRandom : public Rng {
public:
    explicit LegacyRandom(int64_t seed) { set_seed(seed); }
    std::unique_ptr<Rng> fork() override;
    std::unique_ptr<PosFactory> fork_positional() override;
    void set_seed(int64_t seed) override;
    int32_t next_int() override { return next(32); }
    int32_t next_int(int32_t bound) override;
    int64_t next_long() override;
    bool next_boolean() override { return next(1) != 0; }
    float next_float() override;
    double next_double() override;
    virtual int32_t next(int bits);
    int64_t state() const { return seed_; }
    void set_state(int64_t s) { seed_ = s; }
private:
    int64_t seed_ = 0;
};

class Xoroshiro : public Rng {
public:
    explicit Xoroshiro(int64_t seed);
    Xoroshiro(int64_t lo, int64_t hi);
    std::unique_ptr<Rng> fork() override;
    std::unique_ptr<PosFactory> fork_positional() override;
    void set_seed(int64_t seed) override;
    int32_t next_int() override { return int32_t(next_raw()); }
    int32_t next_int(int32_t bound) override;
    int64_t next_long() override { return next_raw(); }
    bool next_boolean() override { return (next_raw() & 1) != 0; }
    float next_float() override;
    double next_double() override;
    void consume_count(int n) override { for (int i = 0; i < n; i++) next_raw(); }
private:
    int64_t next_raw();
    int64_t next_bits(int bits) { return int64_t(uint64_t(next_raw()) >> (64 - bits)); }
    int64_t lo_, hi_;
};

// Mojang's WorldgenRandom over a Xoroshiro source: it is a LegacyRandomSource (java.util.Random's algorithms) whose next(bits) takes the top bits of the
// Xoroshiro source's next long. Biome decoration uses it, so its draws differ from those of a plain Xoroshiro.
class WorldgenRandomXoro : public LegacyRandom {
public:
    explicit WorldgenRandomXoro(int64_t seed) : LegacyRandom(0), x_(seed) {}
    void set_seed(int64_t seed) override { x_.set_seed(seed); }
    int32_t next(int bits) override { return int32_t(uint64_t(x_.next_long()) >> (64 - bits)); }
    std::unique_ptr<Rng> fork() override { return x_.fork(); }
    std::unique_ptr<PosFactory> fork_positional() override { return x_.fork_positional(); }
private:
    Xoroshiro x_;
};

// Mojang's Mth.getSeed and the 128 bit seed helpers
int64_t pos_seed(int x, int y, int z);
int64_t mix_stafford13(int64_t z);
void upgrade_seed_to_128(int64_t seed, int64_t& lo, int64_t& hi);   // mixed
void seed_from_hash_of(const std::string& s, int64_t& lo, int64_t& hi);
int32_t java_string_hash(const std::string& s);

}  // namespace pn::vg
