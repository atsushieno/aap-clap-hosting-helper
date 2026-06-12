plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.android) apply false
    alias(libs.plugins.vanniktech.maven.publish) apply false
}

subprojects {
    group = "org.androidaudioplugin"
    repositories {
        google()
        mavenLocal()
        mavenCentral()
    }
}

tasks.register<Delete>("clean") {
    delete(rootProject.layout.buildDirectory)
}
