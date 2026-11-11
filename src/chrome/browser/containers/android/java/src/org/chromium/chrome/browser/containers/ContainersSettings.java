// Copyright 2026 The tennessine Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.containers;

import android.content.Context;
import android.os.Bundle;
import android.text.InputType;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.TextUtils;
import android.text.style.ForegroundColorSpan;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.appcompat.app.AlertDialog;
import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.PreferenceScreen;

import org.chromium.base.supplier.MonotonicObservableSupplier;
import org.chromium.base.supplier.ObservableSuppliers;
import org.chromium.base.supplier.SettableMonotonicObservableSupplier;
import org.chromium.build.annotations.NullMarked;
import org.chromium.build.annotations.Nullable;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.containers.ContainersBridge.Container;
import org.chromium.chrome.browser.containers.ContainersBridge.Rule;
import org.chromium.chrome.browser.settings.ChromeBaseSettingsFragment;
import org.chromium.components.browser_ui.settings.TextMessagePreference;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * Settings → Containers: the containers (add, rename, recolor, change the icon, delete), where new
 * tabs open, and Traffic Control's rules, as oganesson's Settings page has them.
 */
@NullMarked
public class ContainersSettings extends ChromeBaseSettingsFragment {
    /** Firefox's color names (dubna's container.h), in its order. */
    private static final String[] COLORS = {
        "blue", "turquoise", "green", "yellow", "orange", "red", "pink", "purple", "toolbar"
    };

    /** The icon choices' view ids, after the colors' (1 to 9). */
    private static final int ICON_ID_BASE = 100;

    /** A rule's container that no longer exists, as after a sync deleted it. */
    private static final String DELETED_CONTAINER = "Deleted container";

    /** Shown when sync changed or removed a rule while its dialog was open. */
    private static final String RULE_CHANGED =
            "This rule was changed by sync meanwhile. The list now shows the current rules.";

    private final SettableMonotonicObservableSupplier<String> mPageTitle =
            ObservableSuppliers.createMonotonic();

    @Override
    public void onCreatePreferences(@Nullable Bundle savedInstanceState, @Nullable String rootKey) {
        mPageTitle.set(getString(R.string.containers_title));
        setPreferenceScreen(getPreferenceManager().createPreferenceScreen(getStyledContext()));
        rebuild();
    }

    @Override
    public MonotonicObservableSupplier<String> getPageTitle() {
        return mPageTitle;
    }

    @Override
    public @AnimationType int getAnimationType() {
        return AnimationType.PROPERTY;
    }

    private Context getStyledContext() {
        return getPreferenceManager().getContext();
    }

    private void rebuild() {
        Context context = getStyledContext();
        PreferenceScreen screen = getPreferenceScreen();
        screen.removeAll();
        List<Container> containers = ContainersBridge.getContainers(getProfile());

        TextMessagePreference about = new TextMessagePreference(context, null);
        about.setSummary(
                "Each container keeps its own cookies and site data. A link from a container tab"
                        + " opens in the same container. To open a link in another one, long-press"
                        + " it and choose Open in container.");
        screen.addPreference(about);

        PreferenceCategory list = new PreferenceCategory(context);
        list.setTitle(R.string.containers_title);
        screen.addPreference(list);
        for (Container container : containers) {
            Preference pref = new Preference(context);
            pref.setTitle(container.name);
            pref.setIcon(
                    ContainerIcons.drawable(
                            container.icon,
                            container.argb,
                            Math.round(24 * getResources().getDisplayMetrics().density)));
            pref.setOnPreferenceClickListener(
                    p -> {
                        editContainer(container);
                        return true;
                    });
            list.addPreference(pref);
        }
        Preference add = new Preference(context);
        add.setTitle("Add container");
        add.setOnPreferenceClickListener(
                p -> {
                    editContainer(null);
                    return true;
                });
        list.addPreference(add);

        Preference newTabs = new Preference(context);
        newTabs.setTitle("Open new tabs in");
        newTabs.setSummary(targetName(ContainersBridge.getNewTabSetting(getProfile()), containers));
        newTabs.setOnPreferenceClickListener(
                p -> {
                    chooseNewTabContainer(containers);
                    return true;
                });
        list.addPreference(newTabs);

        PreferenceCategory traffic = new PreferenceCategory(context);
        traffic.setTitle("Traffic Control");
        screen.addPreference(traffic);
        TextMessagePreference trafficAbout = new TextMessagePreference(context, null);
        trafficAbout.setSummary(
                "Sites that match a rule open in its container, whichever tab they start in. The"
                        + " first rule that matches wins. A tab that browses within one site stays"
                        + " where it is, unless you type the address.");
        traffic.addPreference(trafficAbout);
        List<Rule> rules = ContainersBridge.getRules(getProfile());
        for (int i = 0; i < rules.size(); i++) {
            Rule rule = rules.get(i);
            int index = i;
            Preference pref = new Preference(context);
            pref.setTitle(TextUtils.join(", ", patterns(rule.urlFilter)));
            pref.setSummary(
                    targetName(rule.container, containers) + (rule.enabled ? "" : " (off)"));
            pref.setOnPreferenceClickListener(
                    p -> {
                        editRule(rule, index, containers);
                        return true;
                    });
            traffic.addPreference(pref);
        }
        Preference addRule = new Preference(context);
        addRule.setTitle("Add rule");
        addRule.setOnPreferenceClickListener(
                p -> {
                    editRule(null, -1, containers);
                    return true;
                });
        traffic.addPreference(addRule);
    }

