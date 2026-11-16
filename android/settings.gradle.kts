//
// Copyright (c) OpenIPC  https://openipc.org  MIT License
//

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "camex"
include(":app")
// The tunnel library for apps that build camex in (#170)
include(":tunnel")
