// Development helper: Mojang's decoration random for one chunk and feature, for comparing with Painite's Xoroshiro port.
// usage: java -cp server.jar:libs/* tools/oracle/RngCheck.java SEED CHUNKX CHUNKZ INDEX STEP
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.XoroshiroRandomSource;

public class RngCheck {
    public static void main(String[] a) {
        long seed = Long.parseLong(a[0]);
        int cx = Integer.parseInt(a[1]), cz = Integer.parseInt(a[2]), index = Integer.parseInt(a[3]), step = Integer.parseInt(a[4]);
        WorldgenRandom random = new WorldgenRandom(new XoroshiroRandomSource(0L));
        long deco = random.setDecorationSeed(seed, cx * 16, cz * 16);
        System.out.println("decoration seed " + deco);
        random.setFeatureSeed(deco, index, step);
        StringBuilder sb = new StringBuilder("draws:");
        for (int i = 0; i < 4; i++) sb.append(' ').append(random.nextInt(16));
        sb.append(" | ").append(random.nextInt(65)).append(' ').append(random.nextFloat()).append(' ').append(random.nextDouble()).append(' ').append(random.nextLong());
        System.out.println(sb);
    }
}
