import java.io.FileInputStream
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

/** The release signing key lives outside git: keystore.properties at the repo root, ignored. */
val keystoreFile: File = rootProject.file("../keystore.properties")

android {
    namespace = "dev.niccc2007.relyacam"
    compileSdk {
        version = release(37)
    }

    if (keystoreFile.exists()) {
        val p = Properties().apply { FileInputStream(keystoreFile).use { load(it) } }
        signingConfigs {
            create("release") {
                keyAlias = p["keyAlias"].toString()
                keyPassword = p["keyPassword"].toString()
                storeFile = rootProject.file(p["storeFile"].toString())
                storePassword = p["storePassword"].toString()
                enableV1Signing = false
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    defaultConfig {
        applicationId = "dev.niccc2007.relyacam"
        minSdk = 26
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (keystoreFile.exists()) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
}

base.archivesName.set("RelyACam")

dependencies {
    implementation(project(":core:camera"))
    implementation(project(":core:stream"))
    implementation(project(":core:protocol"))
    implementation(project(":core:qr"))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.material3)
    implementation(libs.kotlinx.coroutines.android)
}
