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
        startUpdateCheck(true);
    }

    // the phone's own updates: the "android" release on GitHub, checked in the background at each start and again when
    // the app comes back to the front (at most every ten minutes); what happened is said on the screen and kept in
    // update.log beside the songs, so a phone that doesn't update says why
    private static volatile boolean checking;
    private long lastCheck;
    private java.io.File pending;       // downloaded, waiting for the phone to allow installing it

    @Override
    protected void onResume() {
        super.onResume();
        if (pending != null && pending.exists())
            install(pending);
        else if (System.currentTimeMillis() - lastCheck > 10 * 60 * 1000)
            startUpdateCheck(false);
    }

    private void startUpdateCheck(final boolean first) {
        if (checking)
            return;
        checking = true;
        lastCheck = System.currentTimeMillis();
        new Thread(new Runnable() {
            public void run() {
                try {
                    checkUpdate();
                } catch (Exception e) {
                    android.util.Log.w("GHVita", "update check", e);
                    note("Couldn't check for updates: " + e.getMessage(), true);
                } finally {
                    checking = false;
                }
            }
        }).start();
    }

    private void note(final String what, final boolean show) {
        try {
            java.io.File ext = getExternalFilesDir(null);
            if (ext != null) {
                java.io.FileWriter w = new java.io.FileWriter(new java.io.File(ext, "update.log"), true);
                w.write(new java.util.Date() + "  " + what + "\n");
                w.close();
                // the game's main menu shows the last of them
                java.io.FileWriter st = new java.io.FileWriter(new java.io.File(ext, "update_state.txt"), false);
                st.write("Update: " + what + "\n");
                st.close();
            }
        } catch (Exception e) {
            // nothing to do
        }
        if (show)
            runOnUiThread(new Runnable() {
                public void run() {
                    android.widget.Toast.makeText(GHVitaActivity.this, what, android.widget.Toast.LENGTH_LONG).show();
                }
            });
    }

    // the installer, once the phone lets this app install (Android 8 asks once, in its settings)
    private void install(final java.io.File apk) {
        runOnUiThread(new Runnable() {
            public void run() {
                if (android.os.Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
                    pending = apk;
                    android.widget.Toast.makeText(GHVitaActivity.this,
                        "GH Vita update ready: allow \"Install unknown apps\" for GH Vita, then come back", android.widget.Toast.LENGTH_LONG).show();
                    try {
                        startActivity(new android.content.Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                            android.net.Uri.parse("package:" + getPackageName())));
                    } catch (Exception e) {
                        android.util.Log.w("GHVita", "unknown sources settings", e);
                    }
                    return;
                }
                pending = null;
                android.net.Uri uri = androidx.core.content.FileProvider.getUriForFile(GHVitaActivity.this, getPackageName() + ".files", apk);
                android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_VIEW);
                i.setDataAndType(uri, "application/vnd.android.package-archive");
                i.addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION | android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                android.widget.Toast.makeText(GHVitaActivity.this, "A new GH Vita is ready: install it", android.widget.Toast.LENGTH_LONG).show();
                startActivity(i);
            }
        });
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
        if (token != null && !token.isEmpty())
            c.setRequestProperty("Authorization", "Bearer " + token);
        c.setRequestProperty("Accept", accept);
        c.setRequestProperty("User-Agent", "GHVita-Android");
        c.setInstanceFollowRedirects(true);
        c.setConnectTimeout(15000);
        c.setReadTimeout(60000);
        return c;
    }

    // a release asset: the API answers with a redirect to the file's storage, which is fetched without the token
    private InputStream asset(String url, String token) throws Exception {
        java.net.HttpURLConnection c = open(url, token, "application/octet-stream");
        c.setInstanceFollowRedirects(false);
        int code = c.getResponseCode();
        if (code >= 300 && code < 400) {
            String to = c.getHeaderField("Location");
            c.disconnect();
            java.net.HttpURLConnection d = (java.net.HttpURLConnection) new java.net.URL(to).openConnection();
            d.setRequestProperty("User-Agent", "GHVita-Android");
            d.setConnectTimeout(15000);
            d.setReadTimeout(60000);
            if (d.getResponseCode() != 200)
                throw new java.io.IOException("asset: HTTP " + d.getResponseCode());
            return d.getInputStream();
        }
        if (code != 200)
            throw new java.io.IOException("asset: HTTP " + code);
        return c.getInputStream();
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

    // The second way builds arrive: the repository's "builds" branch, pushed straight from a build machine (no release and
    // no GitHub Actions needed). android/version.txt is the build, android/parts.txt the pieces (each under GitHub's
    // 100 MB a file) that joined make the APK. The newer of the release and the branch is taken.
    private static final String CONTENTS = "https://api.github.com/repos/davirazuk/ghvita/contents/android/";
    private String rawToken;            // the token the release check got through with (the repository is private)

    private InputStream raw(String name) throws Exception {
        java.net.HttpURLConnection c = open(CONTENTS + name + "?ref=builds", rawToken, "application/vnd.github.raw");
        if (c.getResponseCode() != 200)
            throw new java.io.IOException(name + ": HTTP " + c.getResponseCode());
        return c.getInputStream();
    }

    private void checkUpdate() throws Exception {
        int mine = getPackageManager().getPackageInfo(getPackageName(), 0).versionCode;
        String token = readToken(), apk = null;
        int latest = 0;
        try {
            // (no token: asked as anyone, which a public repository answers)
            java.net.HttpURLConnection rc = open(API, token, "application/vnd.github+json");
            int code = rc.getResponseCode();
            if (code == 401 || code == 403 || code == 404) {
                // an old token in update.txt: the one built into the app instead
                String baked = null;
                try {
                    baked = readAll(getAssets().open("ghv/update_token")).trim();
                } catch (java.io.IOException e) {
                    baked = null;
                }
                if (baked != null && !baked.isEmpty() && !baked.equals(token)) {
                    token = baked;
                    rc = open(API, token, "application/vnd.github+json");
                    code = rc.getResponseCode();
                }
            }
            if ((code == 401 || code == 403 || code == 404) && token != null) {
                token = null;                   // refused: asked without one (a public repository answers)
                rc = open(API, token, "application/vnd.github+json");
                code = rc.getResponseCode();
            }
            if (code == 200) {
                org.json.JSONObject rel = new org.json.JSONObject(readAll(rc.getInputStream()));
                org.json.JSONArray assets = rel.getJSONArray("assets");
                String ver = null;
                for (int i = 0; i < assets.length(); i++) {
                    org.json.JSONObject a = assets.getJSONObject(i);
                    if (a.getString("name").equals("GHVita-android.apk"))
                        apk = a.getString("url");
                    if (a.getString("name").equals("version.txt"))
                        ver = a.getString("url");
                }
                if (apk != null && ver != null)
                    latest = Integer.parseInt(readAll(asset(ver, token)).trim());
            } else
                note("GitHub answered " + code + " to the update check", false);
        } catch (Exception e) {
            android.util.Log.w("GHVita", "release check", e);
        }
        // the builds branch
        rawToken = token;
        java.util.List<String> parts = new java.util.ArrayList<>();
        java.util.List<Long> sizes = new java.util.ArrayList<>();
        int branch = 0;
        try {
            branch = Integer.parseInt(readAll(raw("version.txt")).trim());
            if (branch > Math.max(latest, mine)) {
                for (String line : readAll(raw("parts.txt")).split("\\r?\\n")) {
                    String[] f = line.trim().split("\\s+");
                    if (f.length == 2 && !f[0].contains("/") && !f[0].contains(".."))  {
                        parts.add(f[0]);
                        sizes.add(Long.parseLong(f[1]));
                    }
                }
            }
        } catch (Exception e) {
            android.util.Log.w("GHVita", "builds branch check", e);
        }
        boolean fromBranch = !parts.isEmpty() && branch > Math.max(latest, mine);
        if (fromBranch)
            latest = branch;
        if (latest <= mine) {
            note("GH Vita is up to date (build " + mine + ")", true);
            return;
        }
        note("downloading GH Vita build " + latest + " (this is " + mine + ")", true);
        java.io.File dir = new java.io.File(getExternalCacheDir(), "update");
        dir.mkdirs();
        java.io.File out = new java.io.File(dir, "GHVita-" + latest + ".apk");
        if (!out.exists() || out.length() < 1000000) {
            java.io.File part = new java.io.File(dir, "download.part");
            OutputStream o = new FileOutputStream(part);
            byte[] buf = new byte[1 << 16];
            int n;
            if (fromBranch) {
                for (int i = 0; i < parts.size(); i++) {
                    InputStream in = raw(parts.get(i));
                    long have = 0;
                    while ((n = in.read(buf)) > 0) {
                        o.write(buf, 0, n);
                        have += n;
                    }
                    in.close();
                    if (have != sizes.get(i)) {
                        o.close();
                        part.delete();
                        note("piece " + (i + 1) + " of the download came out the wrong size", true);
                        return;
                    }
                }
            } else {
                InputStream in = asset(apk, token);
                while ((n = in.read(buf)) > 0)
                    o.write(buf, 0, n);
                in.close();
            }
            o.close();
            part.renameTo(out);
        }
        if (out.length() < 1000000) {
            note("the download came out too small (" + out.length() + " bytes)", true);
            out.delete();
            return;
        }
        note("build " + latest + " downloaded, installing", true);
        install(out);
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
