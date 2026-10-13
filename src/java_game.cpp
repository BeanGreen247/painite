// Java Edition side of the shared game: entity/tab-list/block/time packets for events, and player input handling.
#include "log.hpp"
#include "server.hpp"
#include <algorithm>
#include <cmath>

namespace pn {

uint64_t pack_block_pos(int x, int y, int z);

static uint8_t angle_byte(float a) { return static_cast<uint8_t>(static_cast<int>(std::floor(a * 256.0f / 360.0f)) & 255); }

static void put_entry(Wr& w, const GIdentity& id, int gamemode) {
    w.raw(id.uuid, 16);
    w.str(id.name);
    w.varint(static_cast<int32_t>(id.props.size()));
    for (auto& p : id.props) {
        w.str(p.name);
        w.str(p.value);
        w.boolean(!p.signature.empty());
        if (!p.signature.empty()) w.str(p.signature);
    }
    w.boolean(false);      // chat session
    w.varint(gamemode);
    w.boolean(true);       // listed
    w.varint(0);           // latency
    w.boolean(false);      // display name
    w.varint(0);           // list priority
    w.boolean(true);       // hat
}

void Worker::send_block_ack(JConn& c, int seq) {
    Wr w;
    w.varint(c.vd->packet("play/c/block_changed_ack"));
    w.varint(seq);
    send(c, w.b);
}

void Worker::send_block_update(JConn& c, int x, int y, int z, uint16_t block) {
    Wr w;
    w.varint(c.vd->packet("play/c/block_update"));
    w.be64(pack_block_pos(x, y, z));
    w.varint(Blocks::get().proto_state(c.vd->protocol, block));
    send(c, w.b);
}

void Worker::send_time(JConn& c, int64_t age, int64_t day) {
    Wr w;
    w.varint(c.vd->id.pl_time);
    w.be64(static_cast<uint64_t>(age));
    w.varint(2);
    for (int clock : {1, 0}) {
        w.varint(clock);
        // varlong day time
        uint64_t v = static_cast<uint64_t>(day);
        while (v >= 0x80) { w.u8(static_cast<uint8_t>(v | 0x80)); v >>= 7; }
        w.u8(static_cast<uint8_t>(v));
        w.f32(0.0f);
        w.f32(1.0f);
    }
    send(c, w.b);
}

void Worker::teleport_player(JConn& c, const GPos& p) {
    c.x = p.x; c.y = p.y; c.z = p.z; c.yaw = p.yaw; c.pitch = p.pitch;
    Wr w;
    w.varint(c.vd->id.pl_position);
    w.varint(++c.tp_id);
    w.f64(p.x); w.f64(p.y); w.f64(p.z);
    w.f64(0); w.f64(0); w.f64(0);
    w.f32(p.yaw); w.f32(p.pitch);
    w.be32(0);
    send(c, w.b);
    on_move(c, p.x, p.y, p.z, p.yaw, p.pitch, true, false, true);
}

void Worker::java_event(JConn& c, const GEvent& ev) {
    const VanillaData& vd = *c.vd;
    const auto& id = vd.id;
    Wr w;
    switch (ev.kind) {
        case GEvent::AddTab: {
            if (ev.id->gid == c.gid) return;
            w.varint(id.pl_player_info);
            w.u8(0xff);
            w.varint(1);
            put_entry(w, *ev.id, ev.value);
            send(c, w.b);
            break;
        }
        case GEvent::RemoveTab:
            w.varint(vd.packet("play/c/player_info_remove"));
            w.varint(1);
            w.raw(ev.id->uuid, 16);
            send(c, w.b);
            break;
        case GEvent::Spawn: {
            if (ev.id->gid == c.gid) return;
            w.varint(vd.packet("play/c/add_entity"));
            w.varint(ev.id->eid);
            w.raw(ev.id->uuid, 16);
            w.varint(vd.reg_id("entity_type/player", 0));
            w.f64(ev.pos.x); w.f64(ev.pos.y); w.f64(ev.pos.z);
            w.u8(0);   // velocity (LpVec3 zero)
            w.u8(angle_byte(ev.pos.pitch));
            w.u8(angle_byte(ev.pos.yaw));
            w.u8(angle_byte(ev.pos.head));
            w.varint(0);
            send(c, w.b);
            break;
        }
        case GEvent::Despawn:
            w.varint(vd.packet("play/c/remove_entities"));
            w.varint(1);
            w.varint(ev.id->eid);
            send(c, w.b);
            break;
        case GEvent::Move: {
            if (ev.id->gid == c.gid) return;
            int props = ev.pos.ground ? 1 : 0;
            if (ev.moved && ev.turned) {
                w.varint(vd.packet("play/c/move_entity_pos_rot"));
                w.varint(ev.id->eid); w.varint(props);
                w.be16(static_cast<uint16_t>(ev.dx)); w.be16(static_cast<uint16_t>(ev.dy)); w.be16(static_cast<uint16_t>(ev.dz));
                w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch));
            } else if (ev.moved || !ev.turned) {
                w.varint(vd.packet("play/c/move_entity_pos"));
                w.varint(ev.id->eid); w.varint(props);
                w.be16(static_cast<uint16_t>(ev.dx)); w.be16(static_cast<uint16_t>(ev.dy)); w.be16(static_cast<uint16_t>(ev.dz));
            } else {
                w.varint(vd.packet("play/c/move_entity_rot"));
                w.varint(ev.id->eid);
                w.boolean(ev.pos.ground);
                w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch));
            }
            send(c, w.b);
            if (ev.turned) {
                Wr h;
                h.varint(vd.packet("play/c/rotate_head"));
                h.varint(ev.id->eid);
                h.u8(angle_byte(ev.pos.head));
                send(c, h.b);
            }
            break;
        }
        case GEvent::Teleport: {
            if (ev.id->gid == c.gid) { if (ev.value == 1) teleport_player(c, ev.pos); return; }
            w.varint(vd.packet("play/c/entity_position_sync"));
            w.varint(ev.id->eid);
            w.varint(0);
            w.f64(ev.pos.x); w.f64(ev.pos.y); w.f64(ev.pos.z);
            w.f32(ev.pos.yaw); w.f32(ev.pos.pitch);
            w.boolean(ev.pos.ground);
            send(c, w.b);
            Wr h;
            h.varint(vd.packet("play/c/rotate_head"));
            h.varint(ev.id->eid);
            h.u8(angle_byte(ev.pos.head));
            send(c, h.b);
            break;
        }
        case GEvent::Swing:
            w.varint(vd.packet("play/c/swing_animation"));
            w.varint(ev.id->eid);
            w.varint(0);
            w.varint(1);
            w.varint(6);
            send(c, w.b);
            break;
        case GEvent::Block: send_block_update(c, ev.bx, ev.by, ev.bz, ev.block); break;
        case GEvent::Time: send_time(c, ev.t0, ev.t1); break;
        case GEvent::Gamemode:
            w.varint(id.pl_player_info);
            w.u8(0x04);
            w.varint(1);
            w.raw(ev.id->uuid, 16);
            w.varint(ev.value);
            send(c, w.b);
            if (ev.id->gid == c.gid) {
                c.gamemode = ev.value;
                Wr g;
                g.varint(id.pl_game_event);
                g.u8(3);
                g.f32(static_cast<float>(ev.value));
                send(c, g.b);
                Wr ab;
                ab.varint(id.pl_abilities);
                ab.u8(ev.value == 1 ? 0x0d : ev.value == 3 ? 0x0f : 0x00);
                ab.f32(0.05f);
                ab.f32(0.1f);
                send(c, ab.b);
            }
            break;
        case GEvent::Message: send_chat(c, ev.text); break;
        case GEvent::Kick: kick(c, ev.text); break;
        default: break;
    }
}

