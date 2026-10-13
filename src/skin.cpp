#include "skin.hpp"
#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace pn {

// ---- PNG ------------------------------------------------------------------------------------------------

bool decode_png(const uint8_t* p, size_t n, int& w, int& h, std::vector<uint8_t>& rgba) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (n < 33 || memcmp(p, sig, 8) != 0) return false;
    size_t i = 8;
    int depth = 0, ctype = 0, interlace = 0;
    std::vector<uint8_t> idat, plte, trns;
    bool have_hdr = false;
    while (i + 12 <= n) {
        uint32_t len = static_cast<uint32_t>(p[i]) << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3];
        const uint8_t* type = p + i + 4;
        const uint8_t* d = p + i + 8;
        if (len > n || i + 12 + len > n) return false;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = static_cast<int>(static_cast<uint32_t>(d[0]) << 24 | d[1] << 16 | d[2] << 8 | d[3]);
            h = static_cast<int>(static_cast<uint32_t>(d[4]) << 24 | d[5] << 16 | d[6] << 8 | d[7]);
            depth = d[8]; ctype = d[9]; interlace = d[12];
            have_hdr = true;
        } else if (!memcmp(type, "PLTE", 4)) plte.assign(d, d + len);
        else if (!memcmp(type, "tRNS", 4)) trns.assign(d, d + len);
        else if (!memcmp(type, "IDAT", 4)) idat.insert(idat.end(), d, d + len);
        else if (!memcmp(type, "IEND", 4)) break;
        i += 12 + len;
    }
    if (!have_hdr || interlace != 0 || depth != 8 || w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!ch) return false;
    size_t stride = static_cast<size_t>(w) * static_cast<size_t>(ch);
    std::vector<uint8_t> raw((stride + 1) * static_cast<size_t>(h));
    uLongf outl = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &outl, idat.data(), static_cast<uLong>(idat.size())) != Z_OK || outl != raw.size()) return false;
    std::vector<uint8_t> img(stride * static_cast<size_t>(h));
    for (int y = 0; y < h; y++) {
        uint8_t f = raw[static_cast<size_t>(y) * (stride + 1)];
        const uint8_t* src = &raw[static_cast<size_t>(y) * (stride + 1) + 1];
        uint8_t* dst = &img[static_cast<size_t>(y) * stride];
        const uint8_t* up = y ? dst - stride : nullptr;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= static_cast<size_t>(ch) ? dst[x - static_cast<size_t>(ch)] : 0;
            int b = up ? up[x] : 0;
            int c = (up && x >= static_cast<size_t>(ch)) ? up[x - static_cast<size_t>(ch)] : 0;
            int v = src[x];
            switch (f) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: {
                    int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                    v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: return false;
            }
            dst[x] = static_cast<uint8_t>(v);
        }
    }
    rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (size_t px = 0; px < static_cast<size_t>(w) * static_cast<size_t>(h); px++) {
        const uint8_t* s = &img[px * static_cast<size_t>(ch)];
        uint8_t* o = &rgba[px * 4];
        switch (ctype) {
            case 0: o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
            case 2: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
            case 3: {
                size_t k = s[0];
                if (k * 3 + 2 >= plte.size()) return false;
                o[0] = plte[k * 3]; o[1] = plte[k * 3 + 1]; o[2] = plte[k * 3 + 2];
                o[3] = k < trns.size() ? trns[k] : 255;
                break;
            }
            case 4: o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
            default: memcpy(o, s, 4);
        }
    }
    return true;
}

static void blit_mirrored(std::vector<uint8_t>& im, int sx, int sy, int dx, int dy, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) memcpy(&im[(static_cast<size_t>(dy + y) * 64 + static_cast<size_t>(dx + x)) * 4], &im[(static_cast<size_t>(sy + y) * 64 + static_cast<size_t>(sx + w - 1 - x)) * 4], 4);
}

