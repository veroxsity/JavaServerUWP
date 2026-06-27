package banditvault.xboxcompat.mixin;

import banditvault.xboxcompat.XboxCompatLog;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.io.IOException;
import java.net.URI;
import java.nio.file.FileSystem;
import java.nio.file.FileSystems;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;

@Mixin(value = FileSystems.class, remap = false)
public abstract class ZipFsBypassMixin {

    @Inject(method = "newFileSystem(Ljava/net/URI;Ljava/util/Map;Ljava/lang/ClassLoader;)Ljava/nio/file/FileSystem;",
            at = @At("HEAD"), cancellable = true, remap = false)
    private static void onNewFileSystem(URI uri, Map<String, ?> env, ClassLoader loader,
                                         CallbackInfoReturnable<FileSystem> cir) {
        try {
            if ("jar".equals(uri.getScheme())) {
                // uwp zipfs temp paths are not loadable
                Map<String, Object> patchedEnv = new HashMap<>();
                if (env != null) {
                    patchedEnv.putAll(env);
                }
                patchedEnv.putIfAbsent("create", "true");
                patchedEnv.putIfAbsent("useTempFile", Boolean.FALSE);

                Path jarPath = java.nio.file.Paths.get(
                    uri.getPath().replaceFirst("^file:", "")
                );
                URI jarUri = new URI("jar:file", null, jarPath.toAbsolutePath().toString(), null);

                FileSystem fs = FileSystems.newFileSystem(jarUri, patchedEnv, loader);
                cir.setReturnValue(fs);
                XboxCompatLog.debug("ZipFsBypass: created ZipFS for: {}", jarPath);
            }
        } catch (Exception e) {
            XboxCompatLog.debug("ZipFsBypass: bypass not needed for: {}", uri);
        }
    }
}
