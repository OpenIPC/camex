/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * LogWindowTest.java — how much of the shown log left the top when the buffer dropped its
 * oldest lines (Brass.LogWindow.droppedChars; after pusher 0.22.2). camex's log has no
 * newline after its last line.
 */
package org.openipc.camex;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class LogWindowTest {
    private static int dropped(String old, String now) {
        return Brass.LogWindow.droppedChars(old, now);
    }

    @Test
    public void nothingDropped() {
        assertEquals(0, dropped("", "a"));
        assertEquals(0, dropped("a\nb", "a\nb"));
        assertEquals(0, dropped("a\nb", "a\nb\nc"));
    }

    @Test
    public void headDropped() {
        assertEquals(2, dropped("a\nb\nc", "b\nc\nd"));
        assertEquals(6, dropped("aa\nbb\ncc", "cc\ndd\nee"));
        // a long line (wraps on the screen) counts with all its characters
        assertEquals(42, dropped("10:00:00 CONNECTING · bad server: refused\nb", "b\nc"));
    }

    @Test
    public void nothingShared() {
        assertEquals(3, dropped("a\nb", "c\nd"));
    }
}
