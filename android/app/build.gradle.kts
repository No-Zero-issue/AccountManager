plugins {
    id("com.android.application")
}

// ============================================================ 版本号
// 默认与桌面端共用同一个数字：读仓库里的 AccountManager/version.h（版本唯一来源）
//   versionName = 主.次.修订（和界面显示、EXE 文件属性一致）
//   versionCode = 主*10000 + 次*100 + 修订（Android 要求只增不减）
// 手机端要单独发版时，在 android/gradle.properties 里取消注释
// androidVersionName / androidVersionCode 即可覆盖；只写其中一个也行，
// 另一个仍从 version.h 取；两个都写了就完全不读 version.h。
val androidVersionNameOverride = providers.gradleProperty("androidVersionName").orNull?.takeIf { it.isNotBlank() }
val androidVersionCodeOverride = providers.gradleProperty("androidVersionCode").orNull?.takeIf { it.isNotBlank() }?.toInt()

// 读 version.h 里的某个 #define（文件缺失 / 名字写错都直接报错，避免悄悄发出错版本号）
fun versionNumberFromHeader(name: String): Int {
    val headerFile = rootProject.file("../AccountManager/version.h")
    check(headerFile.isFile) {
        "找不到 ${headerFile.path}：请确认仓库结构完整，或在 android/gradle.properties 里设置 androidVersionName / androidVersionCode"
    }
    val matched = Regex("""^\s*#define\s+$name\s+(\d+)""", RegexOption.MULTILINE).find(headerFile.readText())
    checkNotNull(matched) { "${headerFile.path} 里找不到 #define $name" }
    return matched.groupValues[1].toInt()
}

// 只有需要同步时才读 version.h
val syncedVersionNumbers: List<Int> by lazy {
    listOf("APP_VERSION_MAJOR", "APP_VERSION_MINOR", "APP_VERSION_PATCH").map { versionNumberFromHeader(it) }
}

val appVersionName: String = androidVersionNameOverride
    ?: syncedVersionNumbers.let { (major, minor, patch) -> "$major.$minor.$patch" }
val appVersionCode: Int = androidVersionCodeOverride
    ?: syncedVersionNumbers.let { (major, minor, patch) -> major * 10000 + minor * 100 + patch }

android {
    namespace = "com.accountmanager.steamguard"
    compileSdk = 37
    buildToolsVersion = "36.0.0"

    signingConfigs {
        create("release") {
            storeFile = rootProject.file("keystore/release.jks")
            storePassword = "AccountManager2026!"
            keyAlias = "accountmanager"
            keyPassword = "AccountManager2026!"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }

    defaultConfig {
        applicationId = "com.accountmanager.steamguard"
        minSdk = 26
        targetSdk = 37
        versionCode = appVersionCode
        versionName = appVersionName
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// 产物改名：release -> 2FA.apk，其它变体 -> 2FA-<变体>.apk
// 与 README「版本与发布」记录的产物路径保持一致，发布时不用再手动改名
androidComponents {
    onVariants { variant ->
        val fileName = if (variant.name == "release") "2FA.apk" else "2FA-" + variant.name + ".apk"
        variant.outputs.forEach { output -> output.outputFileName.set(fileName) }
    }
}


dependencies {
    implementation("com.google.zxing:core:3.5.3")   // Apache-2.0：二维码解码/编码
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")   // Apache-2.0：相机扫码界面
    implementation("androidx.core:core:1.13.1")          // 补：zxing-android-embedded 用了 ContextCompat 却没声明
    implementation("androidx.appcompat:appcompat:1.7.0")  // 补：其 CaptureActivity 需要
    testImplementation("junit:junit:4.13.2")
}
