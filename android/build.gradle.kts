import java.net.URI
import java.security.MessageDigest

plugins {
	id("com.android.application")
	id("com.google.protobuf")
}

// Utility function used to get the git version
// All the below replaces the following "gladed.androidgitversion" config:
// configure<com.gladed.androidgitversion.AndroidGitVersionExtension> {
//   codeFormat = "MNNPPBBBB"
//   format = "%tag%%-count%%-branch%%-dirty%"
//   prefix = "v"  // Only tags beginning with v are considered.
//   untrackedIsDirty = false
// }

fun ProviderFactory.git(vararg args: String): String {
	val result = exec {
		commandLine("git", *args)
		isIgnoreExitValue = true
	}
	return result.standardOutput.asText.get().trim()
}

val gitTag = providers.git("describe", "--tags", "--match", "v*", "--abbrev=0")
	.ifEmpty { "v0.0.0" }
val commitsSinceTag = providers.git(
	"rev-list", "$gitTag..HEAD", "--count"
).toIntOrNull() ?: 0
val branchName = providers.git(
	"rev-parse", "--abbrev-ref", "HEAD"
).replace("/", "-")
val isDirty = providers.git("status", "--porcelain")
	.lineSequence()
	.any { it.isNotBlank() && !it.startsWith("??") } // untrackedIsDirty = false

val versionParts = gitTag
	.removePrefix("v")
	.split(".")
	.mapNotNull { it.toIntOrNull() }

// Ensure we have at least 3 components by padding with 0
val major = versionParts.getOrElse(0) { 0 }
val minor = versionParts.getOrElse(1) { 0 }
val patch = versionParts.getOrElse(2) { 0 }

val gitVersionName = if (commitsSinceTag == 0) gitTag else buildString {
	append(gitTag)
	append("-")
	append(commitsSinceTag)
	append("-")
	append(branchName)
	if (isDirty) append("-dirty")
}

val gitVersionCode =
	major * 100_000_000 +
		minor * 1000_000 +
		patch * 10_000 +
		commitsSinceTag

// Save sync (Core/SaveSync.h) with emusync, from a pinned release: its AAR, downloaded once and
// checked against its SHA-256 (from the release's SHA256SUMS). It brings libemusync.so, which
// SaveSync loads at run time on Android 7.0 and later (the AAR's minSdk; PPSSPP keeps 21: see
// AndroidManifest.xml), and the Keystore-backed settings class SaveSyncSecrets uses. Keep the
// version in step with ext/emusync.
val emusyncVersion = "0.2.0"
val emusyncAarSha256 = "bd69015c6d46cb1bca108e3b085a8efb1507b3f6ca31c504523dd23c4fa6be59"
val emusyncAar = layout.buildDirectory.file("emusync/emusync-$emusyncVersion.aar")
val downloadEmusync = tasks.register("downloadEmusync") {
	val url = "https://github.com/ilyalissoboi/emusync/releases/download/v$emusyncVersion/emusync-$emusyncVersion.aar"
	val out = emusyncAar
	val sha256 = emusyncAarSha256
	outputs.file(out)
	doLast {
		val file = out.get().asFile
		fun digest(f: File) = MessageDigest.getInstance("SHA-256")
			.digest(f.readBytes()).joinToString("") { "%02x".format(it) }
		if (file.isFile && digest(file) == sha256) return@doLast
		file.parentFile.mkdirs()
		val tmp = File(file.path + ".part")
		URI(url).toURL().openStream().use { input -> tmp.outputStream().use { input.copyTo(it) } }
		val got = digest(tmp)
		if (got != sha256) {
			tmp.delete()
			throw GradleException("emusync $emusyncVersion: SHA-256 $got of $url, expected $sha256")
		}
		tmp.renameTo(file) || throw GradleException("could not move $tmp to $file")
	}
}

dependencies {
	// 1.6.1 is the newest version we can use that won't complain about minSdk version,
	// and also doesn't collide kotlin versions with com.gladed.androidgitversion.
	// Will replace with a different plugin soon.
	implementation("androidx.appcompat:appcompat:1.7.1")
	implementation("androidx.documentfile:documentfile:1.1.0")
	implementation("com.google.protobuf:protobuf-javalite:4.35.1")
	// Save sync: emusync's AAR (see downloadEmusync).
	implementation(files(emusyncAar).builtBy(downloadEmusync))
}

protobuf {
	protoc {
		artifact = "com.google.protobuf:protoc:3.25.3"
	}

	generateProtoTasks {
		all().forEach { task ->
			task.builtins {
				register("java") {
					option("lite")
				}
			}
		}
	}
}

