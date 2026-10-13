#!/usr/bin/env python3 -I
"""Map every Java block state to a Bedrock block runtime id (the FNV hash Bedrock servers use as network id).

Source of Bedrock's block list: Geyser's block_palette (MIT, https://github.com/GeyserMC/Geyser), which was dumped from a real
Bedrock Dedicated Server. Java states come from data/vanilla-<ver>.blocks. The translation is rule based (name changes, then
property conversions, then the closest Bedrock state); anything left over falls back to the block's default state.

usage: tools/fetch_bedrock_blocks.py [--blocks data/vanilla-26.3.blocks] [--out data] [--palette 26_50]
writes data/blocks-bedrock.bin (u32 magic 'PNBB', u32 count, count x i32 runtime id by Java state id) and data/biomes-bedrock.txt
"""
import argparse, gzip, json, os, struct, sys, urllib.request

PAL = "https://raw.githubusercontent.com/GeyserMC/Geyser/master/core/src/main/resources/bedrock/block_palette.{}.nbt"
BIO = "https://raw.githubusercontent.com/GeyserMC/mappings/master/biomes.json"


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "painite-data-tool"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def nbt_read(d, i, t):
    if t == 1: return struct.unpack(">b", d[i:i+1])[0], i + 1
    if t == 2: return struct.unpack(">h", d[i:i+2])[0], i + 2
    if t == 3: return struct.unpack(">i", d[i:i+4])[0], i + 4
    if t == 4: return struct.unpack(">q", d[i:i+8])[0], i + 8
    if t == 5: return struct.unpack(">f", d[i:i+4])[0], i + 4
    if t == 6: return struct.unpack(">d", d[i:i+8])[0], i + 8
    if t == 7:
        n = struct.unpack(">i", d[i:i+4])[0]; return d[i+4:i+4+n], i + 4 + n
    if t == 8:
        n = struct.unpack(">H", d[i:i+2])[0]; return d[i+2:i+2+n].decode(), i + 2 + n
    if t == 9:
        et = d[i]; n = struct.unpack(">i", d[i+1:i+5])[0]; i += 5; out = []
        for _ in range(n):
            v, i = nbt_read(d, i, et); out.append(v)
        return out, i
    if t == 10:
        out = {}
        while d[i] != 0:
            tt = d[i]; n = struct.unpack(">H", d[i+1:i+3])[0]; name = d[i+3:i+3+n].decode(); i += 3 + n
            out[name], i = nbt_read(d, i, tt)
        return out, i + 1
    if t == 11:
        n = struct.unpack(">i", d[i:i+4])[0]; return list(struct.unpack(f">{n}i", d[i+4:i+4+4*n])), i + 4 + 4 * n
    if t == 12:
        n = struct.unpack(">i", d[i:i+4])[0]; return list(struct.unpack(f">{n}q", d[i+4:i+4+8*n])), i + 4 + 8 * n
    raise ValueError(t)


COLORS = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray", "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"]
FACING = {"down": 0, "up": 1, "north": 2, "south": 3, "west": 4, "east": 5}
CARD = {"north": "north", "south": "south", "west": "west", "east": "east"}
DIRECTION = {"south": 0, "west": 1, "north": 2, "east": 3}          # doors, fence gates, beds, repeaters
WEIRDO = {"east": 0, "west": 1, "south": 2, "north": 3}             # stairs
TORCH_DIR = {"west": "east", "east": "west", "north": "south", "south": "north"}   # Bedrock torch_facing_direction names the opposite side

