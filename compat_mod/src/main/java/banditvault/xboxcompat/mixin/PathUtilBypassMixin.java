package banditvault.xboxcompat.mixin;

import banditvault.xboxcompat.XboxCompatLog;
import net.fabricmc.loader.impl.util.UrlUtil;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.net.URL;

@Mixin(value = UrlUtil.class, remap = false)
public abstract class PathUtilBypassMixin {

    @Inject(method = "asPath", at = @At("HEAD"), cancellable = true, remap = false)
    private static void onAsPath(URL url, CallbackInfoReturnable<java.nio.file.Path> cir) {
        try {
            String protocol = url.getProtocol();
            if ("jar".equals(protocol)) {
                // jar urls can arrive as /c:/ paths
                String path = url.getPath();
                int exclIdx = path.indexOf('!');
                if (exclIdx >= 0) {
                    path = path.substring(0, exclIdx);
                }
                if (path.startsWith("/") && path.length() > 2 && path.charAt(2) == ':') {
                    path = path.substring(1);
                }
                java.nio.file.Path result = java.nio.file.Paths.get(path);
                XboxCompatLog.debug("PathUtilBypass: jar URL resolved: {} -> {}", url, result);
                cir.setReturnValue(result);
            }
        } catch (Exception e) {
            XboxCompatLog.warn("PathUtilBypass: failed to resolve URL: {}", url, e);
        }
    }
}
