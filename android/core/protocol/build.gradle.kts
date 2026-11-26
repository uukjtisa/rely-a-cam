plugins {
    alias(libs.plugins.kotlin.jvm)
}

// Java 17: an Android module links these classes, and 17 is the floor the app compiles to.
kotlin {
    compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) }
}
java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

dependencies {
    compileOnly(libs.json)
    testImplementation(libs.json)
    testImplementation(libs.junit)
}

// The golden vectors live at the repo root, shared with the OBS plugin's tests.
tasks.test {
    systemProperty("vectors", rootProject.file("../protocol/vectors").absolutePath)
}