// Chooses block state properties for a block placed against `face` by a player looking along `yaw`.
static uint16_t placed_state(uint16_t base, int face, float yaw, float cursor_y) {
    Blocks& B = Blocks::get();
    const BlockInfo& bi = B.info(base);
    std::string name = bi.name;
    std::vector<std::pair<std::string, std::string>> kv;
    size_t i = 0;
    while (i < bi.props.size()) {
        size_t e = bi.props.find(',', i);
        std::string t = bi.props.substr(i, e == std::string::npos ? std::string::npos : e - i);
        size_t eq = t.find('=');
        kv.emplace_back(t.substr(0, eq), t.substr(eq + 1));
        if (e == std::string::npos) break;
        i = e + 1;
    }
    auto set = [&](const char* k, const std::string& v) { for (auto& p : kv) if (p.first == k) p.second = v; };
    auto has = [&](const char* k) { for (auto& p : kv) if (p.first == k) return true; return false; };
    static const char* look[4] = {"south", "west", "north", "east"};
    static const char* opp[4] = {"north", "east", "south", "west"};
    int q = static_cast<int>(std::floor(yaw / 90.0f + 0.5f)) & 3;
    if (name.find("torch") != std::string::npos && name.find("wall") == std::string::npos && face >= 2 && face <= 5) {
        std::string wall = name;
        wall.insert(wall.find("torch"), "wall_");
        size_t colon = wall.find(':');
        (void)colon;
        uint16_t w = B.default_state(wall);
        if (w) {
            static const char* dir[6] = {"", "", "north", "south", "west", "east"};
            std::string props = std::string("facing=") + dir[face];
            return B.find(wall, props);
        }
    }
    if (has("axis")) set("axis", face <= 1 ? "y" : face <= 3 ? "z" : "x");
    if (has("facing") && !has("face")) {
        bool horiz = true;
        for (auto& p : kv) if (p.first == "facing" && (p.second == "up" || p.second == "down")) horiz = false;
        (void)horiz;
        bool toward = name.find("stairs") == std::string::npos && name.find("door") == std::string::npos && name.find("gate") == std::string::npos &&
                      name.find("bed") == std::string::npos && name.find("anvil") == std::string::npos;
        set("facing", toward ? opp[q] : look[q]);
    }
    if (has("type") && name.find("slab") != std::string::npos) set("type", face == 0 ? "top" : face == 1 ? "bottom" : cursor_y >= 0.5f ? "top" : "bottom");
    if (has("half") && (name.find("stairs") != std::string::npos || name.find("trapdoor") != std::string::npos))
        set("half", face == 0 ? "top" : face == 1 ? "bottom" : cursor_y >= 0.5f ? "top" : "bottom");
    if (has("persistent")) set("persistent", "true");
    std::sort(kv.begin(), kv.end());
    std::string props;
    for (auto& p : kv) props += (props.empty() ? "" : ",") + p.first + "=" + p.second;
    uint16_t id = B.find(name, props);
    return id ? id : base;
}

