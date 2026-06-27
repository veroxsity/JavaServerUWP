package banditvault.xboxcompat;

import java.security.Permission;

public final class ExitTrap {
    private static volatile boolean installed;
    private static volatile boolean triggered;
    private static volatile int lastStatus;

    private ExitTrap() {
        throw new AssertionError("no instances");
    }

    public static void install() {
        if (installed) {
            return;
        }
        System.setSecurityManager(new NoExitSecurityManager(System.getSecurityManager()));
        installed = true;
    }

    public static boolean wasTriggered() {
        return triggered;
    }

    public static int lastStatus() {
        return lastStatus;
    }

    public static final class ExitTrappedException extends SecurityException {
        private final int status;

        public ExitTrappedException(int status) {
            super("System.exit(" + status + ") blocked by UWP host");
            this.status = status;
        }

        public int status() {
            return status;
        }
    }

    private static final class NoExitSecurityManager extends SecurityManager {
        private final SecurityManager parent;

        NoExitSecurityManager(SecurityManager parent) {
            this.parent = parent;
        }

        @Override
        public void checkPermission(Permission perm) {
            if (parent != null) {
                parent.checkPermission(perm);
            }
        }

        @Override
        public void checkPermission(Permission perm, Object context) {
            if (parent != null) {
                parent.checkPermission(perm, context);
            }
        }

        @Override
        public void checkExit(int status) {
            triggered = true;
            lastStatus = status;
            if (parent != null) {
                parent.checkExit(status);
            }
            throw new ExitTrappedException(status);
        }
    }
}