void upgrade_legacy_skin(int& w, int& h, std::vector<uint8_t>& rgba) {
    if (w != 64 || h != 32) return;
    rgba.resize(64 * 64 * 4, 0);
    // left leg (16,48) from right leg (0,16); left arm (32,48) from right arm (40,16); faces are mirrored, side faces swap
    auto limb = [&](int sx, int sy, int dx, int dy) {
        blit_mirrored(rgba, sx + 4, sy, dx, dy, 4, 4);         // top
        blit_mirrored(rgba, sx + 8, sy, dx + 4, dy, 4, 4);     // bottom
        blit_mirrored(rgba, sx, sy + 4, dx + 8, dy + 4, 4, 12);      // right side -> left side
        blit_mirrored(rgba, sx + 4, sy + 4, dx + 4, dy + 4, 4, 12);  // front
        blit_mirrored(rgba, sx + 8, sy + 4, dx, dy + 4, 4, 12);      // left side -> right side
        blit_mirrored(rgba, sx + 12, sy + 4, dx + 12, dy + 4, 4, 12);  // back
    };
    limb(0, 16, 16, 48);
    limb(40, 16, 32, 48);
    h = 64;
}

// ---- Mojang textures property ---------------------------------------------------------------------------

static std::string b64_decode(const std::string& s) {
    static int8_t t[256];
    static bool init = false;
    if (!init) {
        memset(t, -1, sizeof t);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) t[static_cast<uint8_t>(a[i])] = static_cast<int8_t>(i);
        t[static_cast<uint8_t>('-')] = 62;
        t[static_cast<uint8_t>('_')] = 63;
        init = true;
    }
    std::string o;
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=') break;
        int8_t v = t[c];
        if (v < 0) return {};
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back(static_cast<char>((acc >> bits) & 0xff)); }
    }
    return o;
}

bool parse_textures_property(const std::string& value_b64, std::string& host, std::string& path, bool& slim) {
    std::string js = b64_decode(value_b64);
    JP jp{js.data(), js.data() + js.size()};
    JV v = jp.val();
    if (jp.bad || v.t != JV::Obj) return false;
    const JV* tex = v.get("textures");
    const JV* skin = tex ? tex->get("SKIN") : nullptr;
    const JV* url = skin ? skin->get("url") : nullptr;
    if (!url || url->s.empty()) return false;
    std::string u = url->s;
    size_t sch = u.find("://");
    if (sch == std::string::npos) return false;
    u = u.substr(sch + 3);
    size_t sl = u.find('/');
    if (sl == std::string::npos) return false;
    host = u.substr(0, sl);
    path = u.substr(sl);
    // only Mojang's own texture hosts
    auto ends = [&](const char* suf) { size_t l = strlen(suf); return host.size() >= l && host.compare(host.size() - l, l, suf) == 0; };
    if (!(ends("minecraft.net") || ends("mojang.com"))) return false;
    slim = false;
    if (const JV* md = skin->get("metadata")) if (const JV* m = md->get("model")) slim = m->s == "slim";
    return true;
}

// ---- default skin ---------------------------------------------------------------------------------------

