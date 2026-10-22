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
        versionCode = 30201
        versionName = "3.2.1"
        // 64-bit phones and 32-bit devices (TV boxes, older phones).
        // Override with -PcamexAbis=arm64-v8a for a smaller APK.
        val abis = (project.findProperty("camexAbis") as String?)
            ?: "arm64-v8a,armeabi-v7a"
        ndk { abiFilters += abis.split(",").map { it.trim() } }
        // The app's own strings are English only; keep the libraries' translations
        // just for the README languages (#55)
        resourceConfigurations += listOf("en", "ru", "zh-rCN")
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
            isShrinkResources = true
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
    // Platform classes only (#55): no AppCompat, Material or AndroidX core.
    // @Keep for the method native code calls; R8's default rules honour it.
    implementation("androidx.annotation:annotation:1.7.1")
}
