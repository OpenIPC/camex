/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Field.java — one setting of the camex tunnel, for an app that builds its settings
 * screen from the fields (#170)
 */
package org.openipc.camex.tunnel;

import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * One setting of the tunnel. Values are strings: {@code "true"} / {@code "false"} for a flag,
 * one of {@link #choices} for a choice. The library stores none of them: the app keeps its
 * settings and hands them to {@link CamexTunnel#start}.
 */
public final class Field {
    public enum Kind {
        /** a line of text */
        TEXT,
        /** host:port */
        HOST_PORT,
        /** a secret: hidden on the screen, kept by the app as it keeps its secrets */
        SECRET,
        /** one of {@link #choices} */
        CHOICE,
        /** on or off */
        FLAG,
    }

    /** The key of the value in the values Map. */
    public final String key;
    /** A short English label. */
    public final String label;
    public final Kind kind;
    /** The value of a new setting ("" for none). */
    public final String initial;
    /** The choices of a {@link Kind#CHOICE}, otherwise empty. */
    public final List<String> choices;
    /** False for a setting that may stay empty (camex then takes its default). */
    public final boolean required;

    private Field(String key, String label, Kind kind, String initial, boolean required,
                  String... choices) {
        this.key = key;
        this.label = label;
        this.kind = kind;
        this.initial = initial;
        this.required = required;
        this.choices = Collections.unmodifiableList(Arrays.asList(choices));
    }

    static Field text(String key, String label, String initial, boolean required) {
        return new Field(key, label, Kind.TEXT, initial, required);
    }

    static Field hostPort(String key, String label, String initial) {
        return new Field(key, label, Kind.HOST_PORT, initial, true);
    }

    static Field secret(String key, String label) {
        return new Field(key, label, Kind.SECRET, "", true);
    }

    static Field choice(String key, String label, String initial, String... choices) {
        return new Field(key, label, Kind.CHOICE, initial, true, choices);
    }

    @Override
    public String toString() {
        return key + " (" + kind + ")";
    }
}