    /** A rule's or the new tab setting's container, by name. */
    private String targetName(String id, List<Container> containers) {
        if (id.isEmpty()) return getString(R.string.containers_none);
        if (id.equals(ContainersBridge.TEMPORARY)) {
            return getString(R.string.containers_new_temporary);
        }
        for (Container container : containers) {
            if (container.id.equals(id)) return container.name;
        }
        return DELETED_CONTAINER;
    }

    /**
     * Where {@code shown}, rule {@code index} when this screen was built, is in the rules now:
     * sync may have changed them since. -1 when it is gone or changed.
     */
    private int findRule(Rule shown, int index) {
        List<Rule> rules = ContainersBridge.getRules(getProfile());
        if (index < rules.size() && sameRule(rules.get(index), shown)) return index;
        for (int i = 0; i < rules.size(); i++) {
            if (sameRule(rules.get(i), shown)) return i;
        }
        return -1;
    }

    private static boolean sameRule(Rule a, Rule b) {
        return a.urlFilter.equals(b.urlFilter)
                && a.container.equals(b.container)
                && a.enabled == b.enabled;
    }

    private void showError(String error) {
        new AlertDialog.Builder(getStyledContext())
                .setMessage(error)
                .setPositiveButton(android.R.string.ok, null)
                .show();
    }

    /** A rule's patterns: its lines without blank ones and # comments. */
    private static List<String> patterns(String urlFilter) {
        List<String> patterns = new ArrayList<>();
        for (String line : urlFilter.split("\n")) {
            String pattern = line.trim();
            if (!pattern.isEmpty() && !pattern.startsWith("#")) patterns.add(pattern);
        }
        return patterns;
    }

