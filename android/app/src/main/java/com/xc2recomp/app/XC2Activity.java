package com.xc2recomp.app;

import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * Android entry Activity for XC2 Recomp (ARM64 / Adreno 740 / 8 GB RAM).
 *
 * Subclasses SDL3's SDLActivity so SDL owns the SurfaceView, audio, gamepad, sensor and touch pipelines while
 * injecting `XC2_DATA_DIR`, enabling Android Sustained Performance Mode, and forwarding optional `adb shell am start`
 * `--es args "--selftest"` CLI arguments to `SDL_AppInit`.
 */
public class XC2Activity extends SDLActivity {
    private static final String TAG = "XC2Recomp";

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "main"
        };
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        if (intent != null) {
            String raw = intent.getStringExtra("args");
            if (raw != null && !raw.trim().isEmpty()) {
                return raw.trim().split("\\s+");
            }
        }
        return super.getArguments();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        configureDataDirectory();
        super.onCreate(savedInstanceState);
        configureWindowAndThermals();
    }

    private void configureDataDirectory() {
        File ext = getExternalFilesDir(null);
        if (ext == null) {
            ext = getFilesDir();
        }
        if (ext != null) {
            if (!ext.exists()) {
                //noinspection ResultOfMethodCallIgnored
                ext.mkdirs();
            }
            try {
                String existing = Os.getenv("XC2_DATA_DIR");
                if (existing == null || existing.isEmpty()) {
                    Os.setenv("XC2_DATA_DIR", ext.getAbsolutePath(), true);
                }
                Os.setenv("XC2_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            } catch (ErrnoException e) {
                Log.w(TAG, "Failed to export XC2_DATA_DIR", e);
            }
        }
    }

    private void configureWindowAndThermals() {
        Window window = getWindow();
        if (window == null) {
            return;
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            WindowManager.LayoutParams lp = window.getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(lp);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            View decor = window.getDecorView();
            decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_FULLSCREEN);
        }

        PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (pm != null && pm.isSustainedPerformanceModeSupported()) {
            window.setSustainedPerformanceMode(true);
            Log.i(TAG, "Enabled Android SustainedPerformanceMode");
        }
    }
}
