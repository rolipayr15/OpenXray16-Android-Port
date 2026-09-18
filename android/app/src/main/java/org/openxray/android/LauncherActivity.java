package org.openxray.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.content.res.ColorStateList;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;

public final class LauncherActivity extends Activity {
    public static final String ACTION_RUN_PROFILE = "org.openxray.android.RUN_PROFILE";
    public static final String EXTRA_PROFILE_ID = "org.openxray.android.PROFILE_ID";

    private static final int SELECT_GAME_DATA_REQUEST = 1001;
    private static final String PREFERENCES = "openxray_launcher";
    private static final String LEGACY_GAME_DATA_URI = "game_data_uri";
    private static final String PROFILE_IDS = "profile_ids";
    private static final String SELECTED_PROFILE = "selected_profile";
    private static final String PROFILE_PREFIX = "profile.";
    private static final int[] RENDER_SCALES = {100, 80, 67, 50};
    private static final String[] RENDER_SCALE_LABELS = {
        "Исходное Качество (100%)", "Высоко (80%)", "Баланс (66%)", "Производительность (50%)"
    };

    private final List<Profile> profiles = new ArrayList<>();
    private SharedPreferences preferences;
    private Spinner profileSpinner;
    private EditText profileName;
    private TextView selectedDirectory;
    private EditText launchArguments;
    private Spinner resolutionSpinner;
    private Button launchButton;
    private Button deleteButton;
    private Profile activeProfile;
    private boolean updatingProfileSpinner;

    private static final class Profile {
        String id;
        String name;
        String gameDataUri;
        String arguments;
        int renderScale;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE);
        loadProfiles();
        setContentView(createContentView());
        refreshProfileSpinner(preferences.getString(SELECTED_PROFILE, profiles.get(0).id));
        maybeLaunchShortcut(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        maybeLaunchShortcut(intent);
    }

