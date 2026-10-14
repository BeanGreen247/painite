#include "random.hpp"
#include <cmath>
#include <openssl/evp.h>

namespace pn::vg {

// ---- helpers ---------------------------------------------------------------------------------------------

static inline int64_t rotl(int64_t v, int n) { return int64_t((uint64_t(v) << n) | (uint64_t(v) >> (64 - n))); }

int64_t pos_seed(int x, int y, int z) {
    // Java: x * 3129871 ^ z * 116129781L ^ y  (x * 3129871 is an int multiplication that wraps)
    int32_t xm = int32_t(uint32_t(x) * 3129871u);
    int64_t seed = int64_t(xm) ^ (int64_t(uint64_t(int64_t(z)) * 116129781ull)) ^ int64_t(y);
    seed = int64_t(uint64_t(seed) * uint64_t(seed) * 42317861ull + uint64_t(seed) * 11ull);
    return seed >> 16;
}

int64_t mix_stafford13(int64_t z) {
    uint64_t u = uint64_t(z);
    u = (u ^ (u >> 30)) * 0xbf58476d1ce4e5b9ull;
    u = (u ^ (u >> 27)) * 0x94d049bb133111ebull;
    return int64_t(u ^ (u >> 31));
}

void upgrade_seed_to_128(int64_t seed, int64_t& lo, int64_t& hi) {
    int64_t l = seed ^ int64_t(7640891576956012809ull);
    int64_t h = int64_t(uint64_t(l) + uint64_t(-7046029254386353131ll));
    lo = mix_stafford13(l);
    hi = mix_stafford13(h);
}

void seed_from_hash_of(const std::string& s, int64_t& lo, int64_t& hi) {
    uint8_t d[16];
    unsigned n = 0;
    EVP_Digest(s.data(), s.size(), d, &n, EVP_md5(), nullptr);
    uint64_t a = 0, b = 0;
    for (int i = 0; i < 8; i++) { a = (a << 8) | d[i]; b = (b << 8) | d[8 + i]; }
    lo = int64_t(a);
    hi = int64_t(b);
}

int32_t java_string_hash(const std::string& s) {
    // String.hashCode over UTF-16 units; the names used here are ASCII
    uint32_t h = 0;
    for (unsigned char c : s) h = 31u * h + c;
    return int32_t(h);
}

double Rng::next_gaussian() {
    if (have_next_gaussian_) { have_next_gaussian_ = false; return next_next_gaussian_; }
    double x, y, r;
    do {
        x = 2.0 * next_double() - 1.0;
        y = 2.0 * next_double() - 1.0;
        r = x * x + y * y;
    } while (r >= 1.0 || r == 0.0);
    double m = std::sqrt(-2.0 * std::log(r) / r);
    next_next_gaussian_ = y * m;
    have_next_gaussian_ = true;
    return x * m;
}

// ---- LegacyRandom --------------------------------------------------------------------------------------------

namespace {
struct LegacyFactory : PosFactory {
    int64_t seed;
    explicit LegacyFactory(int64_t s) : seed(s) {}
    std::unique_ptr<Rng> at(int x, int y, int z) const override { return std::make_unique<LegacyRandom>(pos_seed(x, y, z) ^ seed); }
    std::unique_ptr<Rng> from_hash_of(const std::string& name) const override { return std::make_unique<LegacyRandom>(int64_t(java_string_hash(name)) ^ seed); }
    std::unique_ptr<Rng> from_seed(int64_t s) const override { return std::make_unique<LegacyRandom>(s); }
};
}  // namespace

void LegacyRandom::set_seed(int64_t seed) {
    seed_ = (seed ^ 25214903917ll) & 281474976710655ll;
    reset_gaussian();
}

int32_t LegacyRandom::next(int bits) {
    seed_ = int64_t((uint64_t(seed_) * 25214903917ull + 11ull) & 281474976710655ull);
    return int32_t(seed_ >> (48 - bits));
}

int32_t LegacyRandom::next_int(int32_t bound) {
    if ((bound & (bound - 1)) == 0) return int32_t((int64_t(bound) * next(31)) >> 31);
    int32_t sample, modulo;
    do {
        sample = next(31);
        modulo = sample % bound;
    } while (int32_t(uint32_t(sample) - uint32_t(modulo) + uint32_t(bound - 1)) < 0);
    return modulo;
}

int64_t LegacyRandom::next_long() {
    int32_t upper = next(32), lower = next(32);
    return int64_t((uint64_t(int64_t(upper)) << 32) + uint64_t(int64_t(lower)));
}

float LegacyRandom::next_float() { return float(next(24)) * 5.9604645E-8f; }

double LegacyRandom::next_double() {
    int32_t upper = next(26), lower = next(27);
    int64_t combined = (int64_t(upper) << 27) + lower;
    return double(combined) * double(1.110223E-16f);
}

std::unique_ptr<Rng> LegacyRandom::fork() { return std::make_unique<LegacyRandom>(next_long()); }
std::unique_ptr<PosFactory> LegacyRandom::fork_positional() { return std::make_unique<LegacyFactory>(next_long()); }

// ---- Xoroshiro128++ --------------------------------------------------------------------------------------------

namespace {
struct XoroFactory : PosFactory {
    int64_t lo, hi;
    XoroFactory(int64_t l, int64_t h) : lo(l), hi(h) {}
    std::unique_ptr<Rng> at(int x, int y, int z) const override { return std::make_unique<Xoroshiro>(pos_seed(x, y, z) ^ lo, hi); }
    std::unique_ptr<Rng> from_hash_of(const std::string& name) const override {
        int64_t a, b;
        seed_from_hash_of(name, a, b);
        return std::make_unique<Xoroshiro>(a ^ lo, b ^ hi);
    }
    std::unique_ptr<Rng> from_seed(int64_t s) const override { return std::make_unique<Xoroshiro>(s ^ lo, s ^ hi); }
};
}  // namespace

Xoroshiro::Xoroshiro(int64_t seed) { upgrade_seed_to_128(seed, lo_, hi_); if ((lo_ | hi_) == 0) { lo_ = -7046029254386353131ll; hi_ = 7640891576956012809ll; } }
Xoroshiro::Xoroshiro(int64_t lo, int64_t hi) : lo_(lo), hi_(hi) { if ((lo_ | hi_) == 0) { lo_ = -7046029254386353131ll; hi_ = 7640891576956012809ll; } }

void Xoroshiro::set_seed(int64_t seed) { upgrade_seed_to_128(seed, lo_, hi_); if ((lo_ | hi_) == 0) { lo_ = -7046029254386353131ll; hi_ = 7640891576956012809ll; } reset_gaussian(); }

int64_t Xoroshiro::next_raw() {
    int64_t s0 = lo_, s1 = hi_;
    int64_t result = int64_t(uint64_t(rotl(int64_t(uint64_t(s0) + uint64_t(s1)), 17)) + uint64_t(s0));
    s1 ^= s0;
    lo_ = rotl(s0, 49) ^ s1 ^ int64_t(uint64_t(s1) << 21);
    hi_ = rotl(s1, 28);
    return result;
}

int32_t Xoroshiro::next_int(int32_t bound) {
    uint64_t random_bits = uint32_t(next_int());
    uint64_t multiplied = random_bits * uint64_t(uint32_t(bound));
    uint64_t frac = multiplied & 4294967295ull;
    if (frac < uint64_t(bound)) {
        uint32_t unbiased_start = (~uint32_t(bound) + 1u) % uint32_t(bound);
        while (frac < unbiased_start) {
            random_bits = uint32_t(next_int());
            multiplied = random_bits * uint64_t(uint32_t(bound));
            frac = multiplied & 4294967295ull;
        }
    }
    return int32_t(multiplied >> 32);
}

float Xoroshiro::next_float() { return float(next_bits(24)) * 5.9604645E-8f; }
double Xoroshiro::next_double() { return double(next_bits(53)) * double(1.110223E-16f); }

std::unique_ptr<Rng> Xoroshiro::fork() { int64_t a = next_raw(), b = next_raw(); return std::make_unique<Xoroshiro>(a, b); }
std::unique_ptr<PosFactory> Xoroshiro::fork_positional() { int64_t a = next_raw(), b = next_raw(); return std::make_unique<XoroFactory>(a, b); }

}  // namespace pn::vg
