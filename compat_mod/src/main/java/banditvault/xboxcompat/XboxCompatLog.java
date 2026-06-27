package banditvault.xboxcompat;

import java.util.logging.Level;
import java.util.logging.Logger;

public final class XboxCompatLog {
    public static final Logger LOGGER = Logger.getLogger("XboxCompat");

    static {
        LOGGER.setLevel(Level.ALL);
    }

    private XboxCompatLog() {
        throw new AssertionError("no instances");
    }

    private static String format(String message, Object... args) {
        if (args == null || args.length == 0) {
            return "[XboxCompat] " + message;
        }
        String formatted = message;
        for (Object arg : args) {
            formatted = formatted.replaceFirst("\\{\\}", String.valueOf(arg));
        }
        return "[XboxCompat] " + formatted;
    }

    public static void info(String message, Object... args) {
        LOGGER.info(format(message, args));
    }

    public static void warn(String message, Object... args) {
        LOGGER.warning(format(message, args));
    }

    public static void error(String message, Object... args) {
        LOGGER.severe(format(message, args));
    }

    public static void debug(String message, Object... args) {
        LOGGER.fine(format(message, args));
    }
}
