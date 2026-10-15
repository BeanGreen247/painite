// Development helper: the feature order Mojang's FeatureSorter builds for a dimension's biome list, one "step index placed_feature" line per feature.
// usage: java -cp <cp-VERSION.txt> tools/oracle/DumpFeatureOrder.java overworld|nether|end
import java.util.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderLookup;
import net.minecraft.core.registries.Registries;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.FeatureSorter;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.levelgen.placement.PlacedFeature;
import net.minecraft.resources.ResourceKey;

public class DumpFeatureOrder {
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        HolderLookup.Provider provider = VanillaRegistries.createLookup();
        HolderLookup.RegistryLookup<Biome> biomes = provider.lookupOrThrow(Registries.BIOME);
        HolderLookup.RegistryLookup<PlacedFeature> placed = provider.lookupOrThrow(Registries.PLACED_FEATURE);
        Map<PlacedFeature, String> names = new IdentityHashMap<>();
        placed.listElements().forEach(h -> names.put(h.value(), h.key().identifier().getPath()));
        List<Holder<Biome>> sources = new ArrayList<>();
        String dim = args.length > 0 ? args[0] : "overworld";
        if (dim.equals("end")) {
            for (String b : new String[]{"the_end", "end_highlands", "end_midlands", "small_end_islands", "end_barrens"})
                sources.add(biomes.getOrThrow(ResourceKey.create(Registries.BIOME, net.minecraft.resources.Identifier.withDefaultNamespace(b))));
        } else {
            for (Map.Entry<MultiNoiseBiomeSourceParameterList.Preset, Climate.ParameterList<ResourceKey<Biome>>> e : MultiNoiseBiomeSourceParameterList.knownPresets().entrySet()) {
                if (!e.getKey().id().getPath().equals(dim)) continue;
                LinkedHashSet<ResourceKey<Biome>> seen = new LinkedHashSet<>();
                for (var p : e.getValue().values()) seen.add(p.getSecond());
                for (ResourceKey<Biome> k : seen) sources.add(biomes.getOrThrow(k));
            }
        }
        List<FeatureSorter.StepFeatureData> steps = FeatureSorter.buildFeaturesPerStep(sources, b -> b.value().getGenerationSettings().features(), true);
        for (int s = 0; s < steps.size(); s++) {
            List<PlacedFeature> f = steps.get(s).features();
            for (int i = 0; i < f.size(); i++) System.out.println(s + " " + i + " " + names.get(f.get(i)));
        }
    }
}
