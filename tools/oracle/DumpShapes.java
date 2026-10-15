// Development helper (not part of the server): collision boxes of every block state, from Mojang's own shapes.
//   ARGS[0]: one state per line: name <TAB> properties (sorted, comma separated) <TAB> shape id
//   ARGS[1]: one shape per line: id <TAB> x0 y0 z0 x1 y1 z1 ; x0 ... (boxes of the unit cube, empty for no collision)
// usage: java -cp server.jar:libs/* tools/oracle/DumpShapes.java STATES SHAPES
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.phys.AABB;

public class DumpShapes {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        Map<String, Integer> ids = new LinkedHashMap<>();
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (BlockState s : Block.BLOCK_STATE_REGISTRY) {
                String name = BuiltInRegistries.BLOCK.getKey(s.getBlock()).toString();
                TreeMap<String, String> props = new TreeMap<>();
                for (Property p : s.getProperties()) props.put(p.getName(), p.getName(s.getValue(p)));
                StringBuilder ps = new StringBuilder();
                for (var e : props.entrySet()) { if (ps.length() > 0) ps.append(','); ps.append(e.getKey()).append('=').append(e.getValue()); }
                StringBuilder sb = new StringBuilder();
                for (AABB b : s.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO).toAabbs()) {
                    if (sb.length() > 0) sb.append(" ; ");
                    sb.append(b.minX).append(' ').append(b.minY).append(' ').append(b.minZ).append(' ').append(b.maxX).append(' ').append(b.maxY).append(' ').append(b.maxZ);
                }
                int id = ids.computeIfAbsent(sb.toString(), k -> ids.size());
                w.println(name + "\t" + ps + "\t" + id);
            }
        }
        try (PrintWriter w = new PrintWriter(args[1])) {
            for (var e : ids.entrySet()) w.println(e.getValue() + "\t" + e.getKey());
        }
    }
}
