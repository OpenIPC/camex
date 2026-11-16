//
// Copyright (c) OpenIPC  https://openipc.org  MIT License
//
// Top-level build file for camex Android app and the tunnel library (#170)
plugins {
    id("com.android.application") version "8.2.0" apply false
    id("com.android.library") version "8.2.0" apply false
}

// The tunnel library is built for the app's ABIs (-PcamexAbis, checked by both)
extra["camexTunnelAbis"] = ((findProperty("camexAbis") as String?) ?: "arm64-v8a,armeabi-v7a")
    .split(",").map { it.trim() }
