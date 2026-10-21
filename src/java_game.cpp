// Java Edition side of the shared game: entity/tab-list/block/time packets for events, and player input handling.
#include "log.hpp"
#include "perf.hpp"
#include "server.hpp"
#include "dimension.hpp"
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

struct CmdDef { const char* name; int level; };
static const CmdDef kCommands[] = {{"help", 0}, {"list", 0}, {"players", 0}, {"whois", 0}, {"playerinfo", 0}, {"tps", 0}, {"me", 0}, {"tell", 0}, {"msg", 0}, {"w", 0}, {"version", 0}, {"info", 0}, {"painite", 0}, {"pn", 0}, {"say", 2}, {"tp", 2},
    {"teleport", 2}, {"execute", 2}, {"locate", 2}, {"gamemode", 2}, {"time", 2}, {"setblock", 2}, {"fill", 2}, {"kick", 3}, {"ban", 3}, {"ban-ip", 3}, {"pardon", 3}, {"pardon-ip", 3},
    {"banlist", 3}, {"whitelist", 3}, {"op", 3}, {"deop", 3}, {"save-all", 4}, {"save-on", 4}, {"save-off", 4}, {"stop", 4}};

// Command tree for the client: every command is a literal with one greedy text argument, so tab completion and highlighting
// know the names; the server does the parsing.
void Worker::send_command_tree(JConn& c) {
    int string_parser = c.vd->reg_id("command_argument_type/string", -1);
    int level = s_.game.op_level(c.gid);
    std::vector<const CmdDef*> avail;
    for (auto& d : kCommands) if (d.level <= level) avail.push_back(&d);
    Wr w;
    w.varint(c.vd->id.pl_commands);
    size_t n = avail.size();
    w.varint(static_cast<int32_t>(1 + 2 * n));
    w.u8(0);                                         // root
    w.varint(static_cast<int32_t>(n));
    for (size_t i = 0; i < n; i++) w.varint(static_cast<int32_t>(1 + 2 * i));
    for (size_t i = 0; i < n; i++) {
        w.u8(0x01 | 0x04);                           // literal, executable
        if (string_parser >= 0) { w.varint(1); w.varint(static_cast<int32_t>(2 + 2 * i)); } else w.varint(0);
        w.str(avail[i]->name);
        if (string_parser >= 0) {
            w.u8(0x02 | 0x04);                       // argument, executable
            w.varint(0);
            w.str("args");
            w.varint(string_parser);
            w.varint(2);                             // greedy phrase
        } else {
            w.u8(0x01 | 0x04);                       // keep the node count right: an unused literal
            w.varint(0);
            w.str("-");
        }
    }
    w.varint(0);                                     // root index
    send(c, w.b);
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
    c.tp_wait = ++c.tp_id;
    w.varint(c.tp_id);
    w.f64(p.x); w.f64(p.y); w.f64(p.z);
    w.f64(0); w.f64(0); w.f64(0);
    w.f32(p.yaw); w.f32(p.pitch);
    w.be32(0);
    send(c, w.b);
    on_move(c, p.x, p.y, p.z, p.yaw, p.pitch, true, false, true);
}

// The player moved to another dimension: respawn packet, new position, chunks of the new dimension.
void Worker::change_dimension(JConn& c, const GPos& p) {
    const VanillaData& vd = *c.vd;
    c.dim = p.dim;
    c.sent.clear();
    c.pending.clear();
    Wr w;
    w.varint(vd.packet("play/c/respawn"));
    write_spawn_info(c, w);
    w.u8(0);   // keep nothing
    send(c, w.b);
    Wr ab;
    ab.varint(vd.id.pl_abilities);
    ab.u8(c.gamemode == 1 ? 0x0d : c.gamemode == 3 ? 0x0f : 0x00);
    ab.f32(0.05f);
    ab.f32(0.1f);
    send(c, ab.b);
    teleport_player(c, p);
    c.cx = static_cast<int>(std::floor(c.x / 16));
    c.cz = static_cast<int>(std::floor(c.z / 16));
    Wr ce;
    ce.varint(vd.id.pl_center);
    ce.varint(c.cx);
    ce.varint(c.cz);
    send(c, ce.b);
    Wr ge;
    ge.varint(vd.id.pl_game_event);
    ge.u8(13);
    ge.f32(0);
    send(c, ge.b);
    send_time(c, s_.game.game_time(), s_.game.day_time());
    rebuild_pending(c);
    pump_chunks(c);
}

