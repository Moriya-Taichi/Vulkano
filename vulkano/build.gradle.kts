import org.jetbrains.dokka.gradle.DokkaTask

plugins {
    id("com.android.library")
    kotlin("android")
    `maven-publish`
    id("com.vanniktech.maven.publish.base")
    id("org.jetbrains.dokka")
}

android {
    namespace = "dev.vulkano"
    compileSdk = 35
    ndkVersion = "28.1.13356709"
    defaultConfig {
        minSdk = 29
        consumerProguardFiles("consumer-rules.pro")
        testInstrumentationRunner = "dev.vulkano.GpuTestRunner"
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
        getByName("androidTest").java.srcDir("src/test/kotlin")
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
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.8.0")
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
}

// Keep publication identity separate from the Kotlin package/Android namespace.
group = providers.gradleProperty("GROUP").get()
version = providers.gradleProperty("VERSION_NAME").get()
val centralRelease = providers.gradleProperty("centralRelease").orNull == "true"
val licenseName = providers.gradleProperty("POM_LICENSE_NAME")
val licenseUrl = providers.gradleProperty("POM_LICENSE_URL")

// A signing key/token is never needed for ordinary builds or local previews.
if (centralRelease) {
    require(version.toString().matches(Regex("[0-9]+\\.[0-9]+\\.[0-9]+(?:-[A-Za-z0-9][A-Za-z0-9.-]*)?")) &&
        !version.toString().endsWith("-SNAPSHOT")) { "Central releases require an explicit non-SNAPSHOT VERSION_NAME" }
    require(rootProject.file("LICENSE").isFile && licenseName.orNull?.isNotBlank() == true &&
        licenseUrl.orNull?.startsWith("https://") == true) {
        "Project LICENSE and POM_LICENSE_NAME / POM_LICENSE_URL are required for publishing"
    }
    for (name in listOf("mavenCentralUsername", "mavenCentralPassword", "signingInMemoryKey", "signingInMemoryKeyPassword")) {
        require(providers.gradleProperty(name).orNull?.isNotBlank() == true) { "Missing publishing property: $name" }
    }
    mavenPublishing {
        publishToMavenCentral(automaticRelease = false)
        signAllPublications()
    }
}

val licenseResources = tasks.register<Sync>("prepareLicenseResources") {
    from("src/main/cpp/third_party") {
        include("README.md", "VMA-LICENSE.txt", "SPIRV-Headers-LICENSE.txt", "spirv-reflect/LICENSE", "vulkan-headers/README.md", "vulkan-headers/LICENSES/*.txt")
    }
    from(rootProject.file("LICENSE")) { rename { "Vulkano-LICENSE" } }
    into(layout.buildDirectory.dir("generated/licenseResources/META-INF/licenses/vulkano"))
}
android.sourceSets.getByName("main").resources.srcDir(layout.buildDirectory.dir("generated/licenseResources"))
tasks.named("preBuild") { dependsOn(licenseResources) }

val documentationJar = tasks.register<Jar>("documentationJar") {
    archiveClassifier.set("javadoc")
    from(tasks.named<DokkaTask>("dokkaHtml").flatMap { it.outputDirectory })
    dependsOn(tasks.named("dokkaHtml"))
}

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                groupId = project.group.toString()
                artifactId = "vulkano"
                version = project.version.toString()
                from(components["release"])
                artifact(documentationJar)
                pom {
                    name.set("Vulkano")
                    description.set("A Metal-style Kotlin API for Vulkan graphics and compute on Android.")
                    url.set("https://github.com/Moriya-Taichi/Vulkano")
                    if (licenseName.isPresent && licenseUrl.isPresent) {
                        licenses { license { name.set(licenseName); url.set(licenseUrl); distribution.set("repo") } }
                    }
                    developers {
                        developer { id.set("Moriya-Taichi"); name.set("Moriya-Taichi"); url.set("https://github.com/Moriya-Taichi") }
                    }
                    scm {
                        url.set("https://github.com/Moriya-Taichi/Vulkano")
                        connection.set("scm:git:https://github.com/Moriya-Taichi/Vulkano.git")
                        developerConnection.set("scm:git:ssh://git@github.com/Moriya-Taichi/Vulkano.git")
                    }
                }
            }
        }
        repositories {
            maven { name = "localPreview"; url = uri(layout.buildDirectory.dir("repository")) }
        }
    }
    tasks.named<Jar>("sourceReleaseJar") {
        from("src/main/cpp") // Native sources and third-party notices accompany the Kotlin sources.
        from(rootProject.file("LICENSE")) { into("META-INF/licenses/vulkano") }
    }
}

// Same Maven directory layout as a remote repository; useful without Central credentials.
tasks.register<Zip>("generateRepo") {
    dependsOn("publishReleasePublicationToLocalPreviewRepository")
    from(layout.buildDirectory.dir("repository")) {
        include("${project.group.toString().replace('.', '/')}/vulkano/${project.version}/**")
    }
    into("vulkano-repository")
    archiveFileName.set("vulkano-${project.version}-maven.zip")
    destinationDirectory.set(layout.buildDirectory.dir("distributions"))
    isPreserveFileTimestamps = false
    isReproducibleFileOrder = true
}