RENAME = {
    "minecraft:cave_air": "minecraft:air", "minecraft:void_air": "minecraft:air",
    "minecraft:bricks": "minecraft:brick_block", "minecraft:nether_bricks": "minecraft:nether_brick", "minecraft:red_nether_bricks": "minecraft:red_nether_brick",
    "minecraft:end_stone_bricks": "minecraft:end_bricks", "minecraft:cobweb": "minecraft:web", "minecraft:dirt_path": "minecraft:grass_path",
    "minecraft:lily_pad": "minecraft:waterlily", "minecraft:melon": "minecraft:melon_block", "minecraft:magma_block": "minecraft:magma",
    "minecraft:snow_block": "minecraft:snow", "minecraft:note_block": "minecraft:noteblock", "minecraft:nether_quartz_ore": "minecraft:quartz_ore",
    "minecraft:sugar_cane": "minecraft:reeds", "minecraft:slime_block": "minecraft:slime", "minecraft:spawner": "minecraft:mob_spawner",
    "minecraft:rooted_dirt": "minecraft:dirt_with_roots", "minecraft:powered_rail": "minecraft:golden_rail", "minecraft:jack_o_lantern": "minecraft:lit_pumpkin",
    "minecraft:comparator": "minecraft:unpowered_comparator", "minecraft:repeater": "minecraft:unpowered_repeater", "minecraft:terracotta": "minecraft:hardened_clay",
    "minecraft:light_gray_glazed_terracotta": "minecraft:silver_glazed_terracotta", "minecraft:tall_seagrass": "minecraft:seagrass",
    "minecraft:cobblestone_stairs": "minecraft:stone_stairs", "minecraft:end_stone_brick_stairs": "minecraft:end_brick_stairs",
    "minecraft:prismarine_brick_stairs": "minecraft:prismarine_bricks_stairs", "minecraft:flowering_azalea_leaves": "minecraft:azalea_leaves_flowered",
    "minecraft:small_dripleaf": "minecraft:small_dripleaf_block", "minecraft:big_dripleaf_stem": "minecraft:big_dripleaf", "minecraft:waxed_copper_block": "minecraft:waxed_copper",
    "minecraft:oak_button": "minecraft:wooden_button", "minecraft:oak_door": "minecraft:wooden_door", "minecraft:oak_pressure_plate": "minecraft:wooden_pressure_plate",
    "minecraft:oak_fence_gate": "minecraft:fence_gate", "minecraft:oak_trapdoor": "minecraft:trapdoor", "minecraft:shulker_box": "minecraft:undyed_shulker_box",
    "minecraft:beetroots": "minecraft:beetroot", "minecraft:cave_vines_plant": "minecraft:cave_vines", "minecraft:kelp_plant": "minecraft:kelp",
    "minecraft:twisting_vines_plant": "minecraft:twisting_vines", "minecraft:weeping_vines_plant": "minecraft:weeping_vines", "minecraft:moving_piston": "minecraft:moving_block",
    "minecraft:piston_head": "minecraft:piston_arm_collision", "minecraft:attached_melon_stem": "minecraft:melon_stem", "minecraft:attached_pumpkin_stem": "minecraft:pumpkin_stem",
    "minecraft:frogspawn": "minecraft:frog_spawn", "minecraft:lava_cauldron": "minecraft:lava_cauldron", "minecraft:water_cauldron": "minecraft:cauldron",
    "minecraft:powder_snow_cauldron": "minecraft:cauldron", "minecraft:stone_slab": "minecraft:normal_stone_slab", "minecraft:dead_bush": "minecraft:deadbush",
    "minecraft:wall_torch": "minecraft:torch", "minecraft:soul_wall_torch": "minecraft:soul_torch", "minecraft:redstone_wall_torch": "minecraft:redstone_torch",
    "minecraft:copper_wall_torch": "minecraft:copper_torch", "minecraft:tripwire": "minecraft:trip_wire", "minecraft:test_block": "minecraft:air", "minecraft:test_instance_block": "minecraft:air",
    "minecraft:nether_portal": "minecraft:portal", "minecraft:light": "minecraft:light_block_15",
}


def bedrock_name(jn, props, names):
    n = RENAME.get(jn, jn)
    if jn.endswith("_banner") and jn != "minecraft:banner":
        return "minecraft:standing_banner"
    if jn.endswith("_wall_banner"):
        return "minecraft:wall_banner"
    if jn.endswith("_bed"):
        return "minecraft:bed"
    if jn.startswith("minecraft:potted_"):
        return "minecraft:flower_pot"
    if jn.endswith("_wall_head") or jn.endswith("_wall_skull"):
        return jn.replace("_wall_", "_")
    if jn in ("minecraft:dark_oak_sign", "minecraft:dark_oak_wall_sign"):
        return jn.replace("dark_oak", "darkoak").replace("_sign", "_standing_sign") if jn.endswith("dark_oak_sign") else "minecraft:darkoak_wall_sign"
    if jn.endswith("cut_copper_slab") and props.get("type") == "double":
        return jn.replace("cut_copper_slab", "double_cut_copper_slab")
    if jn == "minecraft:stone_slab":
        return "minecraft:normal_stone_double_slab" if props.get("type") == "double" else "minecraft:normal_stone_slab"
    if jn == "minecraft:lava_cauldron":
        return "minecraft:cauldron"
    if jn.endswith("_wall_hanging_sign"):
        return jn.replace("_wall_hanging_sign", "_hanging_sign")
    if jn.endswith("_sign") and not jn.endswith("_hanging_sign") and not jn.endswith("_wall_sign"):
        base = jn[:-5]
        return ("minecraft:standing_sign" if base == "minecraft:oak" else base + "_standing_sign")
    if jn.endswith("_wall_sign"):
        base = jn[:-10]
        return "minecraft:wall_sign" if base == "minecraft:oak" else base + "_wall_sign"
    if jn.endswith("_slab") and props.get("type") == "double":
        return jn[:-5] + "_double_slab"
    if jn.endswith("_slab") and jn[:-5] + "_double_slab" not in names and jn == "minecraft:stone_slab":
        return "minecraft:normal_stone_slab"
    if jn == "minecraft:water":
        return "minecraft:water" if props.get("level", "0") == "0" else "minecraft:flowing_water"
    if jn == "minecraft:lava":
        return "minecraft:lava" if props.get("level", "0") == "0" else "minecraft:flowing_lava"
    if jn in ("minecraft:redstone_torch", "minecraft:redstone_wall_torch") and props.get("lit") == "false":
        return "minecraft:unlit_redstone_torch"
    if jn in ("minecraft:furnace", "minecraft:blast_furnace", "minecraft:smoker") and props.get("lit") == "true":
        return {"minecraft:furnace": "minecraft:lit_furnace", "minecraft:blast_furnace": "minecraft:lit_blast_furnace", "minecraft:smoker": "minecraft:lit_smoker"}[jn]
    if jn == "minecraft:redstone_lamp" and props.get("lit") == "true":
        return "minecraft:lit_redstone_lamp"
    if jn.startswith("minecraft:light") and False:
        return n
    return n


