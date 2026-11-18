/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Brass.java — the brass look of camex's screens (brand brass 1.3.0, the
 * interface review of 2026-11-18): heading with the version, panel, display,
 * board rows with lamps, section bands and brass buttons, built in code like
 * the screens themselves. Colours come from res/values/colors.xml.
 */
package org.openipc.camex;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

final class Brass {
    private Brass() {
    }

    @SuppressWarnings("deprecation")   // getColor(int) of Resources: the only one before 6.0
    static int color(Context c, int id) {
        return Build.VERSION.SDK_INT >= 23 ? c.getColor(id) : c.getResources().getColor(id);
    }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                c.getResources().getDisplayMetrics());
    }

    /** The heading: brass serif capitals with a soft glow, centred. */
    static TextView title(Context c, CharSequence text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setAllCaps(true);
        t.setTextSize(26);
        t.setTypeface(Typeface.SERIF);
        t.setLetterSpacing(0.2f);
        t.setTextColor(color(c, R.color.brass));
        t.setShadowLayer(dp(c, 8), 0, 0, 0x59c9a24d);
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        return t;
    }

    /** The panel: a vertical column on the brass2-framed gradient. */
    static LinearLayout panel(Context c) {
        LinearLayout p = new LinearLayout(c);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setBackgroundResource(R.drawable.panel);
        int pad = dp(c, 18);
        p.setPadding(pad, pad, pad, pad);
        return p;
    }

    /** The display: mono text on black, dim until a state colours it. */
    static TextView display(Context c) {
        TextView t = new TextView(c);
        t.setBackgroundResource(R.drawable.display);
        t.setTypeface(Typeface.MONOSPACE);
        t.setTextColor(color(c, R.color.dim));
        int pad = dp(c, 12);
        t.setPadding(pad, pad, pad, pad);
        return t;
    }

    /** A lamp: a circle in the state colour, a thin #6e5320 rim. */
    static GradientDrawable lamp(Context c, int colorId) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.OVAL);
        d.setColor(color(c, colorId));
        d.setStroke(dp(c, 1), 0xff6e5320);
        return d;
    }

    /** The heading centred on the screen; the version number in small mono to its right. */
    static LinearLayout titleRow(Context c, CharSequence text, CharSequence version) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setBaselineAligned(true);
        // equal side columns: the title stays in the middle whatever the version's width
        r.addView(new android.view.View(c), new LinearLayout.LayoutParams(0, 1, 1));
        TextView t = title(c, text);
        r.addView(t, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        TextView v = new TextView(c);
        v.setText(version);
        v.setTypeface(Typeface.MONOSPACE);
        v.setTextSize(11);
        v.setTextColor(color(c, R.color.dim));
        v.setPadding(dp(c, 4), 0, 0, 0);
        r.addView(v, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return r;
    }

    /** One row of the board: lamp, label (dim mono caps), value (ink mono). */
    static final class Row {
        final LinearLayout view;
        final android.view.View lamp;
        final TextView label, value;

        Row(Context c, String l) {
            view = new LinearLayout(c);
            view.setOrientation(LinearLayout.HORIZONTAL);
            view.setGravity(Gravity.TOP);
            view.setPadding(0, dp(c, 3), 0, dp(c, 3));
            lamp = new android.view.View(c);
            int s = dp(c, 12);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(s, s);
            lp.setMargins(0, dp(c, 3), dp(c, 10), 0);
            view.addView(lamp, lp);
            label = new TextView(c);
            label.setText(l);
            label.setTypeface(Typeface.MONOSPACE);
            label.setTextSize(13);
            label.setLetterSpacing(0.08f);
            label.setTextColor(color(c, R.color.dim));
            view.addView(label, new LinearLayout.LayoutParams(dp(c, 84), LinearLayout.LayoutParams.WRAP_CONTENT));
            value = new TextView(c);
            value.setTypeface(Typeface.MONOSPACE);
            value.setTextSize(13);
            value.setTextColor(color(c, R.color.ink));
            view.addView(value, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        }

        /** colorId 0: no lamp (an empty slot keeps the column). */
        void set(int colorId, CharSequence v) {
            if (colorId == 0)
                lamp.setBackground(null);
            else
                lamp.setBackground(Brass.lamp(view.getContext(), colorId));
            value.setText(v);
        }
    }

    /** A section: a full-width brass plate band with the section name, dark serif caps. */
    static void section(LinearLayout box, Context c, String text, boolean gap) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setAllCaps(true);
        t.setTypeface(Typeface.SERIF, Typeface.BOLD);
        t.setLetterSpacing(0.25f);
        t.setTextSize(15);
        t.setTextColor(color(c, R.color.ink_dark));
        t.setBackgroundResource(R.drawable.plate);
        t.setGravity(Gravity.CENTER);
        int p = dp(c, 8);
        t.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(c, gap ? 28 : 8), 0, dp(c, 10));
        box.addView(t, lp);
    }

    static Button button(Context c, int text) {
        Button b = new Button(c, null, 0, R.style.Brass);
        b.setText(text);
        return b;
    }

    /** Layout parameters with the 6 dp margin of a brass button. */
    static LinearLayout.LayoutParams margins(Context c, int width, int height, float weight) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(width, height, weight);
        int m = dp(c, 6);
        lp.setMargins(m, m, m, m);
        return lp;
    }
}