void Worker::on_creative_slot(JConn& c, Rd& r) {
    int slot = static_cast<int16_t>(r.be16());
    int count = r.varint();
    if (r.bad || slot < 36 || slot > 44) return;
    int idx = slot - 36;
    c.hotbar[idx] = 0;
    if (count <= 0) return;
    int item = r.varint();
    if (r.bad || item < 0 || static_cast<size_t>(item) >= c.vd->items.size()) return;
    c.hotbar[idx] = Blocks::get().default_state(c.vd->items[static_cast<size_t>(item)]);
}

void Worker::on_dig(JConn& c, Rd& r) {
    int status = r.varint();
    uint64_t p = r.be64();
    int face = r.u8();
    int seq = r.varint();
    if (r.bad) return;
    (void)face;
    int x = static_cast<int>(static_cast<int64_t>(p) >> 38);
    int y = static_cast<int>(static_cast<int64_t>(p << 52) >> 52);
    int z = static_cast<int>(static_cast<int64_t>(p << 26) >> 38);
    bool instant = c.gamemode == 1;
    if (status == 0 || status == 2) {
        bool take = (status == 0 && instant) || (status == 2 && c.gamemode != 1 && c.gamemode != 3 && c.gamemode != 2);
        double dx = x + 0.5 - c.x, dy = y + 0.5 - (c.y + 1.62), dz = z + 0.5 - c.z;
        bool near = dx * dx + dy * dy + dz * dz <= 7.0 * 7.0;
        bool protect = s_.cfg.spawn_protection > 0 && std::max(std::abs(x - s_.world.spawn_x), std::abs(z - s_.world.spawn_z)) <= s_.cfg.spawn_protection &&
                       s_.game.op_level(c.gid) == 0;
        uint16_t cur = s_.world.get_block(x, y, z);
        const BlockInfo& bi = Blocks::get().info(cur);
        if (take && near && !protect && !bi.air && bi.name != "minecraft:bedrock" && s_.world.set_block(x, y, z, 0)) {
            s_.game.block_changed(x, y, z, 0);
        } else if (status == 0 || status == 2) {
            send_block_update(c, x, y, z, s_.world.get_block(x, y, z));
        }
    }
    send_block_ack(c, seq);
}