    private ScrollView createContentView() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(22), dp(20), dp(28));
        content.setBackgroundColor(Color.rgb(9, 14, 11));

        content.addView(label("v0.7.1", 12, Color.rgb(127, 181, 135), true), matchWidth());
        TextView title = label("OpenXRdroid16", 31, Color.rgb(238, 244, 239), true);
        title.setPadding(0, dp(4), 0, 0);
        content.addView(title, matchWidth());
        TextView subtitle = label("Порт от BardOwOeSaLo/r15. В случае багов, вылетов, иных проблем с творением сея, снимайте логи, и пишите на форум.", 15,
            Color.rgb(166, 181, 169), false);
        subtitle.setPadding(0, dp(6), 0, dp(22));
        content.addView(subtitle, matchWidth());

        LinearLayout profileCard = card();
        profileCard.addView(sectionTitle("профиль запуска"), matchWidth());
        profileSpinner = new Spinner(this);
        profileSpinner.setBackgroundTintList(ColorStateList.valueOf(Color.rgb(103, 145, 111)));
        profileSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                if (!updatingProfileSpinner && position >= 0 && position < profiles.size()) {
                    saveEditorToActiveProfile(false);
                    selectProfile(profiles.get(position));
                }
            }
        });
        profileCard.addView(profileSpinner, matchWidth());

        LinearLayout profileActions = row();
        Button create = button("Создать новый профиль", false);
        create.setOnClickListener(view -> createProfile(false));
        profileActions.addView(create, weighted());
        Button duplicate = button("Копировать профиль", false);
        duplicate.setOnClickListener(view -> createProfile(true));
        profileActions.addView(duplicate, weightedWithStartMargin());
        deleteButton = button("Удалить профиль", false);
        deleteButton.setOnClickListener(view -> confirmDeleteProfile());
        profileActions.addView(deleteButton, weightedWithStartMargin());
        profileCard.addView(profileActions, matchWidthWithTopMargin(10));

        profileName = edit("Название профиля");
        profileCard.addView(fieldLabel("Название"), matchWidthWithTopMargin(18));
        profileCard.addView(profileName, matchWidth());
        profileCard.addView(fieldLabel("Игровые файлы"), matchWidthWithTopMargin(16));
        selectedDirectory = label("Каталог не выбран", 13, Color.rgb(179, 194, 181), false);
        selectedDirectory.setTextIsSelectable(true);
        selectedDirectory.setPadding(dp(12), dp(12), dp(12), dp(12));
        selectedDirectory.setBackground(roundRect(Color.rgb(24, 33, 27), 12, Color.rgb(50, 68, 55)));
        profileCard.addView(selectedDirectory, matchWidth());
        Button selectDirectory = button("Выбрать каталог ресурсов", false);
        selectDirectory.setOnClickListener(view -> selectGameDataDirectory());
        profileCard.addView(selectDirectory, matchWidthWithTopMargin(8));

        profileCard.addView(fieldLabel("Внутреннее разрешение"), matchWidthWithTopMargin(16));
        resolutionSpinner = new Spinner(this);
        resolutionSpinner.setAdapter(new ArrayAdapter<>(this,
            android.R.layout.simple_spinner_dropdown_item, RENDER_SCALE_LABELS));
        resolutionSpinner.setBackgroundTintList(ColorStateList.valueOf(Color.rgb(103, 145, 111)));
        profileCard.addView(resolutionSpinner, matchWidth());
        TextView resolutionHint = label(
            "Меньший процент заметно разгружает GPU",
            12, Color.rgb(130, 148, 134), false);
        resolutionHint.setPadding(0, dp(7), 0, 0);
        profileCard.addView(resolutionHint, matchWidth());

        profileCard.addView(fieldLabel("параметры запуска"), matchWidthWithTopMargin(16));
        launchArguments = edit("Параметры запуска");
        launchArguments.setSingleLine(false);
        launchArguments.setMinLines(2);
        launchArguments.setGravity(Gravity.TOP);
        profileCard.addView(launchArguments, matchWidth());
        content.addView(profileCard, matchWidth());

        launchButton = button("Запустить", true);
        launchButton.setOnClickListener(view -> {
            if (saveEditorToActiveProfile(true))
                launchProfile(activeProfile, false);
        });
        content.addView(launchButton, matchWidthWithTopMargin(16));
        Button shortcut = button("Создать ярлык профиля", false);
        shortcut.setOnClickListener(view -> {
            if (saveEditorToActiveProfile(true))
                requestProfileShortcut(activeProfile);
        });
        content.addView(shortcut, matchWidthWithTopMargin(10));

        TextView isolation = label(
            "",
            12, Color.rgb(124, 142, 128), false);
        isolation.setGravity(Gravity.CENTER);
        isolation.setPadding(dp(8), dp(18), dp(8), 0);
        content.addView(isolation, matchWidth());

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(content);
        return scroll;
    }

    private void loadProfiles() {
        profiles.clear();
        Set<String> ids = new HashSet<>(preferences.getStringSet(PROFILE_IDS, Collections.emptySet()));
        if (ids.isEmpty()) {
            Profile profile = new Profile();
            profile.id = UUID.randomUUID().toString();
            profile.name = "Основной";
            profile.gameDataUri = preferences.getString(LEGACY_GAME_DATA_URI, "");
            profile.arguments = "";
            profile.renderScale = 67;
            profiles.add(profile);
            saveProfile(profile);
            saveProfileIds();
            preferences.edit().putString(SELECTED_PROFILE, profile.id).apply();
            return;
        }
        for (String id : ids) {
            Profile profile = readProfile(id);
            if (profile != null)
                profiles.add(profile);
        }
        if (profiles.isEmpty()) {
            preferences.edit().remove(PROFILE_IDS).apply();
            loadProfiles();
            return;
        }
        sortProfiles();
    }

    private Profile readProfile(String id) {
        String prefix = PROFILE_PREFIX + id + ".";
        String name = preferences.getString(prefix + "name", null);
        if (name == null)
            return null;
        Profile profile = new Profile();
        profile.id = id;
        profile.name = name;
        profile.gameDataUri = preferences.getString(prefix + "uri", "");
        profile.arguments = preferences.getString(prefix + "args", "");
        profile.renderScale = preferences.getInt(prefix + "scale", 67);
        return profile;
    }

    private void saveProfile(Profile profile) {
        String prefix = PROFILE_PREFIX + profile.id + ".";
        preferences.edit().putString(prefix + "name", profile.name)
            .putString(prefix + "uri", profile.gameDataUri)
            .putString(prefix + "args", profile.arguments)
            .putInt(prefix + "scale", profile.renderScale).apply();
    }

    private void saveProfileIds() {
        Set<String> ids = new HashSet<>();
        for (Profile profile : profiles)
            ids.add(profile.id);
        preferences.edit().putStringSet(PROFILE_IDS, ids).apply();
    }

    private void sortProfiles() {
        Collections.sort(profiles, (left, right) -> String.CASE_INSENSITIVE_ORDER.compare(left.name, right.name));
    }

    private void refreshProfileSpinner(String selectedId) {
        sortProfiles();
        List<String> names = new ArrayList<>();
        int selectedIndex = 0;
        for (int index = 0; index < profiles.size(); ++index) {
            names.add(profiles.get(index).name);
            if (profiles.get(index).id.equals(selectedId))
                selectedIndex = index;
        }
        updatingProfileSpinner = true;
        profileSpinner.setAdapter(new ArrayAdapter<>(this,
            android.R.layout.simple_spinner_dropdown_item, names));
        profileSpinner.setSelection(selectedIndex);
        updatingProfileSpinner = false;
        selectProfile(profiles.get(selectedIndex));
    }

    private void selectProfile(Profile profile) {
        activeProfile = profile;
        preferences.edit().putString(SELECTED_PROFILE, profile.id).apply();
        profileName.setText(profile.name);
        launchArguments.setText(profile.arguments);
        selectedDirectory.setText(profile.gameDataUri.isEmpty() ? "Каталог не выбран" : readableUri(profile.gameDataUri));
        resolutionSpinner.setSelection(scaleIndex(profile.renderScale));
        launchButton.setEnabled(hasPersistedReadPermission(profile.gameDataUri));
        deleteButton.setEnabled(profiles.size() > 1);
    }

    private boolean saveEditorToActiveProfile(boolean showErrors) {
        if (activeProfile == null)
            return false;
        String name = profileName.getText().toString().trim();
        if (name.isEmpty()) {
            if (showErrors)
                Toast.makeText(this, "Введите название профиля", Toast.LENGTH_LONG).show();
            return false;
        }
        activeProfile.name = name;
        activeProfile.arguments = launchArguments.getText().toString().trim();
        activeProfile.renderScale = RENDER_SCALES[resolutionSpinner.getSelectedItemPosition()];
        saveProfile(activeProfile);
        saveProfileIds();
        return true;
    }

    private void createProfile(boolean copyCurrent) {
        saveEditorToActiveProfile(false);
        Profile profile = new Profile();
        profile.id = UUID.randomUUID().toString();
        profile.name = copyCurrent && activeProfile != null ? activeProfile.name + "- копирка" : "Новый профиль";
        profile.gameDataUri = copyCurrent && activeProfile != null ? activeProfile.gameDataUri : "";
        profile.arguments = copyCurrent && activeProfile != null ? activeProfile.arguments : "";
        profile.renderScale = copyCurrent && activeProfile != null ? activeProfile.renderScale : 67;
        profiles.add(profile);
        saveProfile(profile);
        saveProfileIds();
        refreshProfileSpinner(profile.id);
        profileName.requestFocus();
        profileName.selectAll();
    }

    private void confirmDeleteProfile() {
        if (activeProfile == null || profiles.size() <= 1)
            return;
        new AlertDialog.Builder(this).setTitle("Удалить профиль?")
            .setMessage("Настройки профиля будут удалены из лаунчера. Игровые сохранения останутся в префах приложения.")
            .setNegativeButton("Отмена", null)
            .setPositiveButton("Удалить", (dialog, which) -> deleteActiveProfile()).show();
    }

    private void deleteActiveProfile() {
        String prefix = PROFILE_PREFIX + activeProfile.id + ".";
        preferences.edit().remove(prefix + "name").remove(prefix + "uri")
            .remove(prefix + "args").remove(prefix + "scale").apply();
        profiles.remove(activeProfile);
        saveProfileIds();
        refreshProfileSpinner(profiles.get(0).id);
    }

    private void selectGameDataDirectory() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
            Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        startActivityForResult(intent, SELECT_GAME_DATA_REQUEST);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != SELECT_GAME_DATA_REQUEST || resultCode != RESULT_OK || data == null || activeProfile == null)
            return;
        Uri selectedUri = data.getData();
        if (selectedUri == null)
            return;
        try {
            getContentResolver().takePersistableUriPermission(selectedUri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            activeProfile.gameDataUri = selectedUri.toString();
            saveProfile(activeProfile);
            selectProfile(activeProfile);
        } catch (SecurityException exception) {
            Toast.makeText(this, "Android не предоставил постоянный доступ к каталогу", Toast.LENGTH_LONG).show();
        }
    }

    private boolean hasPersistedReadPermission(String value) {
        if (value == null || value.isEmpty())
            return false;
        Uri uri = Uri.parse(value);
        for (UriPermission permission : getContentResolver().getPersistedUriPermissions())
            if (permission.isReadPermission() && permission.getUri().equals(uri))
                return true;
        return false;
    }

    private void launchProfile(Profile profile, boolean fromShortcut) {
        if (profile == null || !hasPersistedReadPermission(profile.gameDataUri)) {
            Toast.makeText(this, "Профилю нужен доступ к каталогу игровых файлов", Toast.LENGTH_LONG).show();
            return;
        }
        Intent intent = new Intent(this, OpenXRayActivity.class);
        intent.putExtra(OpenXRayActivity.EXTRA_GAME_DATA_URI, profile.gameDataUri);
        intent.putExtra(OpenXRayActivity.EXTRA_PROFILE_ID, profile.id);
        intent.putExtra(OpenXRayActivity.EXTRA_PROFILE_NAME, profile.name);
        intent.putExtra(OpenXRayActivity.EXTRA_ENGINE_ARGUMENTS, profile.arguments);
        intent.putExtra(OpenXRayActivity.EXTRA_RENDER_SCALE, profile.renderScale);
        startActivity(intent);
        if (fromShortcut)
            finish();
    }

    private void requestProfileShortcut(Profile profile) {
        ShortcutManager manager = getSystemService(ShortcutManager.class);
        if (manager == null || !manager.isRequestPinShortcutSupported()) {
            Toast.makeText(this, "Этот лаунчер Android не поддерживает закрепляемые ярлыки", Toast.LENGTH_LONG).show();
            return;
        }
        Intent intent = new Intent(this, LauncherActivity.class).setAction(ACTION_RUN_PROFILE)
            .setData(Uri.parse("openxray://profile/" + profile.id)).putExtra(EXTRA_PROFILE_ID, profile.id);
        ShortcutInfo shortcut = new ShortcutInfo.Builder(this, "profile-" + profile.id)
            .setShortLabel(profile.name).setLongLabel("OpenXRdroid16 — " + profile.name)
            .setIcon(Icon.createWithResource(this, R.drawable.ic_launcher)).setIntent(intent).build();
        if (manager.requestPinShortcut(shortcut, null))
            Toast.makeText(this, "Запрос на создание ярлыка отправлен", Toast.LENGTH_SHORT).show();
    }

    private void maybeLaunchShortcut(Intent intent) {
        if (intent == null || !ACTION_RUN_PROFILE.equals(intent.getAction()))
            return;
        String id = intent.getStringExtra(EXTRA_PROFILE_ID);
        for (Profile profile : profiles) {
            if (profile.id.equals(id)) {
                launchProfile(profile, true);
                return;
            }
        }
        Toast.makeText(this, "Профиль этого ярлыка больше не существует", Toast.LENGTH_LONG).show();
    }

    private String readableUri(String value) {
        String path = Uri.parse(value).getPath();
        return path == null || path.isEmpty() ? value : path.replace("/tree/primary:", "Внутренняя память / ");
    }

    private int scaleIndex(int value) {
        for (int index = 0; index < RENDER_SCALES.length; ++index)
            if (RENDER_SCALES[index] == value)
                return index;
        return 2;
    }

    private LinearLayout card() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(18), dp(18), dp(18), dp(18));
        layout.setBackground(roundRect(Color.rgb(19, 27, 22), 18, Color.rgb(43, 59, 48)));
        return layout;
    }

    private LinearLayout row() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        return layout;
    }

    private TextView sectionTitle(String text) {
        TextView view = label(text, 12, Color.rgb(126, 178, 134), true);
        view.setPadding(0, 0, 0, dp(10));
        return view;
    }

    private TextView fieldLabel(String text) {
        TextView view = label(text, 12, Color.rgb(146, 164, 149), true);
        view.setPadding(0, 0, 0, dp(5));
        return view;
    }

    private TextView label(String text, float size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold)
            view.setTypeface(Typeface.DEFAULT_BOLD);
        return view;
    }

    private EditText edit(String hint) {
        EditText view = new EditText(this);
        view.setHint(hint);
        view.setHintTextColor(Color.rgb(91, 108, 95));
        view.setTextColor(Color.rgb(233, 240, 234));
        view.setTextSize(15);
        view.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        view.setBackgroundTintList(ColorStateList.valueOf(Color.rgb(90, 124, 97)));
        view.setPadding(dp(4), dp(8), dp(4), dp(8));
        view.setSingleLine(true);
        return view;
    }

    private Button button(String text, boolean primary) {
        Button button = new Button(this);
        button.setText(text);
        button.setTextSize(primary ? 15 : 13);
        button.setTextColor(primary ? Color.rgb(8, 16, 10) : Color.rgb(220, 232, 222));
        button.setTypeface(Typeface.DEFAULT_BOLD);
        button.setAllCaps(false);
        button.setMinHeight(dp(primary ? 54 : 46));
        button.setBackground(roundRect(primary ? Color.rgb(124, 190, 132) : Color.rgb(34, 48, 39),
            primary ? 15 : 12, primary ? Color.rgb(155, 216, 162) : Color.rgb(63, 84, 68)));
        return button;
    }

    private GradientDrawable roundRect(int fill, int radius, int stroke) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(dp(radius));
        drawable.setStroke(dp(1), stroke);
        return drawable;
    }

    private LinearLayout.LayoutParams matchWidth() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams matchWidthWithTopMargin(int margin) {
        LinearLayout.LayoutParams params = matchWidth();
        params.topMargin = dp(margin);
        return params;
    }

    private LinearLayout.LayoutParams weighted() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
    }

    private LinearLayout.LayoutParams weightedWithStartMargin() {
        LinearLayout.LayoutParams params = weighted();
        params.leftMargin = dp(7);
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
