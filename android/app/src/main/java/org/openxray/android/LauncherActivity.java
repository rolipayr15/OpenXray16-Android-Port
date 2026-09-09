package org.openxray.android;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

public final class LauncherActivity extends Activity {
    private static final int SELECT_GAME_DATA_REQUEST = 1001;
    private static final String PREFERENCES = "openxray_launcher";
    private static final String GAME_DATA_URI = "game_data_uri";

    private TextView selectedDirectory;
    private Button launchButton;
    private Uri gameDataUri;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(createContentView());
        restoreSelection();
    }

    private ScrollView createContentView() {
        final int spacing = dp(24);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setGravity(Gravity.CENTER_HORIZONTAL);
        content.setPadding(spacing, spacing, spacing, spacing);

        TextView title = new TextView(this);
        title.setText(R.string.launcher_title);
        title.setTextSize(28);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        content.addView(title, matchWidth());

        TextView description = new TextView(this);
        description.setText(R.string.launcher_description);
        description.setTextSize(17);
        description.setPadding(0, dp(16), 0, dp(24));
        content.addView(description, matchWidth());

        selectedDirectory = new TextView(this);
        selectedDirectory.setTextSize(15);
        selectedDirectory.setTextIsSelectable(true);
        selectedDirectory.setPadding(0, 0, 0, dp(16));
        content.addView(selectedDirectory, matchWidth());

        Button selectButton = new Button(this);
        selectButton.setText(R.string.select_game_data);
        selectButton.setAllCaps(false);
        selectButton.setOnClickListener(view -> selectGameDataDirectory());
        content.addView(selectButton, matchWidth());

        launchButton = new Button(this);
        launchButton.setText(R.string.start_engine_host);
        launchButton.setAllCaps(false);
        launchButton.setOnClickListener(view -> startEngine());
        LinearLayout.LayoutParams launchLayout = matchWidth();
        launchLayout.topMargin = dp(12);
        content.addView(launchButton, launchLayout);

        TextView stageNotice = new TextView(this);
        stageNotice.setText(R.string.host_stage_notice);
        stageNotice.setTextSize(13);
        stageNotice.setPadding(0, dp(24), 0, 0);
        content.addView(stageNotice, matchWidth());

        ScrollView scrollView = new ScrollView(this);
        scrollView.addView(content);
        return scrollView;
    }

    private void restoreSelection() {
        SharedPreferences preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE);
        String savedUri = preferences.getString(GAME_DATA_URI, null);
        if (savedUri != null) {
            Uri candidate = Uri.parse(savedUri);
            if (hasPersistedReadPermission(candidate)) {
                gameDataUri = candidate;
            } else {
                preferences.edit().remove(GAME_DATA_URI).apply();
            }
        }
        updateSelectionState();
    }

    private boolean hasPersistedReadPermission(Uri uri) {
        for (UriPermission permission : getContentResolver().getPersistedUriPermissions()) {
            if (permission.isReadPermission() && permission.getUri().equals(uri)) {
                return true;
            }
        }
        return false;
    }

    private void selectGameDataDirectory() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(
            Intent.FLAG_GRANT_READ_URI_PERMISSION |
            Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
            Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
        );
        startActivityForResult(intent, SELECT_GAME_DATA_REQUEST);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != SELECT_GAME_DATA_REQUEST || resultCode != RESULT_OK || data == null) {
            return;
        }

        Uri selectedUri = data.getData();
        if (selectedUri == null) {
            return;
        }

        try {
            getContentResolver().takePersistableUriPermission(
                selectedUri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION
            );
            gameDataUri = selectedUri;
            getSharedPreferences(PREFERENCES, MODE_PRIVATE)
                .edit()
                .putString(GAME_DATA_URI, selectedUri.toString())
                .apply();
            updateSelectionState();
        } catch (SecurityException exception) {
            Toast.makeText(this, R.string.persistent_access_failed, Toast.LENGTH_LONG).show();
        }
    }

    private void startEngine() {
        if (gameDataUri == null || !hasPersistedReadPermission(gameDataUri)) {
            restoreSelection();
            Toast.makeText(this, R.string.select_readable_directory, Toast.LENGTH_LONG).show();
            return;
        }

        Intent intent = new Intent(this, OpenXRayActivity.class);
        intent.putExtra(OpenXRayActivity.EXTRA_GAME_DATA_URI, gameDataUri.toString());
        startActivity(intent);
    }

    private void updateSelectionState() {
        boolean selected = gameDataUri != null;
        selectedDirectory.setText(selected ? getString(R.string.selected_game_data, gameDataUri) : getString(R.string.no_game_data_selected));
        launchButton.setEnabled(selected);
    }

    private LinearLayout.LayoutParams matchWidth() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