def desired_states(jn, props, bnames_states):
    """Bedrock state values implied by the Java properties; keys the Bedrock block does not have are dropped later."""
    d = {}
    P = props
    if "axis" in P: d["pillar_axis"] = P["axis"]
    if jn.endswith("_slab") or "type" in P and P.get("type") in ("top", "bottom", "double"):
        t = P.get("type")
        if t in ("top", "bottom", "double"):
            d["minecraft:vertical_half"] = "top" if t == "top" else "bottom"
    if "half" in P:
        h = P["half"]
        if h in ("upper", "lower"):
            d["upper_block_bit"] = 1 if h == "upper" else 0
        else:
            d["upside_down_bit"] = 1 if h == "top" else 0
    if "facing" in P:
        f = P["facing"]
        if "stairs" in jn: d["weirdo_direction"] = WEIRDO.get(f, 0)
        d["facing_direction"] = FACING.get(f, 0)
        d["minecraft:cardinal_direction"] = CARD.get(f, "north") if f in CARD else "north"
        if f in DIRECTION: d["direction"] = DIRECTION[f]
        if "torch" in jn:
            d["torch_facing_direction"] = TORCH_DIR.get(f, "unknown") if "wall" in jn else "top"
    elif "torch" in jn:
        d["torch_facing_direction"] = "top"
    for jk, bk in (("open", "open_bit"), ("powered", "powered_bit"), ("persistent", "persistent_bit"), ("waterlogged", None), ("occupied", "occupied_bit"),
                   ("inverted", "output_subtract_bit"), ("triggered", "triggered_bit"), ("attached", "attached_bit"), ("disarmed", "disarmed_bit"),
                   ("eye", "end_portal_eye_bit"), ("enabled", "toggle_bit"), ("extended", None), ("conditional", "conditional_bit"), ("hanging", "hanging"),
                   ("up", "wall_post_bit"), ("signal_fire", None), ("lit", "lit"), ("snowy", None), ("unstable", "explode_bit"), ("bottom", None),
                   ("has_bottle_0", "brewing_stand_slot_a_bit"), ("has_bottle_1", "brewing_stand_slot_b_bit"), ("has_bottle_2", "brewing_stand_slot_c_bit"),
                   ("has_record", None), ("has_book", None), ("short", None), ("natural", None), ("tip", None)):
        if jk in P and bk:
            d[bk] = 1 if P[jk] == "true" else 0
    if "hinge" in P: d["door_hinge_bit"] = 1 if P["hinge"] == "right" else 0
    if "part" in P: d["head_piece_bit"] = 1 if P["part"] == "head" else 0
    if "rotation" in P: d["ground_sign_direction"] = int(P["rotation"]); d["facing_direction"] = d.get("facing_direction", 0)
    if "layers" in P: d["height"] = int(P["layers"]) - 1
    if "age" in P:
        a = int(P["age"])
        d["age"] = a; d["growth"] = a; d["age_bit"] = 1 if a else 0
    if "stage" in P: d["age_bit"] = int(P["stage"])
    if jn == "minecraft:lava_cauldron": d["cauldron_liquid"] = "lava"; d["fill_level"] = 6
    if jn == "minecraft:powder_snow_cauldron": d["cauldron_liquid"] = "powder_snow"; d["fill_level"] = 2 * int(P.get("level", "3"))
    if jn == "minecraft:water_cauldron": d["cauldron_liquid"] = "water"; d["fill_level"] = 2 * int(P.get("level", "3"))
    elif "level" in P and jn in ("minecraft:water", "minecraft:lava"): d["liquid_depth"] = int(P["level"])
    elif "level" in P and "cauldron" not in jn: d["fill_level"] = int(P["level"])
    if "candles" in P: d["candles"] = int(P["candles"]) - 1
    if "moisture" in P: d["moisturized_amount"] = int(P["moisture"])
    if "bites" in P: d["bite_counter"] = int(P["bites"])
    if "honey_level" in P: d["honey_level"] = int(P["honey_level"])
    if "eggs" in P: d["turtle_egg_count"] = {"1": "one_egg", "2": "two_egg", "3": "three_egg", "4": "four_egg"}.get(P["eggs"], "one_egg")
    if "delay" in P: d["repeater_delay"] = int(P["delay"]) - 1
    if "power" in P: d["redstone_signal"] = int(P["power"])
    if "pickles" in P: d["cluster_count"] = int(P["pickles"]) - 1
    if "distance" in P and "leaves" in jn: d["update_bit"] = 0
    if "face" in P:
        f = P.get("facing", "north")
        d["facing_direction"] = {"floor": 1, "ceiling": 0}.get(P["face"], FACING.get(f, 2))
    if "shape" in P and ("rail" in jn):
        d["rail_direction"] = {"north_south": 0, "east_west": 1, "ascending_east": 2, "ascending_west": 3, "ascending_north": 4, "ascending_south": 5,
                               "south_east": 6, "south_west": 7, "north_west": 8, "north_east": 9}.get(P["shape"], 0)
    for side in ("north", "south", "east", "west"):
        if side in P and "wall" in jn:
            d["wall_connection_type_" + side] = {"none": "none", "low": "short", "tall": "tall", "false": "none", "true": "short"}.get(P[side], "none")
    if "instrument" in P: pass
    if "attachment" in P: pass
    return d