// An item stack on the wire. The item id is looked up by name in the connection's own registry; component patches are
// version specific encodings, so a stack that carries one is only sent as is to a client of the data version Painite loaded them from.
static void write_stack(Wr& w, const sim::ItemStack& st, const VanillaData& vd) {
    if (st.is_empty()) { w.varint(0); return; }
    int id = vd.reg_id("item/" + st.item()->name.substr(st.item()->name.find(':') + 1), -1);
    if (id < 0) { w.varint(0); return; }
    w.varint(st.count());
    w.varint(id);
    if (vd.version == g_gameplay_version) {
        w.varint(static_cast<int32_t>(st.patch().added.size()));
        w.varint(static_cast<int32_t>(st.patch().removed.size()));
        for (auto& kv : st.patch().added) { w.varint(kv.first); w.raw(kv.second.data(), kv.second.size()); }
        for (int r : st.patch().removed) w.varint(r);
    } else {
        w.varint(0);
        w.varint(0);
    }
}

static void write_lp_vec3(Wr& w, const sim::Vec3& value) {
    auto sanitize = [](double v) {
        if (std::isnan(v)) return 0.0;
        return std::clamp(v, -1.7179869183E10, 1.7179869183E10);
    };
    const double x = sanitize(value.x), y = sanitize(value.y), z = sanitize(value.z);
    const double max = std::max(std::abs(x), std::max(std::abs(y), std::abs(z)));
    if (max < 3.051944088384301E-5) {
        w.u8(0);
        return;
    }
    const uint64_t scale = static_cast<uint64_t>(std::ceil(max));
    const bool extra = (scale & 3u) != scale;
    const uint64_t prefix = extra ? (scale & 3u) | 4u : scale;
    auto pack = [](double v) { return static_cast<uint64_t>(std::llround((v * 0.5 + 0.5) * 32766.0)); };
    const uint64_t packed = prefix | (pack(x / scale) << 3) | (pack(y / scale) << 18) | (pack(z / scale) << 33);
    w.u8(static_cast<uint8_t>(packed));
    w.u8(static_cast<uint8_t>(packed >> 8));
    w.varint(static_cast<int32_t>(static_cast<uint32_t>(packed >> 16)));
    if (extra) w.varint(static_cast<int32_t>(scale >> 2));
}

static void write_entity_data(Wr& w, const std::vector<sim::Entity::DataItem>& data) {
    for (const auto& item : data) {
        w.u8(static_cast<uint8_t>(item.index));
        w.varint(item.serializer);
        if (!item.value.empty()) w.raw(item.value.data(), item.value.size());
    }
    w.u8(0xff);
}

