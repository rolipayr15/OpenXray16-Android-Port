package org.openxray.android;

import android.content.Context;
import android.database.Cursor;
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
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class OpenXRayActivity extends SDLActivity {
    public static final String EXTRA_GAME_DATA_URI = "org.openxray.android.GAME_DATA_URI";
    public static final String EXTRA_PROFILE_ID = "org.openxray.android.PROFILE_ID";
    public static final String EXTRA_PROFILE_NAME = "org.openxray.android.PROFILE_NAME";
    public static final String EXTRA_ENGINE_ARGUMENTS = "org.openxray.android.ENGINE_ARGUMENTS";
    public static final String EXTRA_RENDER_SCALE = "org.openxray.android.RENDER_SCALE";

    private String gameDataUri = "";
    private String profileId = "default";
    private String profileName = "Default";
    private String engineArguments = "";
    private String appFilesPath = "";
    private int renderScale = 100;

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
            "--app-files-path", appFilesPath,
            "--profile-id", profileId,
            "--profile-name", profileName,
            "--render-scale", Integer.toString(renderScale),
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
        Uri document = resolveGameDataDocument(relativePath);
        if (document == null) {
            return -1;
        }

        try (ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(document, "r")) {
            return descriptor == null ? -1 : descriptor.detachFd();
        } catch (IOException | RuntimeException exception) {
            return -1;
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
