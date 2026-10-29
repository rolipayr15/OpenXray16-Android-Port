package org.openxray.android;

import android.content.Context;
import android.database.Cursor;
import android.os.Build;
import android.os.Environment;
import android.util.DisplayMetrics;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.io.IOException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

public final class OpenXRayActivity extends SDLActivity {
    public static final String EXTRA_GAME_DATA_URI = "org.openxray.android.GAME_DATA_URI";
    public static final String EXTRA_PROFILE_ID = "org.openxray.android.PROFILE_ID";
    public static final String EXTRA_PROFILE_NAME = "org.openxray.android.PROFILE_NAME";
    public static final String EXTRA_ENGINE_ARGUMENTS = "org.openxray.android.ENGINE_ARGUMENTS";
    public static final String EXTRA_RENDER_SCALE = "org.openxray.android.RENDER_SCALE";
    public static final String EXTRA_VIEW_DISTANCE = "org.openxray.android.VIEW_DISTANCE";
    public static final String EXTRA_MASTER_SERVER_HOST = "org.openxray.android.MASTER_SERVER_HOST";
    public static final String EXTRA_MASTER_SERVER_PORT = "org.openxray.android.MASTER_SERVER_PORT";
    public static final String EXTRA_GAME_MODE = "org.openxray.android.GAME_MODE";
    public static final String EXTRA_RENDERER = "org.openxray.android.RENDERER";
    /** Asset tree packaged by the stageEngineResources Gradle task. */
    private static final String ENGINE_ASSETS = "openxray";

    /** Directory names that never contain game data; skipping them keeps SAF walks short. */
    private static final Set<String> SKIPPED_DIRECTORIES = new HashSet<>(Arrays.asList(
        ".git", ".svn", ".hg", "$recycle.bin", "system volume information",
        "android", "dcim", "download", "pictures", "movies", "music",
        "alarms", "ringtones", "notifications", "podcasts"));
    private static final int MAX_WALK_DEPTH = 14;

    private String gameDataUri = "";
    private String profileId = "default";
    private String profileName = "Default";
    private String engineArguments = "";
    private String appFilesPath = "";
    private int renderScale = 100;
    private int viewDistance = 100;
    private String masterServerHost = "";
    private int masterServerPort = 28910;
    private String gameMode = "auto";
    private String renderer = "vulkan";
    private String engineResourcesPath = "";
    /** Lower-case relative path -> document id, filled by {@link #walkGameDataTree()}. */
    private final Map<String, String> documentIds = new HashMap<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String selectedUri = getIntent().getStringExtra(EXTRA_GAME_DATA_URI);
        if (selectedUri != null) {
            gameDataUri = selectedUri;
        }
        String selectedProfile = getIntent().getStringExtra(EXTRA_PROFILE_ID);
        if (selectedProfile != null && selectedProfile.matches("[A-Za-z0-9_-]+"))
            profileId = selectedProfile;
        String selectedName = getIntent().getStringExtra(EXTRA_PROFILE_NAME);
        if (selectedName != null)
            profileName = selectedName;
        String selectedArguments = getIntent().getStringExtra(EXTRA_ENGINE_ARGUMENTS);
        if (selectedArguments != null)
            engineArguments = selectedArguments;
        renderScale = Math.max(50, Math.min(100, getIntent().getIntExtra(EXTRA_RENDER_SCALE, 100)));
        viewDistance = Math.max(40, Math.min(100,
            getIntent().getIntExtra(EXTRA_VIEW_DISTANCE, 80)));
        String selectedMasterHost = getIntent().getStringExtra(EXTRA_MASTER_SERVER_HOST);
        if (selectedMasterHost != null &&
            (selectedMasterHost.isEmpty() || selectedMasterHost.matches("[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?")))
            masterServerHost = selectedMasterHost;
        int selectedMasterPort = getIntent().getIntExtra(EXTRA_MASTER_SERVER_PORT, 28910);
        if (selectedMasterPort >= 1 && selectedMasterPort <= 65535)
            masterServerPort = selectedMasterPort;
        String selectedGameMode = getIntent().getStringExtra(EXTRA_GAME_MODE);
        if (selectedGameMode != null && selectedGameMode.matches("auto|shoc|cs|cop|unlock"))
            gameMode = selectedGameMode;
        String selectedRenderer = getIntent().getStringExtra(EXTRA_RENDERER);
        if ("gles".equals(selectedRenderer))
            renderer = selectedRenderer;
        engineResourcesPath = unpackEngineResources();
        File profileDirectory = new File(getFilesDir(), "profiles/" + profileId);
        if (!profileDirectory.exists())
            profileDirectory.mkdirs();
        appFilesPath = profileDirectory.getAbsolutePath();
        super.onCreate(savedInstanceState);
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        ProfileSurface surface = new ProfileSurface(context);
        // SDL adds its SurfaceView to a RelativeLayout without layout params.
        // Once a fixed-size buffer is requested, SurfaceView's measured size can
        // otherwise collapse to the buffer dimensions as well.  Keep the View
        // full-screen so SurfaceFlinger upscales only the lower-resolution
        // buffer and Android continues to dispatch input across the whole screen.
        surface.setLayoutParams(new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        if (renderScale < 100) {
            DisplayMetrics metrics = new DisplayMetrics();
            ((WindowManager) context.getSystemService(Context.WINDOW_SERVICE))
                .getDefaultDisplay().getRealMetrics(metrics);
            int longSide = Math.max(metrics.widthPixels, metrics.heightPixels);
            int shortSide = Math.min(metrics.widthPixels, metrics.heightPixels);
            int width = Math.max(640, Math.round(longSide * renderScale / 200.0f) * 2);
            int height = Math.max(360, Math.round(shortSide * renderScale / 200.0f) * 2);
            surface.getHolder().setFixedSize(width, height);
        }
        return surface;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        return new String[] {
            "--game-data-uri", gameDataUri,
            "--game-data-path", directGameDataPath(this, gameDataUri),
            "--game-mode", gameMode,
            "--renderer", renderer,
            "--engine-resources", engineResourcesPath,
            "--app-files-path", appFilesPath,
            "--profile-id", profileId,
            "--profile-name", profileName,
            "--render-scale", Integer.toString(renderScale),
            "--view-distance", Integer.toString(viewDistance),
            "--master-host", masterServerHost,
            "--master-port", Integer.toString(masterServerPort),
            "--engine-args", engineArguments
        };
    }

