// Development helper (not part of the server): writes the climate parameter lists Mojang's code generates, in list order, one point per line:
//   preset temp_min temp_max humidity_min humidity_max continentalness_min .. erosion .. depth .. weirdness .. offset biome
// usage: java -cp server.jar:libs/* tools/oracle/DumpClimate.java OUTFILE
import java.io.PrintWriter;
import java.util.Map;
import net.minecraft.SharedConstants;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;

public class DumpClimate {
    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (Map.Entry<MultiNoiseBiomeSourceParameterList.Preset, Climate.ParameterList<ResourceKey<Biome>>> e : MultiNoiseBiomeSourceParameterList.knownPresets().entrySet()) {
                String name = e.getKey().id().getPath();
                for (var p : e.getValue().values()) {
                    Climate.ParameterPoint pt = p.getFirst();
                    w.println(name + " " + pt.temperature().min() + " " + pt.temperature().max() + " " + pt.humidity().min() + " " + pt.humidity().max() + " "
                        + pt.continentalness().min() + " " + pt.continentalness().max() + " " + pt.erosion().min() + " " + pt.erosion().max() + " "
                        + pt.depth().min() + " " + pt.depth().max() + " " + pt.weirdness().min() + " " + pt.weirdness().max() + " " + pt.offset() + " "
                        + p.getSecond().identifier().getPath());
                }
            }
        }
    }
}
