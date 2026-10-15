#!/usr/bin/env python3 -I
"""Development helper: translates nested classes of Mojang's decompiled structure pieces (CFR output) into C++ classes that build on
src/vg/legacy_piece.hpp (see tools/j2cpp.py for the statement level translation this builds on).

usage: j2cpp_class.py FILE.java OUTER CLASS[:CppBase] [CLASS ...] [--ids PieceTypeFile.java] [--prefix text]

Each CLASS is a nested class of OUTER (e.g. NetherFortressPieces). Output: C++ class definitions, in the order given, with the
constructors turned into initialiser lists, saved-data members (CompoundTag, addAdditionalSaveData) dropped and the Java method
signatures of postProcess / addChildren mapped to the fixed C++ ones. The output is a draft: compile it and fix what the compiler flags.
"""
import re
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
spec = importlib.util.spec_from_file_location("j2cpp", os.path.join(os.path.dirname(os.path.abspath(__file__)), "j2cpp.py"))
j2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(j2)

TYPES = [
    (r"\bboolean\b", "bool"), (r"\blong\b", "int64_t"), (r"\bBlockState\b", "BS"), (r"\bRandomSource\b", "JRandom"),
    (r"\bWorldGenLevel\b", "JLevel&"), (r"\bStructurePieceAccessor\b", "PieceAccessor&"),
]


def split_members(body):
    """Top level members of a class body: list of text chunks."""
    out = []
    depth = 0
    paren = 0
    cur = []
    i = 0
    in_str = False
    in_char = False
    in_line = False
    in_block = False
    while i < len(body):
        c = body[i]
        n = body[i + 1] if i + 1 < len(body) else ""
        if in_line:
            if c == "\n":
                in_line = False
            cur.append(c)
        elif in_block:
            if c == "*" and n == "/":
                in_block = False
                cur.append("*/")
                i += 1
            else:
                cur.append(c)
        elif in_str:
            cur.append(c)
            if c == "\\":
                cur.append(n)
                i += 1
            elif c == '"':
                in_str = False
        elif c == "/" and n == "/":
            in_line = True
            cur.append(c)
        elif c == "/" and n == "*":
            in_block = True
            cur.append("/*")
            i += 1
        elif c == '"':
            in_str = True
            cur.append(c)
        else:
            cur.append(c)
            if c == "(":
                paren += 1
            elif c == ")":
                paren -= 1
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0 and paren == 0:
                    out.append("".join(cur).strip())
                    cur = []
            elif c == ";" and depth == 0 and paren == 0:
                out.append("".join(cur).strip())
                cur = []
        i += 1
    rest = "".join(cur).strip()
    if rest:
        out.append(rest)
    return [m for m in out if m]


def strip_comments(m):
    m = re.sub(r"/\*.*?\*/", "", m, flags=re.S)
    m = re.sub(r"^\s*//.*$", "", m, flags=re.M)
    return m.strip()


def map_param_type(t):
    t = t.strip()
    t = re.sub(r"@Nullable\s+", "", t)
    for a, b in TYPES:
        t = re.sub(a, b, t)
    t = re.sub(r"\bStructurePiece\b", "StructurePiece*", t)
    return t


def map_params(params, cls):
    out = []
    for p in [x.strip() for x in re.split(r",(?![^<]*>)", params) if x.strip()]:
        p = re.sub(r"@Nullable\s+", "", p)
        m = re.match(r"(.+?)\s+(\w+)$", p)
        if not m:
            out.append(p)
            continue
        t, n = m.group(1).strip(), m.group(2)
        if t == "Direction":
            out.append("const Direction& " + n)
        elif t == "BlockPos":
            out.append("const BlockPos& " + n)
        elif t == "BoundingBox":
            out.append("const BoundingBox& " + ("bb" if n == "boundingBox" else n))
        elif t == "StructurePieceType":
            out.append("const char* " + n)
        elif t in ("StartPiece", "RoomDefinition"):
            out.append(t + "* " + n)
        elif t in ("int", "float", "double", "short", "byte"):
            out.append(t + " " + n)
        else:
            out.append(map_param_type(t) + " " + n)
    return ", ".join(out)


def class_pieces_ids(path):
    ids = {}
    if not path:
        return ids
    for m in re.finditer(r"StructurePieceType (\w+) = StructurePieceType\.set\w*PieceId\([^,]+, \"([^\"]+)\"\)", open(path).read()):
        ids[m.group(1)] = "minecraft:" + m.group(2).lower()
    return ids


def extra_rules(t):
    t = re.sub(r"\(StartPiece\)\s*", "", t)
    t = re.sub(r"\bBoundingBox\.orientBox\(", "orientBox(", t)
    t = re.sub(r"\bDirection (\w+) = getOrientation\(\);", r"auto \1 = getOrientation();", t)
    t = re.sub(r"switch \((orientation)\)", r"switch (\1->v)", t)
    t = re.sub(r"switch \((direction)\)", r"switch (\1.v)", t)
    t = re.sub(r"case (SOUTH|WEST|EAST|NORTH|UP|DOWN):", r"case pn::vg::\1:", t)
    t = re.sub(r"case Direction::(SOUTH|WEST|EAST|NORTH|UP|DOWN):", r"case pn::vg::\1:", t)
    t = re.sub(r"\bStructurePiece\.makeBoundingBox\(", "makeBoundingBox(", t)
    t = re.sub(r"\bStructurePiece\b", "StructurePiece", t)
    t = re.sub(r"\bnew (\w+)\(", r"new \1(", t)
    t = re.sub(r"(?<!::)\b([A-Z]\w+)\.([a-z]\w*)\(", r"\1::\2(", t)
    t = t.replace("pos.set(", "pos = BlockPos(") if False else t
    return t