    /** Keeps SDL touch coordinates normalized to the full-screen View when
     * Android scales a lower-resolution fixed Surface buffer to that View. */
    private static final class ProfileSurface extends SDLSurface {
        ProfileSurface(Context context) {
            super(context);
        }

        @Override
        public boolean onTouch(View view, MotionEvent event) {
            int source = event.getSource();
            if (source == InputDevice.SOURCE_MOUSE ||
                source == (InputDevice.SOURCE_MOUSE | InputDevice.SOURCE_TOUCHSCREEN))
                return super.onTouch(view, event);

            int deviceId = event.getDeviceId();
            if (deviceId < 0)
                --deviceId;
            int action = event.getActionMasked();
            int width = Math.max(1, view.getWidth());
            int height = Math.max(1, view.getHeight());
            if (action == MotionEvent.ACTION_MOVE || action == MotionEvent.ACTION_CANCEL) {
                int nativeAction = action == MotionEvent.ACTION_CANCEL ? MotionEvent.ACTION_UP : action;
                for (int index = 0; index < event.getPointerCount(); ++index)
                    forwardTouch(event, index, deviceId, nativeAction, width, height);
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_DOWN ||
                action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_POINTER_DOWN) {
                int index = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_DOWN
                    ? 0 : event.getActionIndex();
                forwardTouch(event, index, deviceId, action, width, height);
            }
            return true;
        }

        private static void forwardTouch(MotionEvent event, int index, int deviceId,
            int action, int width, int height) {
            float pressure = Math.min(1.0f, event.getPressure(index));
            SDLActivity.onNativeTouch(deviceId, event.getPointerId(index), action,
                event.getX(index) / width, event.getY(index) / height, pressure);
        }
    }

