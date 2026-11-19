/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * LogWindowTest.java — the head of the shown log that left the top when the buffer dropped
 * its oldest lines (Brass.LogWindow.headChars): the number of lines comes from the log's
 * counter, so equal lines that repeat count right (brass 1.3.1). camex's log has no newline
 * after its last line.
 */
package org.openipc.camex;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public class LogWindowTest {
    private static int head(String text, long lines) {
        return Brass.LogWindow.headChars(text, lines);
    }

    @Test
    public void noLines() {
        assertEquals(0, head("a\nb", 0));
        assertEquals(0, head("", 0));
    }

    @Test
    public void someLines() {
        assertEquals(2, head("a\nb\nc", 1));
        assertEquals(6, head("aa\nbb\ncc", 2));
        // a long line (it wraps on the screen) counts with all its characters
        assertEquals(42, head("10:00:00 CONNECTING · bad server: refused\nb", 1));
    }

    @Test
    public void equalLinesRepeat() {
        // the same line twice in one second: two lines gone are two, whatever the text
        assertEquals(22, head("10:00 stop\n10:00 stop\n10:01 x", 2));
    }

    @Test
    public void moreThanShown() {
        assertEquals(3, head("a\nb", 5));
    }
}
