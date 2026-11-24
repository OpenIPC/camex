/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Brass.java — the brass look of camex's screens (brand brass 1.3.0, the
 * interface review of 2026-11-18): heading with the version, panel, display,
 * board rows with lamps, section bands and brass buttons, built in code like
 * the screens themselves. Colours come from res/values/colors.xml, the faces
 * from assets/fonts (brass 1.5.1): Brass Serif for headings and buttons, Brass
 * Mono for the rest, no system face left on the screen.
 */
package org.openipc.camex;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

final class Brass {
    private Brass() {
    }

    @SuppressWarnings("deprecation")   // getColor(int) of Resources: the only one before 6.0
    static int color(Context c, int id) {
        return Build.VERSION.SDK_INT >= 23 ? c.getColor(id) : c.getResources().getColor(id);
    }

    // The brass faces, read from the assets once per process; null until then
    private static Typeface serif, mono, monoBold;

    /**
     * Read the three faces once; a face whose asset does not open stays the
     * system's (serif, monospace).
     */
    private static synchronized void loadFonts(Context c) {
        if (serif != null)
            return;
        android.content.res.AssetManager a = c.getApplicationContext().getAssets();
        mono = font(a, "BrassMono-Regular.ttf", Typeface.MONOSPACE);
        monoBold = font(a, "BrassMono-Bold.ttf", Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        serif = font(a, "BrassSerif-Bold.ttf", Typeface.create(Typeface.SERIF, Typeface.BOLD));
    }

    private static Typeface font(android.content.res.AssetManager a, String name, Typeface system) {
        try {
            return Typeface.createFromAsset(a, "fonts/" + name);
        } catch (RuntimeException e) {     // "native typeface cannot be made": no such asset
            return system;
        }
    }

    /** Brass Serif (bold): headings, section bands, buttons. */
    static synchronized Typeface serif(Context c) {
        loadFonts(c);
        return serif;
    }

    /** Brass Mono: the board, labels, fields, the log. */
    static synchronized Typeface mono(Context c) {
        loadFonts(c);
        return mono;
    }

    /**
     * The brass faces over a tree of views made in code without a face of their
     * own (the subtitle, check boxes, radio buttons, field labels): a button gets
     * Brass Serif, all other text Brass Mono, bold Brass Mono Bold; faces set
     * already stay. Called once a screen is built (after setContentView).
     */
    static void fonts(View v) {
        if (v instanceof TextView) {
            TextView t = (TextView) v;
            Typeface f = t.getTypeface();
            Context c = v.getContext();
            Typeface s, m, mb;
            synchronized (Brass.class) {
                loadFonts(c);
                s = serif;
                m = mono;
                mb = monoBold;
            }
            if (f != s && f != m && f != mb) {
                int style = f == null ? Typeface.NORMAL : f.getStyle();
                // a Button is a TextView too; a CheckBox or a RadioButton is a Button,
                // but its text is a label: mono
                if (t instanceof Button && !(t instanceof android.widget.CompoundButton))
                    t.setTypeface(s);
                else
                    t.setTypeface((style & Typeface.BOLD) != 0 ? mb : m);
            }
        }
        if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++)
                fonts(g.getChildAt(i));
        }
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
        t.setTypeface(serif(c));
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
        t.setTypeface(mono(c));
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
        v.setTypeface(mono(c));
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
            label.setTypeface(mono(c));
            label.setTextSize(13);
            label.setLetterSpacing(0.08f);
            label.setTextColor(color(c, R.color.dim));
            view.addView(label, new LinearLayout.LayoutParams(dp(c, 84), LinearLayout.LayoutParams.WRAP_CONTENT));
            value = new TextView(c);
            value.setTypeface(mono(c));
            value.setTextSize(13);
            value.setTextColor(color(c, R.color.ink));
            view.addView(value, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        }

        /** A lamp without colour: its rim only (a part out of work, as a stopped tunnel). */
        static final int UNLIT = -1;

