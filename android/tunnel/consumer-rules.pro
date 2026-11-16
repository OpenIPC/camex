#
# Copyright (c) OpenIPC  https://openipc.org  MIT License
#
# consumer-rules.pro — for the app's R8: libcamex.so finds these by name
#
-keepclassmembers class org.openipc.camex.tunnel.CamexVpnService {
    private int establishTun(java.lang.String, int, int, java.lang.String[]);
    native <methods>;
}