def pick(entries, want):
    best, score = None, -1
    for e in entries:
        s = 0
        for k, v in e["states"].items():
            if k in want and want[k] == v: s += 2
            elif k in want and str(want[k]) == str(v): s += 2
        # penalise states we wanted but the entry lacks a match for (already reflected); prefer entries whose unspecified states are default (0/false/first)
        s -= 0.001 * sum(1 for k, v in e["states"].items() if k not in want and v not in (0, "bottom", "none", "x", "y", "north", "normal", "unknown", "south"))
        if s > score: best, score = e, s
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--blocks", default="data/vanilla-26.3.blocks")
    ap.add_argument("--out", default="data")
    ap.add_argument("--palette", default="26_50")
    a = ap.parse_args()
    raw = gzip.decompress(get(PAL.format(a.palette)))
    root, _ = nbt_read(raw, 3, raw[0])
    pal = {}
    for e in root["blocks"]:
        pal.setdefault(e["name"], []).append(e)
    air = pal["minecraft:air"][0]["network_id"]
    rows = []
    for line in open(a.blocks):
        t = line.rstrip("\n").split("\t")
        props = dict(kv.split("=", 1) for kv in t[2].split(",")) if t[2] else {}
        rows.append((int(t[0]), t[1], props))
    n = max(r[0] for r in rows) + 1
    out = [air] * n
    missing = {}
    for sid, jn, props in rows:
        bn = bedrock_name(jn, props, pal)
        ents = pal.get(bn)
        if not ents:
            missing[jn] = bn
            out[sid] = pal["minecraft:stone"][0]["network_id"] if not any(w in jn for w in ("grass", "flower", "vine", "plant", "sapling", "torch", "carpet", "button", "sign", "banner", "rail")) else air
            continue
        want = desired_states(jn, props, None)
        e = pick(ents, want)
        out[sid] = e["network_id"]
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "blocks-bedrock.bin"), "wb") as f:
        f.write(b"PNBB" + struct.pack("<I", n) + struct.pack(f"<{n}i", *out))
    bio = json.loads(get(BIO))
    with open(os.path.join(a.out, "biomes-bedrock.txt"), "w") as f:
        for k, v in bio.items():
            f.write(f"{k} {v['bedrock_id']}\n")
    print(f"mapped {n} Java states ({len(rows)} rows); {len(missing)} Java blocks had no Bedrock counterpart:")
    print(" ".join(sorted(missing)))


if __name__ == "__main__":
    main()
