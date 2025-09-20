
package io.ratelimit;

public class NativeRateLimiter {
    static {
        try {
            System.loadLibrary("ratelimit_jni");
        } catch (UnsatisfiedLinkError e) {
            // Fallback: allow user to set -Djava.library.path
            throw e;
        }
    }

    public static class Decision {
        public final boolean allowed;
        public final long remaining;
        public final long resetMs;
        public Decision(boolean allowed, long remaining, long resetMs) {
            this.allowed = allowed;
            this.remaining = remaining;
            this.resetMs = resetMs;
        }
        @Override public String toString() {
            return "Decision{allowed=" + allowed + ", remaining=" + remaining + ", resetMs=" + resetMs + "}";
        }
    }

    // Native binding to C++ library (singleton limiter inside native side)
    public static native Decision allow(String key, int capacity, int refillPerSec);
}
