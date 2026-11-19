/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * SettingsActivity.java — camex connection settings in brass: one SETTINGS window, the
 * section CAMEX built from the tunnel library's fields (CamexTunnel.fields()), brass Cancel
 * and Save (brand brass 1.3.0, the interface review of 2026-11-18)
 */
package org.openipc.camex;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.LayerDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.text.TextUtils;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.openipc.camex.tunnel.CamexTunnel;
import org.openipc.camex.tunnel.ConnectLink;
import org.openipc.camex.tunnel.Field;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The settings, kept by {@link Prefs} and handed to the tunnel library at Start. Fields, their
 * order, labels, hints and the client name's default come from the library only. Also opened
 * by a camex link (the library's {@link ConnectLink}, #146): its values are filled in and
 * marked, nothing is saved before Save.
 */
public class SettingsActivity extends Activity {
    // The overlay of a field a link changed (green, light)
    private static final int LINK_MARK = 0x332E7D32;
    // Before API 23: a marked field that had no background of its own
    private static final Object NO_BACKGROUND = new Object();
    private static final String STATE_VALUES = "values";
    private static final String STATE_NOTE = "link_note";
    private static final String STATE_MARKED = "link_marked";
    private static final String STATE_FOCUS = "focus";
    private static final String STATE_SELECTION = "selection";

    private final Map<String, Field> fields = new LinkedHashMap<>();
    private final Map<String, View> inputs = new HashMap<>();
    private final Map<String, Map<String, RadioButton>> choices = new HashMap<>();
    // The keys of the fields a link changed
    private final ArrayList<String> marked = new ArrayList<>();
    private TextView noteView;
    private ScrollView scroll;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        for (Field f : CamexTunnel.fields(this, Prefs.APP)) {
            // camex's own app has no web page: no port for nginx
            if (!CamexTunnel.WEB_PORT.equals(f.key)) fields.put(f.key, f);
        }
        build();

        if (savedInstanceState == null) {
            show(Prefs.load(this));
            Uri data = getIntent().getData();
            if (data != null) fromLink(data.toString());
        } else {
            // After a rotation: what was typed, the link's note and marks
            @SuppressWarnings("unchecked")
            HashMap<String, String> v = (HashMap<String, String>) savedInstanceState.getSerializable(STATE_VALUES);
            show(v != null ? v : Prefs.load(this));
            CharSequence note = savedInstanceState.getCharSequence(STATE_NOTE);
            if (note != null) showNote(note);
            ArrayList<String> keys = savedInstanceState.getStringArrayList(STATE_MARKED);
            if (keys != null) for (String k : keys) mark(k);
            // the field being edited, its cursor and the keyboard as they were
            View focus = inputs.get(savedInstanceState.getString(STATE_FOCUS, ""));
            if (focus instanceof EditText) {
                EditText e = (EditText) focus;
                int sel = Math.min(savedInstanceState.getInt(STATE_SELECTION, 0), e.length());
                e.requestFocus();
                e.setSelection(Math.max(sel, 0));
                getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE
                    | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
            }
        }
    }

    /** The window: SETTINGS, a note of a link, the section CAMEX, Cancel and Save. */
    private void build() {
        int pad = Brass.dp(this, 16);
        LinearLayout frame = Brass.panel(this);
        TextView title = Brass.title(this, getString(R.string.settings_title));
        title.setTextSize(20);
        frame.addView(title);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(Brass.dp(this, 4), 0, Brass.dp(this, 4), 0);
        noteView = Brass.display(this);
        noteView.setTextColor(Brass.color(this, R.color.amber));
        noteView.setVisibility(View.GONE);
        LinearLayout.LayoutParams np = new LinearLayout.LayoutParams(-1, -2);
        np.setMargins(0, Brass.dp(this, 8), 0, 0);
        box.addView(noteView, np);
        Brass.section(box, this, getString(R.string.section_camex), false);
        for (Field f : fields.values()) box.addView(field(f));

        ScrollView sv = new ScrollView(this);
        sv.addView(box);
        // opens at the top (a link's note first), the keyboard only when a field is tapped
        // (windowSoftInputMode in the manifest); the window, not its first field, takes the
        // first focus
        box.setDescendantFocusability(ViewGroup.FOCUS_BEFORE_DESCENDANTS);
        box.setFocusableInTouchMode(true);
        scroll = sv;
        frame.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));
        LinearLayout actions = new LinearLayout(this);
        Button cancel = Brass.button(this, R.string.cancel);
        Button save = Brass.button(this, R.string.save);
        actions.addView(cancel, Brass.margins(this, 0, -2, 1));
        actions.addView(save, Brass.margins(this, 0, -2, 1));
        frame.addView(actions);
        cancel.setOnClickListener(v -> finish());
        save.setOnClickListener(v -> save());

        LinearLayout page = new LinearLayout(this);
        page.setPadding(pad, pad, pad, pad);
        page.addView(frame, new LinearLayout.LayoutParams(-1, -1));
        setContentView(page);
    }

    /** A field as the library describes it: its label, then its input. */
    private View field(Field f) {
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        TextView label = new TextView(this);
        label.setText(f.label);
        label.setTextColor(Brass.color(this, R.color.dim));
        label.setPadding(0, Brass.dp(this, 10), 0, 0);
        col.addView(label);
        if (f.kind == Field.Kind.CHOICE) {
            RadioGroup g = new RadioGroup(this);
            g.setOrientation(RadioGroup.HORIZONTAL);
            Map<String, RadioButton> buttons = new HashMap<>();
            for (String c : f.choices) {
                RadioButton r = new RadioButton(this);
                r.setText(c);
                r.setId(View.generateViewId());
                r.setSaveEnabled(false);   // the activity keeps the values itself
                g.addView(r);
                buttons.put(c, r);
            }
            choices.put(f.key, buttons);
            inputs.put(f.key, g);
            col.addView(g);
            return col;
        }
        EditText e = new EditText(this);
        e.setSingleLine(true);
        e.setTypeface(Typeface.MONOSPACE);
        e.setSaveEnabled(false);
        // no autofill service sees the settings, the psk least of all (as the old layout)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            e.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        if (!f.hint.isEmpty()) e.setHint(f.hint);
        if (CamexTunnel.MTU.equals(f.key) || CamexTunnel.KEEPALIVE.equals(f.key)
                || CamexTunnel.SERVER_TIMEOUT.equals(f.key)) {
            e.setInputType(InputType.TYPE_CLASS_NUMBER);
        } else if (f.kind == Field.Kind.HOST_PORT) {
            e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        }
        inputs.put(f.key, e);
        col.addView(e);
        if (f.kind == Field.Kind.SECRET) {
            // hidden, with "Show the key" under it, the same in every app on camex
            e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
            e.setTypeface(Typeface.MONOSPACE);
            CheckBox show = new CheckBox(this);
            show.setText(Field.SHOW_SECRET);
            show.setSaveEnabled(false);
            show.setOnCheckedChangeListener((b, on) -> {
                int sel = e.getSelectionEnd();
                e.setInputType(InputType.TYPE_CLASS_TEXT | (on
                    ? InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                    : InputType.TYPE_TEXT_VARIATION_PASSWORD));
                e.setTypeface(Typeface.MONOSPACE);
                e.setSelection(Math.max(sel, 0));
            });
            col.addView(show, new LinearLayout.LayoutParams(-2, -2));
        }
        return col;
    }

    private String get(String key) {
        View v = inputs.get(key);
        if (v instanceof RadioGroup) {
            for (Map.Entry<String, RadioButton> c : choices.get(key).entrySet())
                if (c.getValue().isChecked()) return c.getKey();
            return fields.get(key).initial;
        }
        String s = ((EditText) v).getText() == null ? "" : ((EditText) v).getText().toString();
        return CamexTunnel.PSK.equals(key) ? s : s.trim();
    }

    private void set(String key, String value) {
        View v = inputs.get(key);
        if (v == null) return;
        if (v instanceof RadioGroup) {
            RadioButton r = choices.get(key).get(value);
            if (r == null) r = choices.get(key).get(fields.get(key).initial);
            if (r != null) ((RadioGroup) v).check(r.getId());
        } else {
            ((EditText) v).setText(value == null ? "" : value);
        }
    }

    private void show(Map<String, String> v) {
        for (String key : fields.keySet()) set(key, v.get(key));
    }

    private HashMap<String, String> values() {
        HashMap<String, String> v = new HashMap<>();
        for (String key : fields.keySet()) v.put(key, get(key));
        return v;
    }

    /*
     * One settings screen (singleTask): a link opened while it is up comes
     * here, so no older screen is left behind to save stale values over it
     */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        Uri data = intent.getData();
        if (data == null) return;
        // A link with nothing usable leaves what is on the screen as it is
        ConnectLink c = ConnectLink.parse(data.toString());
        if (c == null || (c.values.isEmpty() && c.port == null)) {
            fromLink(data.toString());   // only its note
            if (!marked.isEmpty()) {
                // The marks on the screen are an earlier link's, not this one's
                showNote(noteView.getText() + "\n" + getString(R.string.link_earlier));
            }
            return;
        }
        // The saved settings again, the marks of an earlier link gone: only
        // this link's values over them
        for (String key : marked) unmark(inputs.get(key));
        marked.clear();
        show(Prefs.load(this));
        fromLink(data.toString());
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putSerializable(STATE_VALUES, values());
        if (noteView.getVisibility() == View.VISIBLE) out.putCharSequence(STATE_NOTE, noteView.getText());
        out.putStringArrayList(STATE_MARKED, marked);
        View focus = getCurrentFocus();
        for (Map.Entry<String, View> i : inputs.entrySet()) {
            if (i.getValue() == focus && focus instanceof EditText) {
                out.putString(STATE_FOCUS, i.getKey());
                out.putInt(STATE_SELECTION, ((EditText) focus).getSelectionEnd());
            }
        }
    }

    /** A link's values over the saved ones: the changed fields marked. */
    private void fromLink(String link) {
        ConnectLink c = ConnectLink.parse(link);
        if (c == null) {
            showNote(getString(R.string.link_unknown));
            return;
        }
        List<String> changed = new ArrayList<>();
        // Host and port each only when the link has it: the saved other one stays
        if (c.host != null || c.port != null) {
            String cur = get(CamexTunnel.SERVER);
            int colon = cur.lastIndexOf(':');
            String host = c.host != null ? c.host : colon > 0 ? cur.substring(0, colon) : cur;
            String port = c.port != null ? String.valueOf(c.port)
                : colon > 0 ? cur.substring(colon + 1) : String.valueOf(Prefs.DEFAULT_PORT);
            // a port alone with no host yet: shown as ":port", the host to be typed before it
            take(CamexTunnel.SERVER, host + ":" + port, changed);
        }
        for (String key : new String[]{CamexTunnel.NAME, CamexTunnel.TRANSPORT, CamexTunnel.PUBKEY,
                CamexTunnel.PSK, CamexTunnel.MTU, CamexTunnel.KEEPALIVE}) {
            String value = c.values.get(key);
            if (value != null) take(key, value, changed);
        }
        StringBuilder note = new StringBuilder();
        // Nothing came through (a cut link, every field malformed): not
        // "the settings saved already"
        note.append(c.values.isEmpty() && c.port == null ? getString(R.string.link_unknown)
            : changed.isEmpty() ? getString(R.string.link_same)
            : getString(R.string.link_changed, TextUtils.join(", ", changed)));
        if (!c.errors.isEmpty()) {
            List<String> bad = new ArrayList<>();
            for (String letter : c.errors.keySet()) bad.add(label(letter));
            note.append('\n').append(getString(R.string.link_rejected, TextUtils.join(", ", bad)));
        }
        showNote(note);
    }

    private void take(String key, String value, List<String> changed) {
        if (!fields.containsKey(key) || value.equals(get(key))) return;
        set(key, value);
        mark(key);
        changed.add(fields.get(key).label);
    }

    /** The label of a link's field (its letter), as the screen shows it. */
    private String label(String letter) {
        String key;
        switch (letter) {
            case "h": case "p": key = CamexTunnel.SERVER; break;
            case "t": key = CamexTunnel.TRANSPORT; break;
            case "k": key = CamexTunnel.PUBKEY; break;
            case "s": key = CamexTunnel.PSK; break;
            case "n": key = CamexTunnel.NAME; break;
            case "m": key = CamexTunnel.MTU; break;
            case "a": key = CamexTunnel.KEEPALIVE; break;
            default: return letter;
        }
        return fields.containsKey(key) ? fields.get(key).label : letter;
    }

    private void showNote(CharSequence text) {
        noteView.setText(text);
        noteView.setVisibility(View.VISIBLE);
        scroll.post(() -> scroll.scrollTo(0, 0));   // the note first
    }

    private void mark(String key) {
        View field = inputs.get(key);
        if (field == null) return;
        if (key.equals(CamexTunnel.TRANSPORT)) {
            RadioButton r = choices.get(key).get(get(key));
            if (r != null) field = r;
        }
        mark(field);
        if (!marked.contains(key)) marked.add(key);
    }

    // A field a link changed: a light overlay of its own (the theme's tints
    // stay as they are, so removing it leaves the field as it was). A view's
    // foreground is API 23; before it the overlay is layered over the field's
    // own background, which is put back as it was (#158)
    private void mark(View field) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            field.setForeground(new ColorDrawable(LINK_MARK));
        } else if (field.getTag(R.id.link_mark) == null) {
            Drawable own = field.getBackground();
            field.setTag(R.id.link_mark, own != null ? own : NO_BACKGROUND);
            field.setBackground(own != null
                ? new LayerDrawable(new Drawable[]{own, new ColorDrawable(LINK_MARK)})
                : new ColorDrawable(LINK_MARK));
        }
    }

    private void unmark(View field) {
        if (field == null) return;
        if (field instanceof RadioGroup) {
            for (int i = 0; i < ((RadioGroup) field).getChildCount(); i++)
                unmark(((RadioGroup) field).getChildAt(i));
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            field.setForeground(null);
            return;
        }
        Object own = field.getTag(R.id.link_mark);
        if (own == null) return;
        field.setBackground(own == NO_BACKGROUND ? null : (Drawable) own);
        field.setTag(R.id.link_mark, null);
    }

    private void save() {
        HashMap<String, String> v = values();
        // an empty client name is not saved: the default again
        if (v.get(CamexTunnel.NAME).isEmpty()) {
            v.put(CamexTunnel.NAME, CamexTunnel.defaultName(this, Prefs.APP));
            set(CamexTunnel.NAME, v.get(CamexTunnel.NAME));
        }
        String[] bad = CamexTunnel.check(v);
        if (bad != null) {
            View field = inputs.get(bad[0]);
            String why = CamexTunnel.PUBKEY.equals(bad[0]) ? getString(R.string.err_server_pubkey)
                : CamexTunnel.PSK.equals(bad[0]) ? getString(R.string.err_psk) : bad[1];
            if (field instanceof EditText) {
                ((EditText) field).setError(why);
                field.requestFocus();
            } else {
                Toast.makeText(this, label(bad[0]) + ": " + why, Toast.LENGTH_LONG).show();
            }
            return;
        }
        Prefs.save(this, v);
        TunnelLog.add("settings saved");
        // From a link the screen opened by itself, even with the tunnel up
        // (the settings go to the tunnel at Start)
        Toast.makeText(this, getIntent().getData() != null ? R.string.saved_next_start
            : R.string.saved, Toast.LENGTH_LONG).show();
        finish();
    }
}
