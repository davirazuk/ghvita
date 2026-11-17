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
        // the phone's own updates: the "android" release on GitHub, checked in the background each start
        new Thread(new Runnable() {
            public void run() {
                try {
                    checkUpdate();
                } catch (Exception e) {
                    android.util.Log.w("GHVita", "update check", e);
                }
            }
        }).start();
    }

    // The repository is private: as on the Vita, a read-only token in update.txt (here in Android/data/org.ghvita.app/
    // files). The release carries version.txt (its build number); a newer one than this app's is downloaded and handed
    // to Android's installer, which asks before it installs
    private static final String API = "https://api.github.com/repos/davirazuk/ghvita/releases/tags/android";

    private String readToken() throws Exception {
        java.io.File f = new java.io.File(getExternalFilesDir(null), "update.txt");
        if (!f.exists()) {
            // the one the build baked in (the repository's ANDROID_UPDATE_TOKEN secret)
            try {
                InputStream in = getAssets().open("ghv/update_token");
                String t = readAll(in).trim();
                return t.isEmpty() ? null : t;
            } catch (java.io.IOException e) {
                return null;
            }
        }
        java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f));
        String t = r.readLine();
        r.close();
        return t == null ? null : t.trim();
    }

    private java.net.HttpURLConnection open(String url, String token, String accept) throws Exception {
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
        c.setRequestProperty("Authorization", "Bearer " + token);
        c.setRequestProperty("Accept", accept);
        c.setRequestProperty("User-Agent", "GHVita-Android");
        c.setInstanceFollowRedirects(true);
        c.setConnectTimeout(15000);
        c.setReadTimeout(60000);
        return c;
    }

    private String readAll(InputStream in) throws Exception {
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[1 << 14];
        int n;
        while ((n = in.read(buf)) > 0)
            b.write(buf, 0, n);
        in.close();
        return b.toString("UTF-8");
    }

    private void checkUpdate() throws Exception {
        String token = readToken();
        if (token == null || token.isEmpty())
            return;
        org.json.JSONObject rel = new org.json.JSONObject(readAll(open(API, token, "application/vnd.github+json").getInputStream()));
        org.json.JSONArray assets = rel.getJSONArray("assets");
        String apk = null, ver = null;
        for (int i = 0; i < assets.length(); i++) {
            org.json.JSONObject a = assets.getJSONObject(i);
            if (a.getString("name").equals("GHVita-android.apk"))
                apk = a.getString("url");
            if (a.getString("name").equals("version.txt"))
                ver = a.getString("url");
        }
        if (apk == null || ver == null)
            return;
        int latest = Integer.parseInt(readAll(open(ver, token, "application/octet-stream").getInputStream()).trim());
        int mine = getPackageManager().getPackageInfo(getPackageName(), 0).versionCode;
        if (latest <= mine)
            return;
        java.io.File dir = new java.io.File(getExternalCacheDir(), "update");
        dir.mkdirs();
        java.io.File out = new java.io.File(dir, "GHVita-" + latest + ".apk");
        if (!out.exists() || out.length() < 1000000) {
            java.io.File part = new java.io.File(dir, "download.part");
            InputStream in = open(apk, token, "application/octet-stream").getInputStream();
            OutputStream o = new FileOutputStream(part);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0)
                o.write(buf, 0, n);
            o.close();
            in.close();
            part.renameTo(out);
        }
        final android.net.Uri uri = androidx.core.content.FileProvider.getUriForFile(this, getPackageName() + ".files", out);
        runOnUiThread(new Runnable() {
            public void run() {
                android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_VIEW);
                i.setDataAndType(uri, "application/vnd.android.package-archive");
                i.addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION | android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                android.widget.Toast.makeText(GHVitaActivity.this, "A new GH Vita is ready: install it", android.widget.Toast.LENGTH_LONG).show();
                startActivity(i);
            }
        });
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