def translate_class(src, outer, spec, ids, indent="    "):
    name, _, cppbase = spec.partition(":")
    a, b = j2.class_region(src, outer)
    region = src[a:b]
    m = re.search(r"(?:public |private |protected )?(?:static )?(abstract )?class\s+" + re.escape(name) + r"\b\s*(?:extends\s+(\w+))?[^{]*\{", region)
    if not m:
        return "// class %s not found\n" % name
    start = m.end() - 1
    end = j2.find_block(region, start)
    jbase = m.group(2) or "StructurePiece"
    base = cppbase or jbase
    if base == "StructurePiece":
        base = "LegacyPiece"
    body = region[start + 1:end - 1]
    out = ["class %s : public %s {\npublic:" % (name, base)]
    ctors = []
    for member in split_members(body):
        mem = strip_comments(member)
        if not mem:
            continue
        mem = re.sub(r"^@\w+(\([^)]*\))?\s*", "", mem, flags=re.M).strip()
        # nested classes are skipped
        if re.match(r"(?:public |private |protected )?(?:static )?(?:abstract )?(?:enum|class|record|interface)\b", mem):
            continue
        # fields
        fm = re.match(r"^((?:(?:public|private|protected|static|final|volatile)\s+)*)([\w<>\[\], .?@]+?)\s+(\w+)(\s*=\s*[^;]+)?;$", mem, re.S)
        if fm and "(" not in fm.group(2):
            mods, typ, fname, init = fm.group(1) or "", fm.group(2).strip(), fm.group(3), fm.group(4) or ""
            if "static" in mods and "final" in mods:
                if typ in ("int", "float", "double", "boolean", "long") and init:
                    out.append(indent + "static constexpr %s %s%s;" % (map_param_type(typ), fname, init))
                else:
                    out.append(indent + "// static %s %s%s;" % (typ, fname, init))
                continue
            if "static" in mods:
                out.append(indent + "// static %s %s;" % (typ, fname))
                continue
            typ2 = typ
            if typ == "RoomDefinition":
                typ2 = "RoomDefinition*"
            elif typ == "Direction":
                typ2 = "Direction"
            elif typ == "boolean":
                typ2 = "bool"
            else:
                typ2 = map_param_type(typ)
            out.append(indent + "%s %s%s;" % (typ2, fname, extra_rules(j2.translate(init)) if init else ""))
            continue
        # methods / constructors
        mm = re.match(r"^((?:(?:public|private|protected|static|final|abstract|synchronized)\s+)*)(?:(@Nullable\s+)?([\w<>\[\],.? ]+?)\s+)?(\w+)\s*\(([^)]*)\)\s*(?:throws [\w, .]+)?\s*\{", mem, re.S)
        if not mm:
            continue
        mods, ret, mname, params = mm.group(1) or "", mm.group(3), mm.group(4), mm.group(5)
        bstart = mem.index("{", mm.end() - 1)
        mbody = mem[bstart + 1:j2.find_block(mem, bstart) - 1]
        if mname == name and ret is None:
            if "CompoundTag" in params:
                continue
            # constructor: super(...) becomes the initialiser list
            sup = re.match(r"\s*super\((.*?)\);\s*(.*)$", mbody, re.S)
            init_list = ""
            rest = mbody
            if sup:
                args = sup.group(1)
                rest = sup.group(2)
                # the first argument is a StructurePieceType constant
                am = re.match(r"\s*StructurePieceType\.(\w+)\s*(?:,\s*(.*))?$", args, re.S)
                if am:
                    pid = ids.get(am.group(1), "minecraft:" + am.group(1).lower())
                    rest_args = am.group(2)
                    init_list = " : %s(\"%s\"%s)" % (base, pid, (", " + extra_rules(j2.translate(rest_args))) if rest_args else "")
                else:
                    init_list = " : %s(%s)" % (base, extra_rules(j2.translate(args)))
            ctors.append((params, init_list, extra_rules(j2.translate(rest))))
            out.append(indent + "%s(%s)%s {%s}" % (name, map_params(params, name), init_list, extra_rules(j2.translate(rest)).rstrip()))
            continue
        if mname in ("addAdditionalSaveData", "getLocatorPosition"):
            continue
        tbody = extra_rules(j2.translate(mbody)).rstrip()
        static = "static " if "static" in mods else ""
        if mname == "postProcess":
            out.append(indent + "void postProcess(JLevel& level, JRandom random, const BoundingBox& chunkBB, const ChunkPos& chunkPos, const BlockPos& referencePos) override {%s\n%s}" % (tbody, indent))
        elif mname == "addChildren":
            out.append(indent + "void addChildren(StructurePiece* startPiece, PieceAccessor& structurePieceAccessor, JRandom random) override {%s\n%s}" % (tbody, indent))
        else:
            r = ret or "void"
            rptr = bool(mm.group(2))
            r = map_param_type(r)
            if rptr and r not in ("int", "bool", "float", "double"):
                r = r + "*"
            out.append(indent + "%s%s %s(%s) {%s\n%s}" % (static, r, mname, map_params(params, name), tbody, indent))
    out.append("};\n")
    return "\n".join(out)


def main():
    args = sys.argv[1:]
    if len(args) < 3:
        print(__doc__)
        return 2
    ids_file = None
    if "--ids" in args:
        i = args.index("--ids")
        ids_file = args[i + 1]
        del args[i:i + 2]
    src = open(args[0], encoding="utf-8").read()
    outer = args[1]
    ids = class_pieces_ids(ids_file)
    for spec in args[2:]:
        print(translate_class(src, outer, spec, ids))
    return 0


if __name__ == "__main__":
    sys.exit(main())
