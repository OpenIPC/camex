//
// Copyright (c) OpenIPC  https://openipc.org  MIT License
//
// build.gradle.kts — the camex tunnel library (#170): camex in-process as a VPN client for
// apps that build it in. An app takes it from the camex sources (a submodule):
//   settings.gradle: include(":camex-tunnel")
//                    project(":camex-tunnel").projectDir = file("<camex>/android/tunnel")
//   app:             implementation(project(":camex-tunnel"))
// The app's root project puts the Android Gradle plugin on the classpath.
//

plugins {
    id("com.android.library")
}

// The library's version is camex's: src/version.h, as the app reads it
val camexVersion: String = run {
    val header = file("../../src/version.h")
    val m = Regex("""(?m)^#\s*define\s+CAMEX_VERSION\s+"(\d+\.\d+\.\d+)"\s*$""")
        .find(header.readText())
        ?: throw GradleException("no CAMEX_VERSION \"X.Y.Z\" in $header")
    m.groupValues[1]
}

android {
    namespace = "org.openipc.camex.tunnel"
    compileSdk = 34

    defaultConfig {
        minSdk = 21
        consumerProguardFiles("consumer-rules.pro")
        // tests/android/tunnel-e2e.sh runs the instrumented test on an emulator
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        buildConfigField("String", "CAMEX_VERSION", "\"$camexVersion\"")
        // No abiFilters: the app's ABIs are the APK's. An app that sets
        // camexTunnelAbis (a list of ABI names, or a comma-separated string) in its root project's extra properties
        // has the native code built only for those.
        val abis = rootProject.extensions.extraProperties.let {
            if (it.has("camexTunnelAbis")) it.get("camexTunnelAbis") else null
        }
        if (abis != null) {
            // a list, or a comma-separated string (a Groovy ext string, a -P property)
            val list = when (abis) {
                is String -> abis.split(",").map { it.trim() }
                is Iterable<*> -> abis.map { it.toString().trim() }
                else -> throw GradleException("camexTunnelAbis: a list of ABI names or a comma-separated string")
            }
            val known = setOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
            require(list.isNotEmpty() && list.all { it in known }) {
                "camexTunnelAbis=${list.joinToString(",")}: one or more of ${known.joinToString(",")}"
            }
            ndk { abiFilters += list }
        }
    }

    buildFeatures {
        buildConfig = true
    }

    externalNativeBuild {
        cmake {
            path("CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_1_8
        targetCompatibility = JavaVersion.VERSION_1_8
    }

    testOptions {
        unitTests.isReturnDefaultValues = true
        // the instrumented test APK as an app of today (Android 14 refuses below 23)
        targetSdk = 34
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test:runner:1.4.0")
    androidTestImplementation("junit:junit:4.13.2")
}
