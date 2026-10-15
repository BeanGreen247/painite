// Development helper (not part of the server): which block entity type each block has, and whether that type is a RandomizableContainer
// (structure templates draw a loot table seed for those). usage: java -cp server.jar:libs/* tools/oracle/DumpBlockEntities.java OUTFILE
// line: block name <TAB> block entity type id <TAB> 1 when randomizable else 0
import java.io.PrintWriter;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.RandomizableContainer;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.entity.BlockEntityType;
import net.minecraft.world.level.block.state.BlockState;

public class DumpBlockEntities {
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (Block b : BuiltInRegistries.BLOCK) {
                BlockState s = b.defaultBlockState();
                for (BlockEntityType<?> t : BuiltInRegistries.BLOCK_ENTITY_TYPE) {
                    if (!t.isValid(s)) continue;
                    int randomizable = 0;
                    try { randomizable = t.create(BlockPos.ZERO, s) instanceof RandomizableContainer ? 1 : 0; } catch (Throwable e) { /* needs bound registries (vault): not a loot container */ }
                    w.println(BuiltInRegistries.BLOCK.getKey(b) + "\t" + BuiltInRegistries.BLOCK_ENTITY_TYPE.getKey(t) + "\t" + randomizable);
                    break;
                }
            }
        }
    }
}
