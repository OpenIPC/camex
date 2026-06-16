plugins {
    id("com.android.application")
}

// Release keystore path (CI secret or local env); blank/absent => unsigned build.
val releaseKeystore: String? =
    System.getenv("CAMEX_KEYSTORE_FILE")?.takeIf { it.isNotBlank() }

android {
    namespace = "org.openipc.camex"
    compileSdk = 34

    defaultConfig {
        applicationId = "org.openipc.camex"
        minSdk = 26
        targetSdk = 34
        versionCode = 20306
        versionName = "2.3.6"
        ndk { abiFilters += listOf("arm64-v8a") }
        externalNativeBuild {
            cmake { arguments("-DANDROID_STL=c++_static") }
        }
    }
    signingConfigs {
        // Release signing is driven by environment variables so the same build
        // works in CI (GitHub Actions secrets) and locally. When the keystore
        // env var is absent the release build stays unsigned (e.g. forks/PRs).
        create("release") {
            if (releaseKeystore != null) {
                storeFile = file(releaseKeystore)
                storePassword = System.getenv("CAMEX_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("CAMEX_KEY_ALIAS")
                keyPassword = System.getenv("CAMEX_KEY_PASSWORD")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isDebuggable = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            if (releaseKeystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
    externalNativeBuild {
        cmake { path("src/main/jni/CMakeLists.txt") }
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.6.1")
    // core 1.12.0+ provides ServiceCompat.startForeground(..., foregroundServiceType)
    implementation("androidx.core:core:1.12.0")
    implementation("com.google.android.material:material:1.11.0")
}
