package dev.teton.somniel;

import android.content.Context;

/** The app's versionName ("0.0.1-m0"). */
final class BuildVersion {
    static String name(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Throwable t) {
            return "unknown";
        }
    }
}
