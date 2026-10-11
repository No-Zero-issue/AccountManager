plugins {
    id("com.android.application")
}

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
        versionCode = 1
        versionName = "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}


dependencies {
    implementation("com.google.zxing:core:3.5.3")   // Apache-2.0：二维码解码/编码
    implementation("com.journeyapps:zxing-android-embedded:4.3.0")   // Apache-2.0：相机扫码界面
    implementation("androidx.core:core:1.13.1")          // 补：zxing-android-embedded 用了 ContextCompat 却没声明
    implementation("androidx.appcompat:appcompat:1.7.0")  // 补：其 CaptureActivity 需要
    testImplementation("junit:junit:4.13.2")
}
