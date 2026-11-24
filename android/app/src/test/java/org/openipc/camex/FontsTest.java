/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * FontsTest.java — the brass fonts as brand gives them (local/brand/brass/fonts/SHA256SUMS,
 * brass 1.5.1), byte for byte: the Android app's (assets/fonts) and the start page's
 * (src/fonts). Gradle runs a unit test in the module's directory (android/app).
 */
package org.openipc.camex;

import static org.junit.Assert.assertEquals;

import java.io.File;
import java.nio.file.Files;
import java.security.MessageDigest;

import org.junit.Test;

public class FontsTest {
    private static final String[][] SUMS = {
        {"src/main/assets/fonts/OFL.txt", "7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da"},
        {"src/main/assets/fonts/BrassMono-Bold.ttf", "190657fa835af45738bcad97b968dde8148ca0e854c1e8737d518919f2394544"},
        {"src/main/assets/fonts/BrassMono-Regular.ttf", "e139010955613c1a95467f4fa403bd158b5a608855b4eeac7643e0f635256749"},
        {"src/main/assets/fonts/BrassSerif-Bold.ttf", "3d0968d62ec97a044c651bbd86b74974e02452be1522bfd4ddc7e4b50bf30bfc"},
        {"../../src/fonts/OFL.txt", "7e6b2818edbd8f6a01ae80641cc8f16a51080d08fb4e532be3a0b6f74adb07da"},
        {"../../src/fonts/BrassMono-Bold.woff2", "a533e9dc41fa7cdf54850c4f5deaccad588586bce91a641fbe8448503b8171b6"},
        {"../../src/fonts/BrassMono-Regular.woff2", "82556c93d385127871fc82e7338c176b1056c12d9e79f230b49dfe9491544340"},
        {"../../src/fonts/BrassSerif-Bold.woff2", "d971f32a0c6e2309fec542eaacf5e394f9995bab62a85f1dae8ab902ad3d1893"},
    };

    private static String sha256(File f) throws Exception {
        StringBuilder s = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(f.toPath())))
            s.append(String.format("%02x", b));
        return s.toString();
    }

    @Test
    public void fontsAsBrandGivesThem() throws Exception {
        for (String[] f : SUMS)
            assertEquals(f[0], f[1], sha256(new File(f[0])));
    }

    @Test
    public void noOtherFonts() {
        String[] names = new File("src/main/assets/fonts").list();
        java.util.Arrays.sort(names);
        assertEquals("[BrassMono-Bold.ttf, BrassMono-Regular.ttf, BrassSerif-Bold.ttf, OFL.txt]",
                java.util.Arrays.toString(names));
    }
}
