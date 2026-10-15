// Development helper (not part of the server): writes the per-state facts world generation asks of block states, one state per line:
//   name <TAB> properties (sorted, comma separated) <TAB> flags (hex)
// flag bits: 0 air, 1 blocksMotion, 2 canBeReplaced, 3 isSolid, 4 canOcclude, 5 full collision shape, 6 water in fluid state, 7 lava in fluid state,
//            8..13 sturdy face down/up/north/south/east/west, 14 liquid block, 15 solid render, 16 propagates skylight, 17 ignited by lava, 18 fluid source, 20..23 light emission, 24..29 collision shape has a full face down/up/north/south/east/west, 30 block has a dynamic shape
// A second file (ARGS[1]) lists the class hierarchy of every block: name <TAB> Class>Super>Super..., used to pick the survival rules of plants and attachments.
// usage: java -cp server.jar:libs/* tools/oracle/DumpBlocks.java OUTFILE [CLASSFILE]
import java.io.PrintWriter;
import java.util.TreeMap;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.material.Fluids;

public class DumpBlocks {
    // 26.3 dropped blocksMotion(); fall back to isSolid() there
    static java.lang.reflect.Method blocksMotion;
    static boolean blocksMotion(BlockState s) throws Exception {
        if (blocksMotion == null) {
            try { blocksMotion = BlockState.class.getMethod("blocksMotion"); } catch (NoSuchMethodException e) { blocksMotion = BlockState.class.getMethod("isSolid"); }
        }
        return (Boolean) blocksMotion.invoke(s);
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        Direction[] dirs = {Direction.DOWN, Direction.UP, Direction.NORTH, Direction.SOUTH, Direction.EAST, Direction.WEST};
        if (args.length > 1) {
            try (PrintWriter c = new PrintWriter(args[1])) {
                for (Block b : net.minecraft.core.registries.BuiltInRegistries.BLOCK) {
                    StringBuilder sb = new StringBuilder();
                    for (Class<?> k = b.getClass(); k != null && k != Object.class && k != Block.class; k = k.getSuperclass()) { if (sb.length() > 0) sb.append('>'); sb.append(k.getSimpleName()); }
                    c.println(net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(b) + "\t" + sb);
                }
            }
        }
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (BlockState s : Block.BLOCK_STATE_REGISTRY) {
                String name = net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(s.getBlock()).toString();
                TreeMap<String, String> props = new TreeMap<>();
                for (Property p : s.getProperties()) props.put(p.getName(), p.getName(s.getValue(p)));
                StringBuilder ps = new StringBuilder();
                for (var e : props.entrySet()) { if (ps.length() > 0) ps.append(','); ps.append(e.getKey()).append('=').append(e.getValue()); }
                long f = 0;
                if (s.isAir()) f |= 1L << 0;
                if (blocksMotion(s)) f |= 1L << 1;
                if (s.canBeReplaced()) f |= 1L << 2;
                if (s.isSolid()) f |= 1L << 3;
                if (s.canOcclude()) f |= 1L << 4;
                if (s.isCollisionShapeFullBlock(EmptyBlockGetter.INSTANCE, BlockPos.ZERO)) f |= 1L << 5;
                var fl = s.getFluidState();
                if (fl.is(Fluids.WATER) || fl.is(Fluids.FLOWING_WATER)) f |= 1L << 6;   // (tags are not bound outside a running server)
                if (fl.is(Fluids.LAVA) || fl.is(Fluids.FLOWING_LAVA)) f |= 1L << 7;
                for (int i = 0; i < 6; i++) if (s.isFaceSturdy(EmptyBlockGetter.INSTANCE, BlockPos.ZERO, dirs[i])) f |= 1L << (8 + i);
                if (s.liquid()) f |= 1L << 14;
                if (s.isSolidRender()) f |= 1L << 15;
                if (s.propagatesSkylightDown()) f |= 1L << 16;
                if (s.ignitedByLava()) f |= 1L << 17;
                if (!fl.isEmpty() && fl.isSource()) f |= 1L << 18;
                f |= ((long) s.getLightEmission()) << 20;
                if (s.getBlock().hasDynamicShape()) f |= 1L << 30;
                for (int i = 0; i < 6; i++) if (net.minecraft.world.level.block.Block.isFaceFull(s.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO), dirs[i])) f |= 1L << (24 + i);
                w.println(name + "\t" + ps + "\t" + Long.toHexString(f));
            }
        }
    }
}
