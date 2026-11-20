group = "io.github.khirohis.flutter_turbo_fetch"
version = "1.0-SNAPSHOT"

buildscript {
    val kotlinVersion = "2.4.0"
    repositories {
        google()
        mavenCentral()
    }

    dependencies {
        classpath("com.android.tools.build:gradle:9.1.0")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:$kotlinVersion")
    }
}

allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

plugins {
    id("com.android.library")
}

val androidEngine: String = (
    providers.gradleProperty("flutter_turbo_fetch.androidEngine").orNull
        ?: rootProject.findProperty("flutter_turbo_fetch.androidEngine")?.toString()
        ?: "okhttp"
    ).trim().lowercase()

if (androidEngine != "okhttp" && androidEngine != "cronet") {
    throw GradleException(
        "Unsupported value for 'flutter_turbo_fetch.androidEngine': '$androidEngine'. " +
            "Expected 'okhttp' or 'cronet'."
    )
}

android {
    namespace = "io.github.khirohis.flutter_turbo_fetch"

    compileSdk = 36

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    sourceSets {
        getByName("main") {
            java.srcDirs(
                "src/main/kotlin",
                if (androidEngine == "cronet") "src/cronet/kotlin" else "src/okhttp/kotlin"
            )
        }
        getByName("test") {
            java.srcDirs(
                "src/test/kotlin",
                if (androidEngine == "cronet") "src/testCronet/kotlin" else "src/testOkhttp/kotlin"
            )
        }
    }

    defaultConfig {
        minSdk = 24
    }

    testOptions {
        unitTests {
            isReturnDefaultValues = true
            isIncludeAndroidResources = true
            all {
                it.useJUnitPlatform()

                it.outputs.upToDateWhen { false }

                it.testLogging {
                    events("passed", "skipped", "failed", "standardOut", "standardError")
                    showStandardStreams = true
                }
            }
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

dependencies {
    if (androidEngine == "cronet") {
        implementation("org.chromium.net:cronet-api:143.7445.0")
    } else {
        implementation("com.squareup.okhttp3:okhttp:4.12.0")
    }
    testImplementation("org.jetbrains.kotlin:kotlin-test")
    testImplementation("org.mockito:mockito-core:5.14.2")
}
