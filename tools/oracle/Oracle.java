// Development helper (not part of the server): answers questions about Mojang's own world generation so Painite's port can be checked value by value.
// usage: java -cp server.jar:libs/* tools/oracle/Oracle.java SEED SETTINGS < queries     (SETTINGS: overworld, nether, end, amplified, large_biomes, caves, floating_islands)
// queries, one per line:
//   climate X Y Z          the six climate values (quantised, at the quart cell containing the block) and the biome Mojang picks
//   df NAME X Y Z          a noise router function evaluated directly at a block (no cell interpolation): continents erosion temperature vegetation depth ridges
//                          preliminary_surface_level final_density barrier fluid_level_floodedness fluid_level_spread lava vein_toggle vein_ridged vein_gap
//   noise NAME X Y Z       a named noise (minecraft:temperature ...) at a point
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.Map;
import net.minecraft.SharedConstants;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.QuartPos;
import net.minecraft.core.registries.Registries;
import net.minecraft.data.worldgen.biome.*;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.levelgen.DensityFunction;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseRouter;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.synth.NormalNoise;

public class Oracle {
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        long seed = Long.parseLong(args[0]);
        String settingsName = args.length > 1 ? args[1] : "overworld";
        HolderLookup.Provider provider = VanillaRegistries.createLookup();
        ResourceKey<NoiseGeneratorSettings> key = ResourceKey.create(Registries.NOISE_SETTINGS, Identifier.withDefaultNamespace(settingsName));
        RandomState state = RandomState.create(provider, key, seed);
        NoiseRouter router = state.router();
        Climate.ParameterList<ResourceKey<Biome>> list = null;
        for (Map.Entry<MultiNoiseBiomeSourceParameterList.Preset, Climate.ParameterList<ResourceKey<Biome>>> e : MultiNoiseBiomeSourceParameterList.knownPresets().entrySet())
            if (e.getKey().id().getPath().equals(settingsName.equals("nether") ? "nether" : "overworld")) list = e.getValue();
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        String line;
        while ((line = in.readLine()) != null) {
            String[] t = line.trim().split("\\s+");
            if (t.length == 0 || t[0].isEmpty()) continue;
            switch (t[0]) {
                case "climate" -> {
                    int x = Integer.parseInt(t[1]), y = Integer.parseInt(t[2]), z = Integer.parseInt(t[3]);
                    Climate.TargetPoint p = state.sampler().sample(QuartPos.fromBlock(x), QuartPos.fromBlock(y), QuartPos.fromBlock(z));
                    String biome = list == null ? "?" : list.findValue(p).identifier().getPath();
                    System.out.println(x + " " + y + " " + z + " " + p.temperature() + " " + p.humidity() + " " + p.continentalness() + " " + p.erosion() + " " + p.depth() + " " + p.weirdness() + " " + biome);
                }
                case "df" -> {
                    String name = t[1];
                    int x = Integer.parseInt(t[2]), y = Integer.parseInt(t[3]), z = Integer.parseInt(t[4]);
                    DensityFunction f = switch (name) {
                        case "continents" -> router.continents();
                        case "erosion" -> router.erosion();
                        case "temperature" -> router.temperature();
                        case "vegetation" -> router.vegetation();
                        case "depth" -> router.depth();
                        case "ridges" -> router.ridges();
                        case "preliminary_surface_level" -> router.preliminarySurfaceLevel();
                        case "final_density" -> router.finalDensity();
                        case "barrier" -> router.barrierNoise();
                        case "fluid_level_floodedness" -> router.fluidLevelFloodednessNoise();
                        case "fluid_level_spread" -> router.fluidLevelSpreadNoise();
                        case "lava" -> router.lavaNoise();
                        case "vein_toggle" -> router.veinToggle();
                        case "vein_ridged" -> router.veinRidged();
                        case "vein_gap" -> router.veinGap();
                        default -> throw new IllegalArgumentException(name);
                    };
                    System.out.println(name + " " + x + " " + y + " " + z + " " + Double.toString(f.compute(new DensityFunction.SinglePointContext(x, y, z))));
                }
                case "noise" -> {
                    ResourceKey<NormalNoise.NoiseParameters> nk = ResourceKey.create(Registries.NOISE, Identifier.parse(t[1]));
                    NormalNoise n = state.getOrCreateNoise(nk);
                    double x = Double.parseDouble(t[2]), y = Double.parseDouble(t[3]), z = Double.parseDouble(t[4]);
                    System.out.println(t[1] + " " + x + " " + y + " " + z + " " + Double.toString(n.getValue(x, y, z)));
                }
                default -> System.out.println("?");
            }
            System.out.flush();
        }
    }
}