android {
	flavorDimensions.add("variant")
	namespace = "org.ppsspp.ppsspp"
	signingConfigs {
		getByName("debug") {
			storeFile = file("debug.keystore")
		}
		create("optimized") {
			storeFile = file("debug.keystore")
		}
		if (project.hasProperty("RELEASE_STORE_FILE")) {
			create("release") {
				storeFile = file(project.property("RELEASE_STORE_FILE") as String)
				storePassword = project.property("RELEASE_STORE_PASSWORD") as String
				keyAlias = project.property("RELEASE_KEY_ALIAS") as String
				keyPassword = project.property("RELEASE_KEY_PASSWORD") as String
			}
		}
	}

	compileSdk = 37
	ndkVersion = "29.0.14206865"

	compileOptions {
		sourceCompatibility = JavaVersion.VERSION_17
		targetCompatibility = JavaVersion.VERSION_17
	}

	lint {
		baseline = file("lint-baseline.xml")
	}

	defaultConfig {
		applicationId = "org.ppsspp.ppsspp"
		// Access the git version info via the extension
		if (gitVersionName != "unknown") {
			println("INFO: Overriding Android Version Name, Code: $gitVersionName $gitVersionCode")
			versionName = gitVersionName
			versionCode = gitVersionCode
		} else {
			println("(not using these:) Android Version Name, Code: $gitVersionName $gitVersionCode")
		}
		file("versionname.txt").writeText(gitVersionName)
		file("versioncode.txt").writeText(gitVersionCode.toString())

		minSdk = 21
		targetSdk = 37
		if (project.hasProperty("ANDROID_VERSION_CODE") && project.hasProperty("ANDROID_VERSION_NAME")) {
			versionCode = (project.property("ANDROID_VERSION_CODE") as String).toInt()
			versionName = project.property("ANDROID_VERSION_NAME") as String
		}
	}
	buildTypes {
		getByName("debug") {
			isDefault = true
			isMinifyEnabled = false
			isJniDebuggable = true
			signingConfig = signingConfigs.getByName("debug")
		}
		create("optimized") {
			isMinifyEnabled = false
			isJniDebuggable = true
			signingConfig = signingConfigs.getByName("debug")
		}
		getByName("release") {
			isMinifyEnabled = false
			if (project.hasProperty("RELEASE_STORE_FILE")) {
				signingConfig = signingConfigs.getByName("release")
			} else {
				println("WARNING: RELEASE_STORE_FILE is missing. Release builds will be unusable.")
			}
		}
	}
	externalNativeBuild {
		cmake {
			path = file("../CMakeLists.txt")
		}
	}
	packaging {
		jniLibs.useLegacyPackaging = true
		// Optional: -PlibrashaderAbi=<abi> (arm64-v8a|armeabi-v7a|x86_64) excludes the other ABIs
		// from packaging. When absent, all present jniLibs are packaged (release flavors prune by
		// abiFilters). Example: -Pandroid.injected.build.abi=arm64-v8a -PlibrashaderAbi=arm64-v8a
		if (project.hasProperty("librashaderAbi")) {
			val keepAbi = project.property("librashaderAbi") as String
			val allAbis = listOf("armeabi-v7a", "arm64-v8a", "x86_64")
			require(keepAbi in allAbis) { "librashaderAbi must be one of $allAbis, got '$keepAbi'" }
			val excludeAbis = allAbis.filter { it != keepAbi }
			jniLibs {
				excludes += excludeAbis.map { "**/$it/librashader.so" }
			}
		}
	}
	sourceSets {
		getByName("main") {
			manifest.srcFile("AndroidManifest.xml")
			res.directories.add("res")
			java.directories.add("src")
			aidl.directories.add("src")
			resources.directories.add("src")
			assets.directories.add("../assets")
		}
		create("normal") {
			res.directories.add("normal/res")
		}
		create("gold") {
			res.directories.add("gold/res")
		}
		create("vr") {
			res.directories.add("normal/res")
			manifest.srcFile("VRManifest.xml")
		}
		create("legacy") {
			res.directories.add("legacy/res")
		}
	}
	productFlavors {
		create("normal") {
			isDefault = true
			applicationId = "org.ppsspp.ppsspp"
			dimension = "variant"
			externalNativeBuild {
				cmake {
					// **FIXED**: Using simple "listOf"
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a", "x86_64"))
				// debugSymbolLevel = "FULL"  // These don't actually help much in the Google Play crash report. We do still have symbols locally.
			}
		}
		create("gold") {
			applicationId = "org.ppsspp.ppssppgold"
			dimension = "variant"
			externalNativeBuild {
				cmake {
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared",
						"-DGOLD=TRUE"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a", "x86_64"))
				// debugSymbolLevel = "FULL"
			}
		}
		create("legacy") {
			applicationId = "org.ppsspp.ppsspplegacy"
			dimension = "variant"
			targetSdk = 29  // To avoid scoped storage, which is the point of the legacy APK
			externalNativeBuild {
				cmake {
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared",
						"-DANDROID_LEGACY=TRUE"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a"))
				// debugSymbolLevel = "FULL"
			}
		}
		create("vr") {
			applicationId = "org.ppsspp.ppssppvr"
			dimension = "variant"
			targetSdk = 29  // To avoid scoped storage, which doesn't work properly on Oculus
			externalNativeBuild {
				cmake {
					arguments.addAll(listOf(
						"-DANDROID=true",
						"-DANDROID_PLATFORM=android-21",
						"-DANDROID_TOOLCHAIN=clang",
						"-DANDROID_CPP_FEATURES=",
						"-DANDROID_STL=c++_shared",
						"-DOPENXR=TRUE",
						"-DANDROID_LEGACY=TRUE"
					))
				}
			}
			ndk {
				abiFilters.addAll(listOf("arm64-v8a"))
			}
		}
	}
	buildFeatures {
		aidl = true
		buildConfig = true
	}
}
androidComponents {
	beforeVariants(selector().all()) { variantBuilder ->
		// **FIXED**: Using simple "setOf"
		val enabledVariants = setOf(
			"normalDebug", "normalOptimized", "normalRelease",
			"goldDebug", "goldRelease",
			"vrDebug", "vrOptimized", "vrRelease",
			"legacyDebug", "legacyOptimized", "legacyRelease"
		)
		variantBuilder.enable = variantBuilder.name in enabledVariants
	}
}

/*
afterEvaluate {
	android.sourceSets.getByName("main").assets.srcDirs.forEach {
		println(it)
	}
}*/

// Use the below to get a lot of deprecation warnings.
// Not really useful to fix most of them though, as the old paths are often needed to support
// old Android versions.
/*
tasks.withType<JavaCompile> {
	options.compilerArgs.addAll(listOf("-Xlint:deprecation"))
}
*/