static int face_dx(int f) { return f == 4 ? -1 : f == 5 ? 1 : 0; }
static int face_dy(int f) { return f == 0 ? -1 : f == 1 ? 1 : 0; }
static int face_dz(int f) { return f == 2 ? -1 : f == 3 ? 1 : 0; }

void Worker::on_place(JConn& c, Rd& r) {
    int hand = r.varint();
    uint64_t p = r.be64();
    int face = r.varint();
    float cx = r.f32(), cy = r.f32(), cz = r.f32();
    (void)cx; (void)cz;
    r.boolean();
    r.boolean();
    int seq = r.varint();
    if (r.bad) return;
    int x = static_cast<int>(static_cast<int64_t>(p) >> 38);
    int y = static_cast<int>(static_cast<int64_t>(p << 52) >> 52);
    int z = static_cast<int>(static_cast<int64_t>(p << 26) >> 38);
    Blocks& B = Blocks::get();
    uint16_t item_block = hand == 0 ? c.hotbar[c.held] : 0;
    uint16_t target = s_.world.get_block(x, y, z);
    int tx = x, ty = y, tz = z;
    if (!B.info(target).replaceable) { tx += face_dx(face); ty += face_dy(face); tz += face_dz(face); }
    bool ok = item_block && c.gamemode != 3 && c.gamemode != 2 && ty >= kMinY && ty < kMinY + kHeight;
    if (ok) {
        double dx = tx + 0.5 - c.x, dz = tz + 0.5 - c.z, dy = ty + 0.5 - (c.y + 1.62);
        ok = dx * dx + dy * dy + dz * dz <= 8.0 * 8.0;
        // not inside the player's own body
        if (ok && std::abs(c.x - (tx + 0.5)) < 0.8 && std::abs(c.z - (tz + 0.5)) < 0.8 && ty >= static_cast<int>(std::floor(c.y)) && ty <= static_cast<int>(std::floor(c.y + 1.8))) ok = false;
        if (ok && !B.info(s_.world.get_block(tx, ty, tz)).replaceable) ok = false;
        if (ok && s_.cfg.spawn_protection > 0 && std::max(std::abs(tx - s_.world.spawn_x), std::abs(tz - s_.world.spawn_z)) <= s_.cfg.spawn_protection &&
            s_.game.op_level(c.gid) == 0)
            ok = false;
    }
    if (ok) {
        uint16_t state = placed_state(item_block, face, c.yaw, cy);
        if (s_.world.set_block(tx, ty, tz, state)) s_.game.block_changed(tx, ty, tz, state);
        else send_block_update(c, tx, ty, tz, s_.world.get_block(tx, ty, tz));
    } else {
        send_block_update(c, x, y, z, target);
        if (tx != x || ty != y || tz != z) send_block_update(c, tx, ty, tz, s_.world.get_block(tx, ty, tz));
    }
    send_block_ack(c, seq);
}

}  // namespace pn