void Worker::java_event(JConn& c, const GEvent& ev) {
    PN_PERF("event.java");
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
            {
                Wr m;   // skin layers: entity data index 16, type byte (checked against a vanilla server by tools/capture_play.py)
                m.varint(vd.packet("play/c/set_entity_data"));
                m.varint(ev.id->eid);
                m.u8(16); m.varint(0); m.u8(ev.pos.skin_parts);
                m.u8(0xff);
                send(c, m.b);
            }
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
            bool v3 = vd.protocol >= 777;   // 26.3 packs the ground flag and a step count into a varint and puts it before the deltas
            int props = ev.pos.ground ? 1 : 0;
            auto head = [&](const char* name) { w.varint(vd.packet(name)); w.varint(ev.id->eid); if (v3) w.varint(props); };
            auto deltas = [&]() { w.be16(static_cast<uint16_t>(ev.dx)); w.be16(static_cast<uint16_t>(ev.dy)); w.be16(static_cast<uint16_t>(ev.dz)); };
            if (ev.moved && ev.turned) {
                head("play/c/move_entity_pos_rot");
                deltas();
                w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch));
                if (!v3) w.boolean(ev.pos.ground);
            } else if (ev.moved || !ev.turned) {
                head("play/c/move_entity_pos");
                deltas();
                if (!v3) w.boolean(ev.pos.ground);
            } else {
                w.varint(vd.packet("play/c/move_entity_rot"));
                w.varint(ev.id->eid);
                if (v3) { w.boolean(ev.pos.ground); w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch)); }
                else { w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch)); w.boolean(ev.pos.ground); }
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
        case GEvent::Dimension:
            if (ev.id->gid == c.gid) change_dimension(c, ev.pos);
            return;
        case GEvent::Teleport: {
            if (ev.id->gid == c.gid) { if (ev.value == 1) teleport_player(c, ev.pos); return; }
            w.varint(vd.packet("play/c/entity_position_sync"));
            w.varint(ev.id->eid);
            if (vd.protocol >= 777) w.varint(0);   // linear path
            w.f64(ev.pos.x); w.f64(ev.pos.y); w.f64(ev.pos.z);
            if (vd.protocol < 777) { w.f64(0); w.f64(0); w.f64(0); }   // velocity
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
            if (vd.protocol >= 777) {
                w.varint(vd.packet("play/c/swing_animation"));
                w.varint(ev.id->eid);
                w.varint(0);
                w.varint(1);
                w.varint(6);
            } else {
                w.varint(vd.packet("play/c/animate"));
                w.varint(ev.id->eid);
                w.u8(0);   // swing main arm
            }
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
        case GEvent::ContainerContent: {
            w.varint(vd.packet("play/c/container_set_content"));
            w.varint(ev.container);
            w.varint(ev.state);
            w.varint(static_cast<int32_t>(ev.items.size()));
            for (auto& st : ev.items) write_stack(w, st, vd);
            write_stack(w, ev.stack, vd);
            send(c, w.b);
            break;
        }
        case GEvent::ContainerSlot:
            w.varint(vd.packet("play/c/container_set_slot"));
            w.varint(ev.container);
            w.varint(ev.state);
            w.be16(static_cast<uint16_t>(ev.slot));
            write_stack(w, ev.stack, vd);
            send(c, w.b);
            break;
        case GEvent::SetCursor:
            w.varint(vd.packet("play/c/set_cursor_item"));
            write_stack(w, ev.stack, vd);
            send(c, w.b);
            break;
        case GEvent::SetPlayerSlot:
            w.varint(vd.packet("play/c/set_player_inventory"));
            w.varint(ev.slot);
            write_stack(w, ev.stack, vd);
            send(c, w.b);
            break;
        case GEvent::HeldSlot:
            w.varint(vd.packet("play/c/set_held_slot"));
            w.varint(ev.slot);
            send(c, w.b);
            break;
        case GEvent::ContainerClose:
            w.varint(vd.packet("play/c/container_close"));
            w.varint(ev.container);
            send(c, w.b);
            break;
        case GEvent::EntityAdd: {
            std::string type_name = ev.entity_kind;
            size_t colon = type_name.find(':');
            if (colon != std::string::npos) type_name.erase(0, colon + 1);
            int type = vd.reg_id("entity_type/" + type_name);
            if (type < 0) return;
            w.varint(vd.packet("play/c/add_entity"));
            w.varint(ev.entity_id);
            w.raw(ev.entity_uuid, sizeof ev.entity_uuid);
            w.varint(type);
            w.f64(ev.pos.x); w.f64(ev.pos.y); w.f64(ev.pos.z);
            write_lp_vec3(w, ev.entity_velocity);
            w.u8(angle_byte(ev.pos.pitch));
            w.u8(angle_byte(ev.pos.yaw));
            w.u8(angle_byte(ev.pos.head));
            w.varint(0);
            send(c, w.b);
            if (!ev.entity_data.empty()) {
                Wr data;
                data.varint(vd.packet("play/c/set_entity_data"));
                data.varint(ev.entity_id);
                write_entity_data(data, ev.entity_data);
                send(c, data.b);
            }
            break;
        }
        case GEvent::EntityRemove:
            w.varint(vd.packet("play/c/remove_entities"));
            w.varint(1);
            w.varint(ev.entity_id);
            send(c, w.b);
            break;
        case GEvent::EntityMove: {
            bool v3 = vd.protocol >= 777;
            int props = ev.pos.ground ? 1 : 0;
            auto head = [&](const char* name) {
                w.varint(vd.packet(name));
                w.varint(ev.entity_id);
                if (v3) w.varint(props);
            };
            if (ev.moved && ev.turned) {
                head("play/c/move_entity_pos_rot");
                w.be16(static_cast<uint16_t>(ev.dx)); w.be16(static_cast<uint16_t>(ev.dy)); w.be16(static_cast<uint16_t>(ev.dz));
                w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch));
                if (!v3) w.boolean(ev.pos.ground);
            } else if (ev.moved || !ev.turned) {
                head("play/c/move_entity_pos");
                w.be16(static_cast<uint16_t>(ev.dx)); w.be16(static_cast<uint16_t>(ev.dy)); w.be16(static_cast<uint16_t>(ev.dz));
                if (!v3) w.boolean(ev.pos.ground);
            } else {
                w.varint(vd.packet("play/c/move_entity_rot"));
                w.varint(ev.entity_id);
                if (v3) { w.boolean(ev.pos.ground); w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch)); }
                else { w.u8(angle_byte(ev.pos.yaw)); w.u8(angle_byte(ev.pos.pitch)); w.boolean(ev.pos.ground); }
            }
            send(c, w.b);
            if (ev.turned) {
                Wr head;
                head.varint(vd.packet("play/c/rotate_head"));
                head.varint(ev.entity_id);
                head.u8(angle_byte(ev.pos.head));
                send(c, head.b);
            }
            break;
        }
        case GEvent::EntitySync:
            w.varint(vd.packet("play/c/entity_position_sync"));
            w.varint(ev.entity_id);
            if (vd.protocol >= 777) w.varint(0);
            w.f64(ev.pos.x); w.f64(ev.pos.y); w.f64(ev.pos.z);
            if (vd.protocol < 777) {
                w.f64(ev.entity_velocity.x); w.f64(ev.entity_velocity.y); w.f64(ev.entity_velocity.z);
            }
            w.f32(ev.pos.yaw); w.f32(ev.pos.pitch);
            w.boolean(ev.pos.ground);
            send(c, w.b);
            if (ev.turned) {
                Wr head;
                head.varint(vd.packet("play/c/rotate_head"));
                head.varint(ev.entity_id);
                head.u8(angle_byte(ev.pos.head));
                send(c, head.b);
            }
            break;
        case GEvent::EntityMotion:
            w.varint(vd.packet("play/c/set_entity_motion"));
            w.varint(ev.entity_id);
            write_lp_vec3(w, ev.entity_velocity);
            send(c, w.b);
            break;
        case GEvent::EntityData:
            w.varint(vd.packet("play/c/set_entity_data"));
            w.varint(ev.entity_id);
            write_entity_data(w, ev.entity_data);
            send(c, w.b);
            break;
        case GEvent::EntityTake:
            if (vd.packet("play/c/take_item_entity") < 0) return;
            w.varint(vd.packet("play/c/take_item_entity"));
            w.varint(ev.entity_id);
            w.varint(ev.other_entity_id);
            w.varint(ev.value);
            send(c, w.b);
            break;
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
    if (r.bad || !c.gid) return;
    // the wire item id is the id of the client's own registry: take the name from it, then build the stack for Painite's registry
    sim::ItemStack st;
    {
        Rd peek = r;
        int count = peek.varint();
        if (count > 0) {
            int wire_id = peek.varint();
            if (peek.bad || wire_id < 0 || static_cast<size_t>(wire_id) >= c.vd->items.size()) return;
            const sim::ItemType* t = sim::Items::get().by_name(c.vd->items[static_cast<size_t>(wire_id)]);
            if (!t) return;
            st = sim::ItemStack(t, count);
            int pos = peek.varint(), neg = peek.varint();
            if (peek.bad || pos < 0 || neg < 0) return;
            if (c.vd->version == g_gameplay_version) {   // component values are only kept for the data version Painite loaded
                for (int i = 0; i < pos && !peek.bad; i++) {
                    int type = peek.varint(), len = peek.varint();
                    if (peek.bad || len < 0 || static_cast<size_t>(len) > peek.left()) return;
                    st.patch().added[type] = std::vector<uint8_t>(peek.p, peek.p + len);
                    peek.p += len;
                }
                for (int i = 0; i < neg && !peek.bad; i++) st.patch().removed.insert(peek.varint());
            }
        }
    }
    s_.game.inv_creative_slot(c.gid, slot, st);
    // placing still reads the hotbar mirror kept on the connection
    if (slot >= 36 && slot <= 44) {
        int idx = slot - 36;
        c.hotbar[idx] = 0;
        c.hotbar_name[idx].clear();
        if (!st.is_empty()) {
            c.hotbar_name[idx] = st.item()->name;
            c.hotbar[idx] = Blocks::get().default_state(st.item()->name);
        }
    }
}

