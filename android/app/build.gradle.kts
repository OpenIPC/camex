plugins {
    id("com.android.application")
}

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
    buildTypes {
        release {
            isMinifyEnabled = true
            isDebuggable = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }
    externalNativeBuild {
        cmake { path("src/main/jni/CMakeLists.txt") }
    }
}

dependencies {
    implementation("androidx.appcompat:appcompat:1.6.1")
    implementation("com.google.android.material:material:1.11.0")
}
