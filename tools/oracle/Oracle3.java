// Development helper (not part of the server): answers questions about Mojang's 26.3 world generation so Painite's port can be checked bit by bit.
// usage: java -cp inner-26.3.jar:libs/* tools/oracle/Oracle3.java SEED SETTINGS < queries     (SETTINGS: overworld, nether, end, amplified, large_biomes, caves, floating_islands)
// queries, one per line (floats are printed as their raw bits in hex, then as decimal):
//   df NAME X Y Z                  a noise router function (temperature vegetation continents erosion depth ridges chunk_surface_level final_density) at one block, uncached
//   vol NAME X Y Z SX SY SZ        the same over a volume (x fastest-varying outermost: index = y + (x + z * SX) * SY), printed as bits
//   noise NAME X Y Z               a named noise (temperature, continentalness, nether/...) at a point; NAME without the namespace
//   noisevol NAME X Y Z SX SY SZ XZSCALE YSCALE AMPLITUDE   Noise.addToVolume into a zeroed buffer, printed as bits
//   climate X Y Z                  the six quantised climate values at a block (quart cell) and the biome
import java.io.BufferedReader;
import java.io.InputStreamReader;
import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.QuartPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseRouter;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensitySampler;
import net.minecraft.world.level.levelgen.densityfunction.DensitySamplerSet;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.levelgen.densityfunction.ScopedDensityBuffer;

public class Oracle3 {
    static DensityFunction pick(NoiseRouter r, String name) {
        return switch (name) {
            case "temperature" -> r.temperature();
            case "vegetation" -> r.vegetation();
            case "continents" -> r.continents();
            case "erosion" -> r.erosion();
            case "depth" -> r.depth();
            case "ridges" -> r.ridges();
            case "chunk_surface_level" -> r.chunkSurfaceLevel();
            case "final_density" -> r.finalDensity();
            default -> throw new IllegalArgumentException(name);
        };
    }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        long seed = Long.parseLong(args[0]);
        String settingsName = args.length > 1 ? args[1] : "overworld";
        HolderLookup.Provider provider = VanillaRegistries.createWorldLookup();
        ResourceKey<NoiseGeneratorSettings> key = ResourceKey.create(Registries.NOISE_SETTINGS, Identifier.withDefaultNamespace(settingsName));
        Holder<NoiseGeneratorSettings> settings = provider.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(key);
        RandomState state = RandomState.create(provider.lookupOrThrow(Registries.NOISE), seed, settings.value());
        NoiseRouter router = settings.value().noiseRouter();
        SamplerContext ctx = SamplerContext.builder().enableCaches().build();
        DensitySamplerSet samplers = state.samplersWithContext(ctx);
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        String line;
        while ((line = in.readLine()) != null) {
            String[] t = line.trim().split("\\s+");
            if (t.length == 0 || t[0].isEmpty()) continue;
            switch (t[0]) {
                case "df" -> {
                    float v = state.sampleBlockValueUncached(pick(router, t[1]), Integer.parseInt(t[2]), Integer.parseInt(t[3]), Integer.parseInt(t[4]));
                    System.out.println(String.format("df %s %s %s %s = %08x %s", t[1], t[2], t[3], t[4], Float.floatToRawIntBits(v), v));
                }
                case "vol" -> {
                    DensityVolume vol = new DensityVolume(Integer.parseInt(t[5]), Integer.parseInt(t[6]), Integer.parseInt(t[7]), Integer.parseInt(t[2]), Integer.parseInt(t[3]), Integer.parseInt(t[4]));
                    DensitySampler.Bound b = samplers.get(pick(router, t[1]));
                    try (ScopedDensityBuffer buf = b.sampleVolume(vol)) {
                        StringBuilder sb = new StringBuilder("vol " + t[1]);
                        for (int i = 0; i < vol.size(); i++) sb.append(String.format(" %08x", Float.floatToRawIntBits(buf.get(i))));
                        System.out.println(sb);
                    }
                }
                case "noise" -> {
                    net.minecraft.world.level.levelgen.synth.Noise n = state.getOrCreateNoise(ResourceKey.create(Registries.NOISE, Identifier.withDefaultNamespace(t[1])));
                    float v = n.get(Double.parseDouble(t[2]), Double.parseDouble(t[3]), Double.parseDouble(t[4]));
                    System.out.println(String.format("noise %s %s %s %s = %08x %s", t[1], t[2], t[3], t[4], Float.floatToRawIntBits(v), v));
                }
                case "noisevol" -> {
                    net.minecraft.world.level.levelgen.synth.Noise n = state.getOrCreateNoise(ResourceKey.create(Registries.NOISE, Identifier.withDefaultNamespace(t[1])));
                    DensityVolume vol = new DensityVolume(Integer.parseInt(t[5]), Integer.parseInt(t[6]), Integer.parseInt(t[7]), Integer.parseInt(t[2]), Integer.parseInt(t[3]), Integer.parseInt(t[4]));
                    try (ScopedDensityBuffer buf = ctx.acquireBuffer(vol)) {
                        buf.fill(0.0f);
                        n.addToVolume(buf, vol, Double.parseDouble(t[8]), Double.parseDouble(t[9]), Float.parseFloat(t[10]));
                        StringBuilder sb = new StringBuilder("noisevol " + t[1]);
                        for (int i = 0; i < vol.size(); i++) sb.append(String.format(" %08x", Float.floatToRawIntBits(buf.get(i))));
                        System.out.println(sb);
                    }
                }
                case "climate" -> {
                    int x = Integer.parseInt(t[1]), y = Integer.parseInt(t[2]), z = Integer.parseInt(t[3]);
                    Climate.Sampler sampler = state.createClimateSampler(ctx);
                    Climate.TargetPoint p = sampler.sample(QuartPos.fromBlock(x), QuartPos.fromBlock(y), QuartPos.fromBlock(z));
                    System.out.println("climate " + p.temperature() + " " + p.humidity() + " " + p.continentalness() + " " + p.erosion() + " " + p.depth() + " " + p.weirdness());
                }
                default -> System.out.println("? " + line);
            }
        }
    }
}