    /** {@code name} after a dot in color {@code argb}. */
    private static CharSequence withDot(String name, int argb) {
        SpannableString text = new SpannableString("●  " + name);
        text.setSpan(new ForegroundColorSpan(argb), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        return text;
    }

    /** The choices of a rule's or the new tab setting's container: id and label. */
    private void addTargets(
            List<Container> containers, List<String> ids, List<CharSequence> labels) {
        ids.add("");
        labels.add(getString(R.string.containers_none));
        for (Container container : containers) {
            ids.add(container.id);
            labels.add(
                    ContainerIcons.withIcon(
                            getStyledContext(), container.name, container.icon, container.argb));
        }
        ids.add(ContainersBridge.TEMPORARY);
        labels.add(getString(R.string.containers_new_temporary));
    }

    private void chooseNewTabContainer(List<Container> containers) {
        List<String> ids = new ArrayList<>();
        List<CharSequence> labels = new ArrayList<>();
        addTargets(containers, ids, labels);
        new AlertDialog.Builder(getStyledContext())
                .setTitle("Open new tabs in")
                .setSingleChoiceItems(
                        labels.toArray(new CharSequence[0]),
                        Math.max(0, ids.indexOf(ContainersBridge.getNewTabSetting(getProfile()))),
                        (dialog, which) -> {
                            ContainersBridge.setNewTabSetting(getProfile(), ids.get(which));
                            dialog.dismiss();
                            rebuild();
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    /**
     * A dialog with a container's name, color and icon; a new container when {@code edited} is
     * null.
     */
    private void editContainer(@Nullable Container edited) {
        Context context = getStyledContext();
        int padding = Math.round(20 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(padding, padding / 2, padding, 0);
        EditText name = new EditText(context);
        name.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        name.setHint("Name");
        layout.addView(name);
        RadioGroup colors = new RadioGroup(context);
        for (int i = 0; i < COLORS.length; i++) {
            RadioButton button = new RadioButton(context);
            button.setId(i + 1);
            button.setText(withDot(COLORS[i], ContainersBridge.colorValue(COLORS[i])));
            colors.addView(button);
        }
        layout.addView(colors);
        TextView iconTitle = new TextView(context);
        iconTitle.setText("Icon");
        iconTitle.setPadding(0, padding / 2, 0, 0);
        layout.addView(iconTitle);
        // In the container's color as the dialog opens.
        String[] icons = ContainerIcons.names();
        int iconArgb = edited != null ? edited.argb : ContainersBridge.colorValue(COLORS[0]);
        RadioGroup iconGroup = new RadioGroup(context);
        for (int i = 0; i < icons.length; i++) {
            RadioButton button = new RadioButton(context);
            button.setId(ICON_ID_BASE + i);
            button.setText(ContainerIcons.withIcon(context, icons[i], icons[i], iconArgb));
            iconGroup.addView(button);
        }
        layout.addView(iconGroup);
        ScrollView scroll = new ScrollView(context);
        scroll.addView(layout);

        String color = edited != null ? edited.color : COLORS[0];
        int checked = 0;
        for (int i = 0; i < COLORS.length; i++) {
            if (COLORS[i].equals(color)) checked = i;
        }
        colors.check(checked + 1);
        String icon = edited != null ? edited.icon : "circle";
        int checkedIcon = 0;
        for (int i = 0; i < icons.length; i++) {
            if (icons[i].equals(icon)) checkedIcon = i;
        }
        iconGroup.check(ICON_ID_BASE + checkedIcon);
        if (edited != null) name.setText(edited.name);

        AlertDialog.Builder builder =
                new AlertDialog.Builder(context)
                        .setTitle(edited != null ? "Edit container" : "Add container")
                        .setView(scroll)
                        .setPositiveButton("Save", null)
                        .setNegativeButton(android.R.string.cancel, null);
        if (edited != null) addDeleteButton(builder, edited);
        AlertDialog dialog = builder.create();
        dialog.setOnShowListener(
                d ->
                        dialog.getButton(AlertDialog.BUTTON_POSITIVE)
                                .setOnClickListener(
                                        v -> {
                                            String text = name.getText().toString().trim();
                                            if (text.isEmpty()) {
                                                name.setError("Enter a name");
                                                return;
                                            }
                                            String newColor =
                                                    COLORS[colors.getCheckedRadioButtonId() - 1];
                                            int iconIndex =
                                                    iconGroup.getCheckedRadioButtonId()
                                                            - ICON_ID_BASE;
                                            saveContainer(
                                                    edited, text, newColor, icons[iconIndex]);
                                            dialog.dismiss();
                                        }));
        dialog.show();
    }

    private void addDeleteButton(AlertDialog.Builder builder, Container edited) {
        builder.setNeutralButton("Delete", (d, w) -> confirmDelete(edited));
    }

    private void saveContainer(
            @Nullable Container edited, String name, String color, String icon) {
        List<Container> containers = ContainersBridge.getContainers(getProfile());
        if (edited == null) {
            containers.add(
                    new Container(UUID.randomUUID().toString(), name, icon, color, 0));
        } else {
            for (Container container : containers) {
                if (container.id.equals(edited.id)) {
                    container.name = name;
                    container.color = color;
                    container.icon = icon;
                }
            }
        }
        ContainersBridge.setContainers(getProfile(), containers);
        rebuild();
    }

    private void confirmDelete(Container deleted) {
        new AlertDialog.Builder(getStyledContext())
                .setTitle("Delete " + deleted.name + "?")
                .setMessage(
                        "Its tabs close, and its cookies and site data are deleted. Its closed"
                                + " tabs are forgotten.")
                .setPositiveButton(
                        "Delete",
                        (d, w) -> {
                            List<Container> containers =
                                    ContainersBridge.getContainers(getProfile());
                            containers.removeIf(c -> c.id.equals(deleted.id));
                            ContainersBridge.setContainers(getProfile(), containers);
                            rebuild();
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    /**
     * A dialog with {@code shown}, rule {@code index} when this screen was built, or a new rule
     * when it is null. Each change applies to the rules as they are then, which sync may have
     * changed meanwhile.
     */
    private void editRule(@Nullable Rule shown, int index, List<Container> containers) {
        Context context = getStyledContext();
        Rule rule = shown != null ? shown : new Rule("", "", true);
        int padding = Math.round(20 * getResources().getDisplayMetrics().density);
        LinearLayout layout = new LinearLayout(context);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(padding, padding / 2, padding, 0);
        EditText filter = new EditText(context);
        filter.setInputType(
                InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_FLAG_MULTI_LINE
                        | InputType.TYPE_TEXT_VARIATION_URI);
        filter.setMinLines(2);
        filter.setHint("example.com\nhttps://*.example.org/path");
        filter.setText(rule.urlFilter);
        layout.addView(filter);
        List<String> ids = new ArrayList<>();
        List<CharSequence> labels = new ArrayList<>();
        addTargets(containers, ids, labels);
        if (!ids.contains(rule.container)) {
            // Kept as it is unless the user picks another one.
            ids.add(rule.container);
            labels.add(DELETED_CONTAINER);
        }
        RadioGroup targets = new RadioGroup(context);
        for (int i = 0; i < labels.size(); i++) {
            RadioButton button = new RadioButton(context);
            button.setId(i + 1);
            button.setText(labels.get(i));
            targets.addView(button);
        }
        targets.check(Math.max(0, ids.indexOf(rule.container)) + 1);
        layout.addView(targets);
        CheckBox enabled = new CheckBox(context);
        enabled.setText("On");
        enabled.setChecked(rule.enabled);
        layout.addView(enabled);
        ScrollView scroll = new ScrollView(context);
        scroll.addView(layout);

        AlertDialog.Builder builder =
                new AlertDialog.Builder(context)
                        .setTitle(shown != null ? "Edit rule" : "Add rule")
                        .setMessage(
                                "Sites, one per line: example.com, or a URL pattern such as"
                                        + " https://*.example.org/path. Lines that start with #"
                                        + " are comments.")
                        .setView(scroll)
                        .setPositiveButton("Save", null)
                        .setNegativeButton(android.R.string.cancel, null);
        if (shown != null) {
            builder.setNeutralButton(
                    "Delete",
                    (d, w) -> {
                        int current = findRule(shown, index);
                        if (current < 0) {
                            rebuild();
                            showError(RULE_CHANGED);
                            return;
                        }
                        List<Rule> kept = ContainersBridge.getRules(getProfile());
                        kept.remove(current);
                        String error = ContainersBridge.setRules(getProfile(), kept);
                        rebuild();
                        if (!error.isEmpty()) showError("The rule wasn't deleted: " + error);
                    });
        }
        AlertDialog dialog = builder.create();
        dialog.setOnShowListener(
                d ->
                        dialog.getButton(AlertDialog.BUTTON_POSITIVE)
                                .setOnClickListener(
                                        v -> {
                                            int current =
                                                    shown != null ? findRule(shown, index) : -1;
                                            if (shown != null && current < 0) {
                                                dialog.dismiss();
                                                rebuild();
                                                showError(RULE_CHANGED);
                                                return;
                                            }
                                            List<Rule> saved =
                                                    ContainersBridge.getRules(getProfile());
                                            Rule changed =
                                                    new Rule(
                                                            filter.getText().toString(),
                                                            ids.get(
                                                                    targets
                                                                                    .getCheckedRadioButtonId()
                                                                            - 1),
                                                            enabled.isChecked());
                                            if (current >= 0) {
                                                saved.set(current, changed);
                                            } else {
                                                saved.add(changed);
                                            }
                                            String error =
                                                    ContainersBridge.setRules(getProfile(), saved);
                                            if (!error.isEmpty()) {
                                                filter.setError(error);
                                                return;
                                            }
                                            dialog.dismiss();
                                            rebuild();
                                        }));
        dialog.show();
    }
}
