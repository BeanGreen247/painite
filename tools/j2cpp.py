#!/usr/bin/env python3 -I
"""Development helper: turns the body of a method of Mojang's decompiled Java (CFR output) into C++ close to what src/vg/jcompat.hpp and
legacy_piece.hpp accept, for the structure pieces that are long runs of generateBox / placeBlock calls. The result is a starting point: read it,
compile it, fix what the compiler flags.

usage: j2cpp.py FILE.java [CLASS] METHOD [METHOD ...]      prints each method as C++ (the signature is kept as a comment)
       j2cpp.py FILE.java --whole CLASS                   prints every member of the class body, translated line by line
"""
import re
import sys

ENUM_VALUES = r"(?:StairsShape|RedstoneSide|SlabType|DoubleBlockHalf|RailShape|AttachFace|Half|WallSide|ChestType|DoorHingeSide|BedPart|Direction\.Axis)"


def find_block(src, start):
    """src[start] is '{': the index just after the matching '}'."""
    depth = 0
    i = start
    in_str = False
    while i < len(src):
        c = src[i]
        if in_str:
            if c == '\\':
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError("unbalanced braces")


def class_region(src, cls):
    if cls is None:
        return 0, len(src)
    m = re.search(r"\bclass\s+" + re.escape(cls) + r"\b[^{]*\{", src)
    if not m:
        raise SystemExit("class %s not found" % cls)
    end = find_block(src, m.end() - 1)
    return m.end(), end


def extract_methods(src, cls, name):
    a, b = class_region(src, cls)
    out = []
    for m in re.finditer(r"^[ \t]*(?:@\w+\s*)*((?:public|protected|private|static|final|abstract|synchronized|\s)*[\w<>\[\], .?]+?)\s+" + re.escape(name) + r"\s*\(([^)]*)\)\s*(?:throws [\w, .]+)?\s*\{", src[a:b], re.M):
        s = a + m.end() - 1
        e = find_block(src, s)
        out.append((m.group(1).strip() + " " + name + "(" + m.group(2).strip() + ")", src[s + 1:e - 1]))
    return out


def snake(s):
    return s.lower()


def translate(body):
    t = body
    t = re.sub(r"RandomSource\.createThreadLocalInstance\(([^()]*(?:\([^()]*\))?[^()]*)\)\.forkPositional\(\)\.at\(", r"positionalRandom(\1, ", t)
    t = re.sub(r"\bfinal\s+", "", t)
    t = re.sub(r"@Override\s*", "", t)
    t = re.sub(r"\bthis\.", "", t)
    # block states
    t = re.sub(r"Blocks\.DYED_TERRACOTTA\.(\w+)\(\)\.defaultBlockState\(\)", lambda m: 'blk("minecraft:%s_terracotta")' % m.group(1), t)
    t = re.sub(r"Blocks\.DYED_TERRACOTTA\.(\w+)\(\)", lambda m: 'BlockRef("minecraft:%s_terracotta")' % m.group(1), t)
    t = re.sub(r"Blocks\.([A-Z0-9_]+)\.defaultBlockState\(\)", lambda m: 'blk("minecraft:%s")' % snake(m.group(1)), t)
    t = re.sub(r"Blocks\.([A-Z0-9_]+)\.getBlock\(\)", lambda m: 'BlockRef("minecraft:%s")' % snake(m.group(1)), t)
    t = re.sub(r"\(BlockState\)\s*", "", t)
    t = re.sub(r"Blocks\.([A-Z0-9_]+)", lambda m: 'BlockRef("minecraft:%s")' % snake(m.group(1)), t)
    t = re.sub(r"\bBiomeTags\.([A-Z0-9_]+)", lambda m: 'Tag("#minecraft:%s")' % snake(m.group(1)), t)
    t = re.sub(r"\bboundingBox\b(?!\()", "bb", t)
    t = re.sub(r"BlockTags\.([A-Z0-9_]+)", lambda m: 'Tag("#minecraft:%s")' % snake(m.group(1)), t)
    t = re.sub(r"BuiltInLootTables\.([A-Z0-9_]+)", lambda m: '"minecraft:chests/%s"' % snake(m.group(1)), t)
    # properties and enum values
    t = re.sub(r"\.setValue\(\s*[A-Za-z]+\.([A-Z0-9_]+)\s*,", lambda m: '.setValue("%s",' % snake(m.group(1)), t)
    t = re.sub(r"\b" + ENUM_VALUES + r"\.([A-Z0-9_]+)\b", lambda m: '"%s"' % snake(m.group(1)), t)
    # directions, rotations
    t = re.sub(r"Direction\.Plane\.HORIZONTAL", "Direction::HORIZONTAL", t)
    t = re.sub(r"\bDirection\.([A-Z]+)\b", r"Direction::\1", t)
    t = re.sub(r"\b(Rotation|Mirror)\.([A-Z0-9_]+)\b", r"\1::\2", t)
    # types
    t = re.sub(r"\bBlockState\b", "BS", t)
    t = re.sub(r"\bboolean\b", "bool", t)
    t = re.sub(r"\blong\b", "int64_t", t)
    t = re.sub(r"\bRandomSource\b", "JRandom", t)
    t = re.sub(r"\b(?:WorldGenLevel|ServerLevelAccessor|LevelAccessor|LevelReader|BlockGetter)\b", "JLevel&", t)
    t = re.sub(r"BlockPos\.MutableBlockPos", "BlockPos", t)
    t = re.sub(r"\bResourceKey<LootTable>", "std::string", t)
    t = re.sub(r"\bnew BlockPos\(", "BlockPos(", t)
    t = re.sub(r"\bnew BoundingBox\(", "BoundingBox(", t)
    # accessors
    for j, c in (("minX", "x0"), ("minY", "y0"), ("minZ", "z0"), ("maxX", "x1"), ("maxY", "y1"), ("maxZ", "z1")):
        t = t.replace("." + j + "()", "." + c)
    t = t.replace(".getXSpan()", ".x_span()").replace(".getYSpan()", ".y_span()").replace(".getZSpan()", ".z_span()")
    t = t.replace(".getCenter()", ".center()").replace("chunkBB.isInside(", "chunkBB.inside(").replace(".isInside(", ".inside(")
    t = re.sub(r"\.get([XYZ])\(\)", lambda m: "." + m.group(1).lower(), t)
    t = t.replace("level.getRandom()", "levelRandom(level)")
    t = t.replace("Math.max(", "std::max(").replace("Math.min(", "std::min(").replace("Math.abs(", "std::abs(")
    t = t.replace("Mth.floor(", "int(std::floor(")
    # `for (Direction d : Direction::HORIZONTAL)` is valid C++ as is; null
    t = re.sub(r"\bnull\b", "nullptr", t)
    t = re.sub(r"\bMth\.randomBetweenInclusive\((\w+), ", r"\1.nextIntBetweenInclusive(", t)
    return t


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src = open(sys.argv[1], encoding="utf-8").read()
    args = sys.argv[2:]
    cls = None
    if args[0] == "--whole":
        a, b = class_region(src, args[1])
        print(translate(src[a:b]))
        return 0
    if args[0][0].isupper() and len(args) > 1 and not args[0].startswith("get"):
        cls = args[0]
        args = args[1:]
    for name in args:
        for sig, body in extract_methods(src, cls, name):
            print("// %s" % sig)
            print("{")
            print(translate(body).rstrip())
            print("}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