std::vector<uint8_t> default_skin_rgba(bool alex) {
    std::vector<uint8_t> im(64 * 64 * 4, 0);
    auto fill = [&](int x0, int y0, int w, int h, uint32_t rgb) {
        for (int y = y0; y < y0 + h; y++)
            for (int x = x0; x < x0 + w; x++) {
                uint8_t* o = &im[(static_cast<size_t>(y) * 64 + static_cast<size_t>(x)) * 4];
                o[0] = static_cast<uint8_t>(rgb >> 16); o[1] = static_cast<uint8_t>(rgb >> 8); o[2] = static_cast<uint8_t>(rgb); o[3] = 255;
            }
    };
    const uint32_t skin = 0xc08c6a, hair = 0x3b2a1a, shirt = alex ? 0x6b9a4a : 0x00a8a8, pants = 0x3c3c9c, shoe = 0x555555;
    fill(0, 0, 32, 16, skin);                    // head (all faces)
    fill(8, 0, 8, 8, hair);                      // hair on top
    fill(0, 8, 32, 3, hair);                     // hair line on every side
    fill(8, 11, 8, 1, hair);
    fill(10, 12, 1, 2, 0xffffff); fill(13, 12, 1, 2, 0xffffff);   // eyes on the front face
    fill(10, 13, 1, 1, 0x4040c0); fill(13, 13, 1, 1, 0x4040c0);
    fill(16, 16, 24, 16, shirt);                 // body
    fill(16, 16, 24, 16, shirt);
    fill(0, 16, 16, 16, pants);                  // right leg
    fill(0, 28, 16, 4, shoe);
    fill(16, 48, 16, 16, pants);                 // left leg
    fill(16, 60, 16, 4, shoe);
    fill(40, 16, 16, 16, shirt);                 // right arm
    fill(40, 28, 16, 4, skin);
    fill(32, 48, 16, 16, shirt);                 // left arm
    fill(32, 60, 16, 4, skin);
    return im;
}

// ---- Bedrock Skin structure -----------------------------------------------------------------------------

static void put_varuint(std::vector<uint8_t>& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
    o.push_back(static_cast<uint8_t>(v));
}
static void put_str(std::vector<uint8_t>& o, const std::string& s) { put_varuint(o, s.size()); o.insert(o.end(), s.begin(), s.end()); }
static void put_i32(std::vector<uint8_t>& o, int32_t v) { for (int i = 0; i < 4; i++) o.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i))); }
static void put_f32(std::vector<uint8_t>& o, float f) { uint8_t b[4]; memcpy(b, &f, 4); o.insert(o.end(), b, b + 4); }

std::vector<uint8_t> bedrock_classic_skin(const std::vector<uint8_t>& rgba, bool slim, const std::string& id) {
    std::vector<uint8_t> o;
    put_str(o, id);
    put_str(o, "");
    put_str(o, slim ? "{\"geometry\":{\"default\":\"geometry.humanoid.customSlim\"}}" : "{\"geometry\":{\"default\":\"geometry.humanoid.custom\"}}");
    put_i32(o, 64);
    put_i32(o, 64);
    put_varuint(o, rgba.size());
    o.insert(o.end(), rgba.begin(), rgba.end());
    put_varuint(o, 0);                       // animations
    put_i32(o, 0); put_i32(o, 0); put_varuint(o, 0);   // no cape
    put_str(o, "");                          // geometry data: the client's built in humanoid
    put_str(o, "0.0.0");
    put_str(o, "");
    put_str(o, "");                          // cape id
    put_str(o, id);                          // full skin id
    o.push_back(slim ? 0 : 1);               // arm size
    put_i32(o, static_cast<int32_t>(0xffb37b62));
    put_varuint(o, 0);                       // persona pieces
    put_varuint(o, 0);                       // tint colours
    o.push_back(0); o.push_back(0); o.push_back(0); o.push_back(1); o.push_back(0);   // premium, persona, cape on classic, primary user, overriding
    put_str(o, "");                          // trusted
    put_str(o, "");                          // profile hash
    return o;
}

struct R {
    const uint8_t* p;
    const uint8_t* e;
    bool bad = false;
    uint64_t vu() {
        uint64_t v = 0;
        for (int s = 0; s < 70; s += 7) {
            if (p >= e) { bad = true; return 0; }
            uint8_t c = *p++;
            v |= static_cast<uint64_t>(c & 0x7f) << s;
            if (!(c & 0x80)) return v;
        }
        bad = true;
        return 0;
    }
    void skip(size_t n) { if (static_cast<size_t>(e - p) < n) { bad = true; p = e; } else p += n; }
    void str() { uint64_t l = vu(); if (!bad) skip(l); }
    void image() { skip(8); uint64_t l = vu(); if (!bad) skip(l); }
};

