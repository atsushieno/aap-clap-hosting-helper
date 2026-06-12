plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.vanniktech.maven.publish)
}

version = libs.versions.aap.clap.hosting.helper.get()

android {
    namespace = "org.androidaudioplugin.clapHostingHelper"
    compileSdk = libs.versions.android.compileSdk.get().toInt()
    buildToolsVersion = libs.versions.build.tools.get()
    ndkVersion = libs.versions.ndk.get()

    defaultConfig {
        minSdk = libs.versions.android.minSdk.get().toInt()
        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                arguments(
                    "-DANDROID_STL=c++_shared",
                )
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    buildFeatures {
        prefab = true
        prefabPublishing = true
    }

    externalNativeBuild {
        cmake {
            version = libs.versions.cmake.get()
            // Root CMakeLists.txt is at the repo root, one level up from this module.
            path("../CMakeLists.txt")
        }
    }

    prefab {
        create("aap-clap-hosting-helper") {
            // Public headers live in include/ at the repo root.
            headers = "../include"
        }
    }

    // Exclude the shared C++ runtime — consumers bring their own.
    packaging.jniLibs.excludes.add("**/libc++_shared.so")
}

dependencies {
    // Prefab dependency: provides find_package(androidaudioplugin) at CMake time
    // and the AAR's classes at Java/Kotlin link time.
    implementation(libs.aap.core)

    testImplementation(libs.junit)
    androidTestImplementation(libs.test.ext.junit)
    androidTestImplementation(libs.test.espresso.core)
}

val packageName = project.name
val packageDescription = "AAP as CLAP host-side adapter (Prefab AAR)"
val packageUrl = "https://github.com/atsushieno/aap-clap-hosting-helper"

mavenPublishing {
    publishToMavenCentral()
    if (project.hasProperty("mavenCentralUsername") ||
        System.getenv("ORG_GRADLE_PROJECT_mavenCentralUsername") != null)
        signAllPublications()
    coordinates(group.toString(), project.name, version.toString())
    pom {
        name.set(packageName)
        description.set(packageDescription)
        url.set(packageUrl)
        scm { url.set(packageUrl) }
        licenses { license { name.set("MIT"); url.set("$packageUrl/blob/main/LICENSE") } }
        developers { developer { id.set("atsushieno"); name.set("Atsushi Eno"); email.set("atsushieno@gmail.com") } }
    }
}