    /**
     * Copies the engine-owned resources (OpenXRay's GL shaders) from the APK
     * into app storage once per installed build, so the native locator can
     * read them as ordinary files. Returns the tree root or "" on failure.
     */
    private String unpackEngineResources() {
        File root = new File(getFilesDir(), "engine-resources");
        File stamp = new File(root, ".stamp");
        String build;
        try {
            build = Long.toString(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
        } catch (android.content.pm.PackageManager.NameNotFoundException exception) {
            build = "unknown";
        }
        try {
            if (stamp.isFile() && build.equals(new String(java.nio.file.Files.readAllBytes(stamp.toPath()),
                    java.nio.charset.StandardCharsets.UTF_8)))
                return root.getAbsolutePath();
            deleteRecursively(root);
            copyAssetTree(ENGINE_ASSETS, root);
            java.nio.file.Files.write(stamp.toPath(), build.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            return root.getAbsolutePath();
        } catch (IOException exception) {
            android.util.Log.e("OpenXRay", "Could not unpack engine resources", exception);
            return "";
        }
    }

    private void copyAssetTree(String assetPath, File destination) throws IOException {
        String[] children = getAssets().list(assetPath);
        if (children == null || children.length == 0) {
            File parent = destination.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                throw new IOException("Cannot create " + parent);
            try (java.io.InputStream input = getAssets().open(assetPath);
                 java.io.OutputStream output = new java.io.FileOutputStream(destination)) {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = input.read(buffer)) > 0)
                    output.write(buffer, 0, read);
            }
            return;
        }
        if (!destination.isDirectory() && !destination.mkdirs())
            throw new IOException("Cannot create " + destination);
        for (String child : children)
            copyAssetTree(assetPath + "/" + child, new File(destination, child));
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null)
            for (File child : children)
                deleteRecursively(child);
        file.delete();
    }

    /** True when the app may read shared storage through ordinary file paths. */
    public static boolean hasDirectStorageAccess(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            return Environment.isExternalStorageManager();
        return context.checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
            == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    /**
     * Maps a tree URI from the external-storage provider to its filesystem path
     * when direct access is granted and the directory is readable. Direct paths
     * let the unmodified engine locator see loose files, mods and the game's own
     * layout at native speed; anything else falls back to the SAF bridge.
     */
    public static String directGameDataPath(Context context, String treeUri) {
        if (treeUri == null || treeUri.isEmpty() || !hasDirectStorageAccess(context))
            return "";
        final String documentId;
        try {
            Uri tree = Uri.parse(treeUri);
            if (!"com.android.externalstorage.documents".equals(tree.getAuthority()))
                return "";
            documentId = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException exception) {
            return "";
        }
        int colon = documentId.indexOf(':');
        if (colon <= 0)
            return "";
        String volume = documentId.substring(0, colon);
        String relative = documentId.substring(colon + 1);
        File base = "primary".equalsIgnoreCase(volume)
            ? Environment.getExternalStorageDirectory() : new File("/storage/" + volume);
        File directory = relative.isEmpty() ? base : new File(base, relative);
        return directory.isDirectory() && directory.canRead() ? directory.getAbsolutePath() : "";
    }

    /**
     * Walks the whole selected tree once and returns flat quadruples of
     * type ("d"/"f"), size, modification time in seconds and the path relative
     * to the tree root. One provider query is issued per directory, and every
     * document id is cached so later opens need a single binder call.
     */
    public String[] walkGameDataTree() {
        documentIds.clear();
        if (gameDataUri.isEmpty())
            return null;

        final Uri tree;
        final String rootId;
        try {
            tree = Uri.parse(gameDataUri);
            rootId = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException exception) {
            return null;
        }

        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED
        };
        List<String> result = new ArrayList<>();
        ArrayDeque<String[]> pending = new ArrayDeque<>();
        pending.add(new String[] { rootId, "", "0" });
        documentIds.put("", rootId);
        while (!pending.isEmpty()) {
            String[] directory = pending.poll();
            String parentPath = directory[1];
            int depth = Integer.parseInt(directory[2]);
            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, directory[0]);
            try (Cursor cursor = getContentResolver().query(children, projection, null, null, null)) {
                if (cursor == null)
                    return null;
                while (cursor.moveToNext()) {
                    String id = cursor.getString(0);
                    String name = cursor.getString(1);
                    if (id == null || name == null || name.isEmpty() || name.indexOf('/') >= 0)
                        continue;
                    boolean isDirectory = DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2));
                    String path = parentPath.isEmpty() ? name : parentPath + "/" + name;
                    if (isDirectory) {
                        if (depth >= MAX_WALK_DEPTH || SKIPPED_DIRECTORIES.contains(name.toLowerCase(Locale.ROOT)))
                            continue;
                        pending.add(new String[] { id, path, Integer.toString(depth + 1) });
                    }
                    documentIds.put(path.toLowerCase(Locale.ROOT), id);
                    long size = cursor.isNull(3) ? -1 : cursor.getLong(3);
                    long modified = cursor.isNull(4) ? 0 : cursor.getLong(4) / 1000;
                    result.add(isDirectory ? "d" : "f");
                    result.add(Long.toString(size));
                    result.add(Long.toString(modified));
                    result.add(path);
                }
            } catch (RuntimeException exception) {
                return null;
            }
        }
        return result.toArray(new String[0]);
    }

    /** Returns flat type/size/name triples, or null when the persisted tree cannot be read. */
    public String[] listGameDataDirectory(String relativePath) {
        Uri directory = resolveGameDataDocument(relativePath);
        if (directory == null) {
            return null;
        }

        List<String> names = new ArrayList<>();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(
            directory,
            DocumentsContract.getDocumentId(directory)
        );
        String[] projection = {
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE
        };
        try (Cursor cursor = getContentResolver().query(children, projection, null, null, null)) {
            if (cursor == null) {
                return null;
            }
            while (cursor.moveToNext()) {
                String name = cursor.getString(0);
                if (name != null && !name.isEmpty()) {
                    boolean directoryEntry = DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(1));
                    long size = cursor.isNull(2) ? -1 : cursor.getLong(2);
                    // Flat triples keep the JNI boundary dependency-free and avoid
                    // delimiter escaping for user-controlled display names.
                    names.add(directoryEntry ? "d" : "f");
                    names.add(Long.toString(size));
                    names.add(name);
                }
            }
        } catch (RuntimeException exception) {
            return null;
        }

        List<String[]> entries = new ArrayList<>();
        for (int index = 0; index < names.size(); index += 3) {
            entries.add(new String[] { names.get(index), names.get(index + 1), names.get(index + 2) });
        }
        Collections.sort(entries, (left, right) -> String.CASE_INSENSITIVE_ORDER.compare(left[2], right[2]));
        List<String> sorted = new ArrayList<>(names.size());
        for (String[] entry : entries) {
            Collections.addAll(sorted, entry);
        }
        return sorted.toArray(new String[0]);
    }

    /**
     * Opens a file below the selected tree and transfers ownership of the read-only
     * descriptor to native code. Native code must close every non-negative result.
     */
    public int openGameDataFile(String relativePath) {
        Uri document = cachedGameDataDocument(relativePath);
        if (document == null)
            document = resolveGameDataDocument(relativePath);
        if (document == null) {
            return -1;
        }

        try (ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(document, "r")) {
            return descriptor == null ? -1 : descriptor.detachFd();
        } catch (IOException | RuntimeException exception) {
            return -1;
        }
    }

    private Uri cachedGameDataDocument(String relativePath) {
        if (documentIds.isEmpty() || relativePath == null)
            return null;
        String id = documentIds.get(relativePath.replace('\\', '/').toLowerCase(Locale.ROOT));
        if (id == null)
            return null;
        try {
            return DocumentsContract.buildDocumentUriUsingTree(Uri.parse(gameDataUri), id);
        } catch (IllegalArgumentException exception) {
            return null;
        }
    }

    private Uri resolveGameDataDocument(String relativePath) {
        if (gameDataUri.isEmpty()) {
            return null;
        }

        Uri tree = Uri.parse(gameDataUri);
        final String rootId;
        try {
            rootId = DocumentsContract.getTreeDocumentId(tree);
        } catch (IllegalArgumentException exception) {
            return null;
        }

        Uri current = DocumentsContract.buildDocumentUriUsingTree(tree, rootId);
        String normalized = relativePath == null ? "" : relativePath.replace('\\', '/');
        for (String segment : normalized.split("/")) {
            if (segment.isEmpty() || segment.equals(".")) {
                continue;
            }
            if (segment.equals("..")) {
                return null;
            }
            current = findChildDocument(current, segment);
            if (current == null) {
                return null;
            }
        }
        return current;
    }

    private Uri findChildDocument(Uri parent, String displayName) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(
            parent,
            DocumentsContract.getDocumentId(parent)
        );
        String[] projection = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME
        };
        try (Cursor cursor = getContentResolver().query(children, projection, null, null, null)) {
            if (cursor == null) {
                return null;
            }
            while (cursor.moveToNext()) {
                String childName = cursor.getString(1);
                if (childName != null && childName.equalsIgnoreCase(displayName)) {
                    return DocumentsContract.buildDocumentUriUsingTree(parent, cursor.getString(0));
                }
            }
        } catch (RuntimeException exception) {
            return null;
        }
        return null;
    }

}
