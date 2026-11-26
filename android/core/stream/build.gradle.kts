plugins {
    alias(libs.plugins.android.library)
}

android {
    namespace = "dev.niccc2007.relyacam.stream"
    compileSdk {
        version = release(37)
    }
    defaultConfig {
        minSdk = 26
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    api(project(":core:protocol"))
    implementation(libs.androidx.core.ktx)
    api(libs.kotlinx.coroutines.android)
}
