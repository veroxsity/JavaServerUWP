package banditvault.uwpremap;

import net.fabricmc.loader.impl.lib.mappingio.MappingReader;
import net.fabricmc.loader.impl.lib.mappingio.tree.MemoryMappingTree;
import net.fabricmc.loader.impl.lib.tinyremapper.IMappingProvider;
import net.fabricmc.loader.impl.lib.tinyremapper.InputTag;
import net.fabricmc.loader.impl.lib.tinyremapper.NonClassCopyMode;
import net.fabricmc.loader.impl.lib.tinyremapper.OutputConsumerPath;
import net.fabricmc.loader.impl.lib.tinyremapper.TinyRemapper;
import net.fabricmc.loader.impl.lib.tinyremapper.TinyUtils;
import net.fabricmc.loader.impl.lib.tinyremapper.api.TrLogger;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public final class RemapHelper {
    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            System.err.println("Usage: RemapHelper <input.jar> <mappings.tiny> <output.jar>");
            System.exit(1);
        }

        Path input = Paths.get(args[0]).toAbsolutePath();
        Path mappingsPath = Paths.get(args[1]).toAbsolutePath();
        Path output = Paths.get(args[2]).toAbsolutePath();

        if (!Files.exists(input)) {
            System.err.println("Input jar does not exist: " + input);
            System.exit(2);
        }
        if (!Files.exists(mappingsPath)) {
            System.err.println("Mappings file does not exist: " + mappingsPath);
            System.exit(2);
        }

        Files.deleteIfExists(output);
        if (output.getParent() != null) {
            Files.createDirectories(output.getParent());
        }

        System.out.println("RemapHelper: " + input.getFileName() + " -> " + output.getFileName());
        long t0 = System.nanoTime();

        System.out.println("  Loading mappings...");
        MemoryMappingTree tree = new MemoryMappingTree();
        MappingReader.read(mappingsPath, tree);
        System.out.println("  Mappings src=" + tree.getSrcNamespace() + " dst=" + tree.getDstNamespaces());

        IMappingProvider provider = TinyUtils.createMappingProvider(tree, "official", "intermediary");

        TrLogger logger = new TrLogger() {
            @Override
            public void log(TrLogger.Level level, String msg) {
                System.out.println("    [tinyremapper:" + level + "] " + msg);
            }
        };

        TinyRemapper remapper = TinyRemapper.newRemapper(logger)
                .withMappings(provider)
                .renameInvalidLocals(true)
                .rebuildSourceFilenames(true)
                .build();

        try {
            InputTag tag = remapper.createInputTag();

            try (OutputConsumerPath out = new OutputConsumerPath.Builder(output).assumeArchive(true).build()) {

                out.addNonClassFiles(input, NonClassCopyMode.FIX_META_INF, remapper);

                remapper.readInputsAsync(tag, input).get();

                remapper.apply(out, tag);
            }
        } finally {
            remapper.finish();
        }

        long elapsedMs = (System.nanoTime() - t0) / 1_000_000L;
        long size = Files.size(output);
        System.out.println("RemapHelper: done in " + elapsedMs + "ms (" + size + " bytes)");
    }
}
