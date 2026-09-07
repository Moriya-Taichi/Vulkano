plugins {
    id("com.android.application")
    kotlin("android")
}
android {
    namespace = "dev.vulkano.sample"
    compileSdk = 35
    ndkVersion = "28.1.13356709"
    defaultConfig {
        applicationId = "dev.vulkano.sample"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        shaders { glslcArgs.addAll(listOf("--target-env=vulkan1.1")) }
    }
    buildFeatures { shaders = true }
    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
dependencies { implementation(project(":vulkano")) }
