// Development helper (not part of the server): for every block state, in the same order as DumpBlocks writes them, the state ids that Mojang's
// BlockState.rotate (90, 180, 270 degrees clockwise) and BlockState.mirror (left-right, front-back) give. Structure templates are placed through these.
// usage: java -cp server.jar:libs/* tools/oracle/DumpTransforms.java OUTFILE        (one line per state: r90 r180 r270 mirrorLR mirrorFB, state ordinals)
import java.io.PrintWriter;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Mirror;
import net.minecraft.world.level.block.Rotation;
import net.minecraft.world.level.block.state.BlockState;

public class DumpTransforms {
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (BlockState s : Block.BLOCK_STATE_REGISTRY) {
                StringBuilder sb = new StringBuilder();
                for (Rotation r : new Rotation[]{Rotation.CLOCKWISE_90, Rotation.CLOCKWISE_180, Rotation.COUNTERCLOCKWISE_90})
                    sb.append(Block.BLOCK_STATE_REGISTRY.getId(s.rotate(r))).append(' ');
                for (Mirror m : new Mirror[]{Mirror.LEFT_RIGHT, Mirror.FRONT_BACK})
                    sb.append(Block.BLOCK_STATE_REGISTRY.getId(s.mirror(m))).append(' ');
                w.println(sb.toString().trim());
            }
        }
    }
}