bool bedrock_skin_extent(const uint8_t* p, const uint8_t* e, const uint8_t*& end_out) {
    R r{p, e};
    r.str(); r.str(); r.str();      // skin id, playfab id, resource patch
    r.image();                      // skin
    uint64_t na = r.vu();
    for (uint64_t i = 0; i < na && !r.bad; i++) { r.image(); r.vu(); r.skip(4); r.vu(); }
    r.image();                      // cape
    r.str(); r.str(); r.str(); r.str(); r.str();   // geometry, geometry version, animation data, cape id, full skin id
    r.skip(1 + 4);                  // arm size, skin colour
    uint64_t np = r.vu();
    for (uint64_t i = 0; i < np && !r.bad; i++) { r.str(); r.skip(4); r.skip(16); r.skip(1); r.str(); }
    uint64_t nt = r.vu();
    for (uint64_t i = 0; i < nt && !r.bad; i++) { r.str(); r.skip(16); }
    r.skip(5);
    r.str(); r.str();
    if (r.bad) return false;
    end_out = r.p;
    return true;
}

static std::string jstr(const JV& v, const char* k) { const JV* x = v.get(k); return x && x->t == JV::Str ? x->s : std::string(); }
static int64_t jint(const JV& v, const char* k) { const JV* x = v.get(k); return x && x->t == JV::Num ? strtoll(x->s.c_str(), nullptr, 10) : 0; }
static bool jbool(const JV& v, const char* k) { const JV* x = v.get(k); return x && x->t == JV::Bool && x->s == "true"; }

