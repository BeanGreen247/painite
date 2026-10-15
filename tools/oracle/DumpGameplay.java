// Development helper (not part of the server): writes block and entity facts gameplay needs, straight from Mojang's classes.
//   blockprops (ARGS[0]), one block per line:  name <TAB> destroyTime <TAB> explosionResistance <TAB> requiresTool(0/1) <TAB> friction <TAB> speedFactor <TAB> jumpFactor <TAB> randomTicks(0/1) <TAB> pushReaction <TAB> hasBlockEntity(0/1) <TAB> soundType
//   entityprops (ARGS[1]), one entity type per line: name <TAB> category <TAB> width <TAB> height <TAB> eyeHeight <TAB> fireImmune <TAB> clientTrackingRange <TAB> updateInterval <TAB> attr=value,...  (default attributes of living entities)
// usage: java -cp server.jar:libs/* tools/oracle/DumpGameplay.java BLOCKFILE ENTITYFILE
import java.io.PrintWriter;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.ai.attributes.AttributeInstance;
import net.minecraft.world.entity.ai.attributes.AttributeSupplier;
import net.minecraft.world.entity.ai.attributes.DefaultAttributes;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;

public class DumpGameplay {
    @SuppressWarnings({"rawtypes", "unchecked"})
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (Block b : BuiltInRegistries.BLOCK) {
                BlockState s = b.defaultBlockState();
                String sound = "";
                try { var f = Block.class.getMethod("getSoundType", BlockState.class); Object st = f.invoke(b, s); sound = String.valueOf(st); } catch (Throwable t) { }
                w.println(BuiltInRegistries.BLOCK.getKey(b) + "\t" + s.getDestroySpeed(EmptyBlockGetter.INSTANCE, BlockPos.ZERO) + "\t" + b.getExplosionResistance() + "\t" + (s.requiresCorrectToolForDrops() ? 1 : 0)
                        + "\t" + b.getFriction() + "\t" + b.getSpeedFactor() + "\t" + b.getJumpFactor() + "\t" + (s.isRandomlyTicking() ? 1 : 0) + "\t" + s.getPistonPushReaction() + "\t" + (s.hasBlockEntity() ? 1 : 0));
            }
        }
        try (PrintWriter w = new PrintWriter(args[1])) {
            for (EntityType<?> t : BuiltInRegistries.ENTITY_TYPE) {
                StringBuilder at = new StringBuilder();
                try {
                    AttributeSupplier sup = DefaultAttributes.getSupplier((EntityType) t);
                    if (sup != null) {
                        for (var h : BuiltInRegistries.ATTRIBUTE.listElements().toList()) {
                            if (sup.hasAttribute(h)) { if (at.length() > 0) at.append(','); at.append(h.key().identifier()).append('=').append(sup.getBaseValue(h)); }
                        }
                    }
                } catch (Throwable e) { }
                var d = t.getDimensions();
                w.println(BuiltInRegistries.ENTITY_TYPE.getKey(t) + "\t" + t.getCategory().getName() + "\t" + d.width() + "\t" + d.height() + "\t" + d.eyeHeight() + "\t" + (t.fireImmune() ? 1 : 0) + "\t" + t.clientTrackingRange() + "\t" + t.updateInterval() + "\t" + at);
            }
        }
    }
}
