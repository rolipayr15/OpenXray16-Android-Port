package org.openxray.android;

import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;

import org.libsdl.app.SDLActivity;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class OpenXRayActivity extends SDLActivity {
    public static final String EXTRA_GAME_DATA_URI = "org.openxray.android.GAME_DATA_URI";

    private String gameDataUri = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        String selectedUri = getIntent().getStringExtra(EXTRA_GAME_DATA_URI);
        if (selectedUri != null) {
            gameDataUri = selectedUri;
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        return new String[] {
            "--game-data-uri", gameDataUri,
            "--app-files-path", getFilesDir().getAbsolutePath()
        };
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
