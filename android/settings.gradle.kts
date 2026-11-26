pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "RelyACam"
include(":app")
// The one Camera2 owner: discovery, zoom, focus, torch, orientation (AGENTS.md rule 4).
include(":core:camera")
// Encoders and the link to the PC.
include(":core:stream")
// The wire format, plain JVM, tested against ../protocol/vectors.
include(":core:protocol")
// QR decoding, from Filet.
include(":core:qr")