bool bedrock_skin_from_client(const JV& cd, std::vector<uint8_t>& out) {
    std::string id = jstr(cd, "SkinId");
    if (id.empty()) return false;
    std::string data = b64_decode(jstr(cd, "SkinData"));
    int w = static_cast<int>(jint(cd, "SkinImageWidth")), h = static_cast<int>(jint(cd, "SkinImageHeight"));
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || data.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) return false;
    std::vector<uint8_t> o;
    put_str(o, id);
    put_str(o, jstr(cd, "PlayFabId"));
    put_str(o, b64_decode(jstr(cd, "SkinResourcePatch")));
    put_i32(o, w); put_i32(o, h);
    put_varuint(o, data.size());
    o.insert(o.end(), data.begin(), data.end());
    const JV* an = cd.get("AnimatedImageData");
    size_t na = an && an->t == JV::Arr ? std::min<size_t>(an->a.size(), 16) : 0;
    put_varuint(o, na);
    for (size_t i = 0; i < na; i++) {
        const JV& a = an->a[i];
        std::string img = b64_decode(jstr(a, "Image"));
        int aw = static_cast<int>(jint(a, "ImageWidth")), ah = static_cast<int>(jint(a, "ImageHeight"));
        if (aw <= 0 || ah <= 0 || img.size() != static_cast<size_t>(aw) * static_cast<size_t>(ah) * 4) return false;
        put_i32(o, aw); put_i32(o, ah);
        put_varuint(o, img.size());
        o.insert(o.end(), img.begin(), img.end());
        put_varuint(o, static_cast<uint64_t>(jint(a, "Type")));
        const JV* fr = a.get("Frames");
        put_f32(o, fr ? static_cast<float>(strtod(fr->s.c_str(), nullptr)) : 0.0f);
        put_varuint(o, static_cast<uint64_t>(jint(a, "AnimationExpression")));
    }
    std::string cape = b64_decode(jstr(cd, "CapeData"));
    int cw = static_cast<int>(jint(cd, "CapeImageWidth")), ch = static_cast<int>(jint(cd, "CapeImageHeight"));
    if (cape.size() != static_cast<size_t>(std::max(cw, 0)) * static_cast<size_t>(std::max(ch, 0)) * 4) { cape.clear(); cw = ch = 0; }
    put_i32(o, cw); put_i32(o, ch);
    put_varuint(o, cape.size());
    o.insert(o.end(), cape.begin(), cape.end());
    put_str(o, b64_decode(jstr(cd, "SkinGeometryData")));
    put_str(o, b64_decode(jstr(cd, "SkinGeometryDataEngineVersion")));
    put_str(o, b64_decode(jstr(cd, "SkinAnimationData")));
    put_str(o, jstr(cd, "CapeId"));
    put_str(o, id);   // full skin id
    o.push_back(jstr(cd, "ArmSize") == "slim" ? 0 : 1);
    std::string col = jstr(cd, "SkinColor");
    uint32_t color = 0xffb37b62;
    if (col.size() == 7 && col[0] == '#') color = 0xff000000u | static_cast<uint32_t>(strtoul(col.c_str() + 1, nullptr, 16));
    put_i32(o, static_cast<int32_t>(color));
    const JV* pp = cd.get("PersonaPieces");
    size_t np = pp && pp->t == JV::Arr ? std::min<size_t>(pp->a.size(), 64) : 0;
    put_varuint(o, np);
    auto put_uuid = [&](const std::string& s) {
        uint8_t u[16] = {};
        std::string hex;
        for (char c : s) if (c != '-') hex += c;
        for (size_t i = 0; i < 16 && i * 2 + 1 < hex.size(); i++) u[i] = static_cast<uint8_t>(strtoul(hex.substr(i * 2, 2).c_str(), nullptr, 16));
        o.insert(o.end(), u, u + 16);
    };
    for (size_t i = 0; i < np; i++) {
        const JV& pc = pp->a[i];
        put_str(o, jstr(pc, "PieceId"));
        static const char* types[] = {"unknown", "persona_skeleton", "persona_body", "persona_skin", "persona_bottom", "persona_feet", "persona_dress", "persona_top",
                                      "persona_high_pants", "persona_hands", "persona_outerwear", "persona_facial_hair", "persona_mouth", "persona_eyes", "persona_hair",
                                      "persona_eyebrows", "persona_capes", "persona_emote"};
        std::string pt = jstr(pc, "PieceType");
        int ti = 0;
        for (int k = 0; k < 18; k++) {
            std::string tn = types[k];
            if (pt == tn || (tn.size() > 8 && pt == tn.substr(8))) ti = k;
        }
        put_i32(o, ti);
        put_uuid(jstr(pc, "PackId"));
        o.push_back(jbool(pc, "IsDefault") ? 1 : 0);
        put_str(o, jstr(pc, "ProductId"));
    }
    const JV* tc = cd.get("PieceTintColors");
    size_t nt = tc && tc->t == JV::Arr ? std::min<size_t>(tc->a.size(), 64) : 0;
    put_varuint(o, nt);
    for (size_t i = 0; i < nt; i++) {
        put_str(o, jstr(tc->a[i], "PieceType"));
        const JV* cs = tc->a[i].get("Colors");
        for (int k = 0; k < 4; k++) {
            uint32_t c = 0xff000000;
            if (cs && cs->t == JV::Arr && static_cast<size_t>(k) < cs->a.size() && cs->a[static_cast<size_t>(k)].s.size() == 7) c = 0xff000000u | static_cast<uint32_t>(strtoul(cs->a[static_cast<size_t>(k)].s.c_str() + 1, nullptr, 16));
            put_i32(o, static_cast<int32_t>(c));
        }
    }
    o.push_back(jbool(cd, "PremiumSkin") ? 1 : 0);
    o.push_back(jbool(cd, "PersonaSkin") ? 1 : 0);
    o.push_back(jbool(cd, "CapeOnClassicSkin") ? 1 : 0);
    o.push_back(1);   // primary user
    o.push_back(0);   // overriding player appearance
    put_str(o, "");
    put_str(o, "");
    out = std::move(o);
    return true;
}

}  // namespace pn