void Worker::on_container_click(JConn& c, Rd& r) {
    int container = r.varint();
    int state = r.varint();
    int slot = static_cast<int16_t>(r.be16());
    int button = static_cast<int8_t>(r.u8());
    int mode = r.varint();
    auto hashed = [&](sim::HashedLite& h) {
        h = sim::HashedLite();
        h.present = r.boolean();
        if (!h.present) return;
        h.item = r.varint();
        h.count = r.varint();
        int pos = r.varint();
        int neg = r.varint();
        if (pos < 0 || neg < 0 || pos > 256 || neg > 256) { r.bad = true; return; }
        for (int i = 0; i < pos && !r.bad; i++) { int t = r.varint(); h.added[t] = static_cast<int32_t>(r.be32()); }
        for (int i = 0; i < neg && !r.bad; i++) h.removed.insert(r.varint());
    };
    int n = r.varint();
    if (r.bad || n < 0 || n > 128) return;
    std::map<int, sim::HashedLite> changed;
    for (int i = 0; i < n && !r.bad; i++) {
        int sl = static_cast<int16_t>(r.be16());
        sim::HashedLite h;
        hashed(h);
        changed[sl] = h;
    }
    sim::HashedLite carried;
    hashed(carried);
    if (r.bad || !c.gid) return;
    s_.game.inv_click(c.gid, container, state, slot, button, mode, changed, carried);
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
    World& W = s_.dim(c.dim);
    if (status == 0 || status == 2) {
        bool take = (status == 0 && instant) || (status == 2 && c.gamemode != 1 && c.gamemode != 3 && c.gamemode != 2);
        double dx = x + 0.5 - c.x, dy = y + 0.5 - (c.y + 1.62), dz = z + 0.5 - c.z;
        bool near = dx * dx + dy * dy + dz * dz <= 7.0 * 7.0;
        bool protect = c.dim == 0 && s_.cfg.spawn_protection > 0 && std::max(std::abs(x - s_.world.spawn_x), std::abs(z - s_.world.spawn_z)) <= s_.cfg.spawn_protection &&
                       s_.game.op_level(c.gid) == 0;
        uint16_t cur = W.get_block(x, y, z);
        const BlockInfo& bi = Blocks::get().info(cur);
        if (take && near && !protect && !bi.air && bi.name != "minecraft:bedrock" && W.set_block(x, y, z, 0)) {
            s_.game.block_changed(c.dim, x, y, z, 0);
        } else if (status == 0 || status == 2) {
            send_block_update(c, x, y, z, W.get_block(x, y, z));
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
    World& W = s_.dim(c.dim);
    uint16_t target = W.get_block(x, y, z);
    int tx = x, ty = y, tz = z;
    if (!B.info(target).replaceable) { tx += face_dx(face); ty += face_dy(face); tz += face_dz(face); }
    const DimInfo& di = dim_info(c.dim);
    if (hand == 0 && c.gamemode != 3 && c.gamemode != 2) {
        const std::string& held = c.hotbar_name[c.held];
        if (held == "minecraft:ender_eye" && s_.game.put_ender_eye(c.dim, x, y, z)) { send_block_ack(c, seq); return; }
        if (held == "minecraft:flint_and_steel" || held == "minecraft:fire_charge") {
            // lighting a nether portal: fire inside an obsidian frame turns the frame's inside into portal
            if (ty >= di.min_y && ty < di.min_y + di.height && s_.game.portal_ignite(c.dim, tx, ty, tz)) { send_block_ack(c, seq); return; }
        }
    }
    bool ok = item_block && c.gamemode != 3 && c.gamemode != 2 && ty >= di.min_y && ty < di.min_y + di.height;
    if (ok) {
        double dx = tx + 0.5 - c.x, dz = tz + 0.5 - c.z, dy = ty + 0.5 - (c.y + 1.62);
        ok = dx * dx + dy * dy + dz * dz <= 8.0 * 8.0;
        // not inside the player's own body
        if (ok && std::abs(c.x - (tx + 0.5)) < 0.8 && std::abs(c.z - (tz + 0.5)) < 0.8 && ty >= static_cast<int>(std::floor(c.y)) && ty <= static_cast<int>(std::floor(c.y + 1.8))) ok = false;
        if (ok && !B.info(W.get_block(tx, ty, tz)).replaceable) ok = false;
        if (ok && c.dim == 0 && s_.cfg.spawn_protection > 0 && std::max(std::abs(tx - s_.world.spawn_x), std::abs(tz - s_.world.spawn_z)) <= s_.cfg.spawn_protection &&
            s_.game.op_level(c.gid) == 0)
            ok = false;
    }
    if (ok) {
        uint16_t state = placed_state(item_block, face, c.yaw, cy);
        if (W.set_block(tx, ty, tz, state)) s_.game.block_changed(c.dim, tx, ty, tz, state);
        else send_block_update(c, tx, ty, tz, W.get_block(tx, ty, tz));
    } else {
        send_block_update(c, x, y, z, target);
        if (tx != x || ty != y || tz != z) send_block_update(c, tx, ty, tz, W.get_block(tx, ty, tz));
    }
    send_block_ack(c, seq);
}

}  // namespace pn