        /** colorId 0: no lamp (an empty slot keeps the column); {@link #UNLIT}: the rim only. */
        void set(int colorId, CharSequence v) {
            if (colorId == 0) {
                lamp.setBackground(null);
            } else if (colorId == UNLIT) {
                GradientDrawable d = new GradientDrawable();
                d.setShape(GradientDrawable.OVAL);
                d.setColor(0);
                d.setStroke(dp(view.getContext(), 1), 0xff6e5320);
                lamp.setBackground(d);
            } else {
                lamp.setBackground(Brass.lamp(view.getContext(), colorId));
            }
            value.setText(v);
        }
    }

    /** A section: a full-width brass plate band with the section name, dark serif caps. */
    static void section(LinearLayout box, Context c, String text, boolean gap) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setAllCaps(true);
        t.setTypeface(serif(c));
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
        b.setTypeface(serif(c));
        return b;
    }

    /** Layout parameters with the 6 dp margin of a brass button. */
    static LinearLayout.LayoutParams margins(Context c, int width, int height, float weight) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(width, height, weight);
        int m = dp(c, 6);
        lp.setMargins(m, m, m, m);
        return lp;
    }

    /**
     * The Show log window (brand brass 1.3.1, after pusher 0.22.1): the oldest line at the top,
     * `rows` lines high with its own scroll, follows the end unless scrolled up, D-pad focus
     * with the focus ring on a frame around the scroll view.
     */
    static final class LogWindow {
        final android.widget.FrameLayout view;    // the frame: display, focus ring
        private final ScrollView scroll;
        private final TextView text;
        private String shown = "";

        LogWindow(Context c, int rows) {
            text = new TextView(c);
            text.setTypeface(mono(c));
            text.setTextSize(11);
            text.setTextColor(color(c, R.color.dim));
            int pad = dp(c, 12);
            text.setPadding(pad, pad, pad, pad);
            scroll = new ScrollView(c) {
                @Override
                public boolean onInterceptTouchEvent(android.view.MotionEvent e) {
                    // the page scrolls too: keep the drag inside the window
                    if (e.getActionMasked() == android.view.MotionEvent.ACTION_DOWN)
                        getParent().requestDisallowInterceptTouchEvent(true);
                    return super.onInterceptTouchEvent(e);
                }
            };
            scroll.setFocusable(true);
            // the ring shows the focus: no system highlight over the text (Android 8+)
            if (android.os.Build.VERSION.SDK_INT >= 26) scroll.setDefaultFocusHighlightEnabled(false);
            scroll.setScrollBarStyle(View.SCROLLBARS_INSIDE_OVERLAY);
            scroll.addView(text);
            // The ring on a frame around the scroll view: the foreground of a
            // ScrollView moves with its content; the frame takes the focused
            // state of its child (FrameLayout foreground works from API 1)
            view = new android.widget.FrameLayout(c);
            view.setBackgroundResource(R.drawable.display);
            view.setAddStatesFromChildren(true);
            view.setForeground(c.getResources().getDrawable(R.drawable.focus_ring, null));
            view.addView(scroll, new android.widget.FrameLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.MATCH_PARENT));
            height = rows * text.getLineHeight() + 2 * pad;
        }

        /** The height for the layout parameters: `rows` lines and the padding. */
        final int height;

        // Whether the window follows the end: true at the end, false once the user scrolls up;
        // kept by the scroll listener, so a scroll posted earlier sees a later drag (#193 review)
        private boolean follow = true;

        private boolean atEnd() {
            return scroll.getScrollY() + scroll.getHeight() >= text.getHeight() - dp(scroll.getContext(), 4);
        }

        // The offset the window scrolled itself to: a scroll event at it is not the user's
        private int autoTarget = -1;

        private void scrollToEnd() {
            autoTarget = Math.max(0, text.getHeight() - scroll.getHeight());
            scroll.scrollTo(0, autoTarget);
        }

        private void toEnd() {
            scroll.post(() -> {
                if (follow) scrollToEnd();
            });
        }

        /** Shown again (Show log on): at the end, as the rule wants. */
        void shown() {
            follow = true;
            toEnd();
        }

        void setText(String s, long removedNow) {
            if (s.equals(shown))
                return;
            if (!listening) {
                listening = true;
                // the user's scroll decides whether to follow; the window's own does not
                scroll.getViewTreeObserver().addOnScrollChangedListener(() -> {
                    if (scroll.getHeight() == 0 || scroll.getScrollY() == autoTarget) return;
                    follow = atEnd();
                });
                // new lines laid out: to the end, once the text has its new height
                text.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
                    if (follow && b - t != ob - ot) scrollToEnd();
                });
            }
            // Lines gone from the top (the buffer is full): the view of a reader scrolled up
            // stays on the same lines; the shift is the height of the dropped text as laid out,
            // a long line wraps over several screen lines (after pusher 0.22.2)
            // How many: the log's counter of dropped lines, not a comparison of the texts,
            // which is wrong when equal lines repeat (brass 1.3.1, after vectis #120)
            int dy = 0;
            long gone = removed < 0 ? 0 : removedNow - removed;
            removed = removedNow;
            if (!follow && gone > 0) {
                int chars = headChars(shown, gone);
                android.text.Layout lay = text.getLayout();
                if (chars > 0 && lay != null)
                    dy = lay.getLineTop(lay.getLineForOffset(Math.min(chars, shown.length())));
            }
            shown = s;
            text.setText(s);
            if (follow) {
                toEnd();
            } else if (dy > 0) {
                int d = dy;
                scroll.post(() -> scroll.scrollTo(0, Math.max(0, scroll.getScrollY() - d)));
            }
        }

        private boolean listening;

        // The log's dropped-line counter at the text shown (-1: none yet)
        private long removed = -1;

        /** The length of the first {@code lines} lines of {@code text}, their newlines included. */
        static int headChars(String text, long lines) {
            int at = 0;
            for (long i = 0; i < lines; i++) {
                int nl = text.indexOf('\n', at);
                if (nl < 0) return text.length();
                at = nl + 1;
            }
            return at;
        }
    }
}
