plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// -Pcope.pcre2=true (or gradle.properties) builds the native lib with PCRE2+JIT.
val usePcre2: Boolean = (project.findProperty("cope.pcre2") as String?)?.toBoolean() ?: false

android {
    namespace = "dev.cope.ide"
    compileSdk = 34
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "dev.cope.ide"
        minSdk = 27
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        // No instrumentation tests in this phase; unit tests are JVM-side.
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            // Two ABIs only, per the APK budget. Splits below ship them separately.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++20"
                arguments += listOf(
                    // c++_static: one native lib, so no need to ship libc++_shared.so.
                    "-DANDROID_STL=c++_static",
                    "-DCOPE_BUILD_TESTS=OFF",
                    "-DCOPE_BUILD_CLI=OFF",
                    "-DCOPE_USE_PCRE2=${if (usePcre2) "ON" else "OFF"}",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "armeabi-v7a")
            isUniversalApk = false
        }
    }

    sourceSets {
        getByName("main") {
            kotlin.srcDirs("src/main/kotlin")
            // The TextMate asset library is shipped verbatim from the repo root:
            // textmate/grammars/*.json and textmate/themes/{dark,light}/*.json
            // land as assets/grammars/... and assets/themes/... The generated
            // index/ TSVs live under src/main/assets.
            assets.srcDirs("src/main/assets", file("${rootDir}/../textmate"))
        }
    }

    buildTypes {
        getByName("debug") {
            isMinifyEnabled = false
            isJniDebuggable = true
            applicationIdSuffix = ".debug"
            versionNameSuffix = "-debug"
        }
        getByName("release") {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            // Unsigned in CI; the artifact is for sideloading after local signing.
            signingConfig = null
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
        freeCompilerArgs += listOf(
            // Warning, not strict: nobody can compile locally, so a missing
            // `public` must not be a build failure. See docs/ui/PHASE4-PLAN.md.
            "-Xexplicit-api=warning",
            "-opt-in=kotlin.RequiresOptIn",
        )
    }

    packaging {
        resources {
            excludes += setOf(
                "/META-INF/{AL2.0,LGPL2.1}",
                "DebugProbesKt.bin",
                "kotlin-tooling-metadata.json",
            )
        }
        jniLibs {
            useLegacyPackaging = false
        }
    }

    androidResources {
        // Grammars and themes are read with a mmap-friendly whole-file read and
        // are already tiny once deflated; fonts must stay uncompressed so the
        // platform can map them.
        noCompress += listOf("ttf", "otf")
    }

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.06.00")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.2")
    implementation("androidx.documentfile:documentfile:1.0.1")

    // Compose: foundation + ui only. No material3 component set is used for
    // chrome (see the design brief); material3 is present for ripple/typography
    // primitives and nothing else.
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("androidx.compose.ui:ui-tooling-preview")

    testImplementation("junit:junit:4.13.2")
}
