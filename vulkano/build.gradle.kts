plugins {
    id("com.android.library")
    kotlin("android")
    `maven-publish`
}

android {
    namespace = "dev.vulkano"
    compileSdk = 35
    ndkVersion = "28.1.13356709"
    defaultConfig {
        minSdk = 29
        consumerProguardFiles("consumer-rules.pro")
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=c++_static" } }
    }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    sourceSets {
        getByName("test").resources.srcDir("../tests/shaders")
        getByName("androidTest").assets.srcDir("../tests/shaders")
    }
    testOptions.unitTests.all {
        providers.gradleProperty("vulkano.hostLibraryPath").orNull?.let { path ->
            it.systemProperty("java.library.path", file(path).absolutePath)
            it.systemProperty("vulkano.runNativeTests", "true")
        }
    }
    publishing { singleVariant("release") { withSourcesJar() } }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
}

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                groupId = "dev.vulkano"
                artifactId = "vulkano"
                version = "0.1.0-SNAPSHOT"
                from(components["release"])
            }
        }
    }
}
