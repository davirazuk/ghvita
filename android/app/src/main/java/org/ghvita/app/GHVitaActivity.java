package org.ghvita.app;

import android.content.res.AssetManager;
import android.os.Bundle;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import org.libsdl.app.SDLActivity;

// SDL's activity, with the game's art unpacked first: the game reads its files by path (fopen), so the APK's "ghv"
// folder is copied into the app's own storage (files/assets) the first time each build starts.
public class GHVitaActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        try {
            unpack();
        } catch (Exception e) {
            android.util.Log.e("GHVita", "unpacking the art", e);
        }
        super.onCreate(savedInstanceState);
    }

    private void unpack() throws Exception {
        File root = new File(getFilesDir(), "assets");
        File stamp = new File(root, ".build");
        String want = String.valueOf(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
        if (stamp.exists()) {
            byte[] b = new byte[64];
            InputStream in = new java.io.FileInputStream(stamp);
            int n = in.read(b);
            in.close();
            if (n > 0 && new String(b, 0, n).equals(want))
                return;
        }
        copy(getAssets(), "ghv", root);
        OutputStream out = new FileOutputStream(stamp);
        out.write(want.getBytes());
        out.close();
        // songs and saves: a folder a file manager reaches (Android/data/org.ghvita.app/files)
        File ext = getExternalFilesDir(null);
        if (ext != null)
            new File(ext, "songs").mkdirs();
    }

    private void copy(AssetManager am, String from, File to) throws Exception {
        String[] names = am.list(from);
        if (names == null || names.length == 0) {
            // a file
            File dir = to.getParentFile();
            if (dir != null)
                dir.mkdirs();
            InputStream in = am.open(from);
            OutputStream out = new FileOutputStream(to);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0)
                out.write(buf, 0, n);
            out.close();
            in.close();
            return;
        }
        to.mkdirs();
        for (String name : names)
            copy(am, from + "/" + name, new File(to, name));
    }
}
