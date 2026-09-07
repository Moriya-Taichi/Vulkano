# ライブラリの公開

[Android公式の公開手順](https://developer.android.com/build/publish-library/upload-library?hl=ja)に沿って、Release variantからMavenパブリケーションを作ります。AARに加え、依存関係を記述したPOM、Gradle Module Metadata、Sources JAR、DokkaによるAPIドキュメントJARを生成します。

公開先はMaven Centralを想定しています。**この設定の追加だけでは公開されません。** Central Portalでのアカウント・namespace登録、署名鍵の設定が必要です。本体には[Apache License 2.0](../LICENSE)を適用しています。

## 公開座標

```text
io.github.moriya-taichi:vulkano:0.1.0
```

上記は最初のリリース候補の座標です。公開完了まではREADMEのローカル導入手順を使用してください。ビルド時の既定値は`0.1.0-SNAPSHOT`で、`-PVERSION_NAME=...`で切り替えます。

以前のローカル座標`dev.vulkano:vulkano`から、GitHubアカウントで所有権を確認できるgroupIdへ変更しています。Kotlinの`import dev.vulkano.*`やAndroid namespaceは変わりません。Central Portal上では`io.github.moriya-taichi`の使用権限を確認してください。

## 認証なしで配布内容を確認する

リポジトリのルートで実行します。

```sh
./gradlew :vulkano:generateRepo
python3 tools/verify-publication.py
```

出力先は以下です。

- Mavenリポジトリ：`vulkano/build/repository/`
- リポジトリZIP：`vulkano/build/distributions/vulkano-0.1.0-SNAPSHOT-maven.zip`

ZIPを解凍すると`vulkano-repository/`が得られます。これを利用側のプロジェクトルートに置き、`settings.gradle.kts`の依存リポジトリへ追加すれば、Maven座標で参照できます。Kotlin標準ライブラリなどを取得するため、既存の`google()`と`mavenCentral()`も残してください。

```kotlin
maven { url = uri("vulkano-repository") }
```

ZIPはフォルダ型Mavenリポジトリの配布・確認用です。Central Portalへアップロードするバンドルは、後述のGradleプラグインが別途生成します。

CIもこのタスクを実行し、POMの座標と依存関係、両ABIのNativeライブラリ、ライセンス通知、Sources、APIドキュメント、チェックサム、ZIPを検査します。ローカルプレビューは未署名でも作成できます。POMには`gradle.properties`で設定したApache License 2.0の名称とURLを記載します。

## 初回公開の準備

1. [Central Portal](https://central.sonatype.com/)へ登録し、`io.github.moriya-taichi`のnamespaceを確認します。
2. PortalのUser Tokenを生成します。通常のログインパスワードではなく、トークンに含まれるusername/passwordを使用します。
3. OpenPGP署名鍵を用意し、公開鍵を公開します。手順は[Centralの署名ガイド](https://central.sonatype.org/publish/requirements/gpg/)を参照してください。
4. GitHubのEnvironment `maven-central`を作成し、次のSecretsを設定します。秘密鍵とトークンはGitやチャットへ貼り付けないでください。

| 種類 | 名前 | 内容 |
| --- | --- | --- |
| Secret | `MAVEN_CENTRAL_USERNAME` | Portal User Tokenのusername |
| Secret | `MAVEN_CENTRAL_PASSWORD` | Portal User Tokenのpassword |
| Secret | `SIGNING_KEY` | ASCII armor形式でエクスポートしたOpenPGP秘密鍵全体 |
| Secret | `SIGNING_KEY_PASSWORD` | 秘密鍵のパスフレーズ |

署名鍵はパスフレーズ付きで用意します。VMA・SPIRV-Reflect・SPIR-V Headersの既存の著作権・ライセンス通知は、AAR内の`classes.jar`とSources JARに同梱します。本体の`LICENSE`も同梱します。ライセンス名とURLはリポジトリで管理するため、GitHub Variablesの設定は不要です。

## リリースする

1. 公開するコミットのCIを確認し、`v0.1.0`など、公開バージョンに一致するタグを作成します。初回公開前に[Android実機検証](android-validation.md)の未確認項目も確認してください。
2. GitHub Actionsの **Stage Maven Central release** を、そのタグを選んで手動実行します。入力`version`には`0.1.0`を指定します。
3. ワークフローが署名済みリポジトリをビルド・検査し、Central Portalへアップロードします。タグとの不一致、SNAPSHOT、ライセンスや必須認証情報の欠落があれば公開処理を開始しません。
4. PortalのDeploymentsで検証結果を確認し、対象のDeploymentをPublishします。ワークフローは自動公開を行いません。
5. Maven Centralから解決できることを確認してから、READMEの導入方法を以下へ切り替えます。

```kotlin
// settings.gradle.ktsの依存リポジトリにmavenCentral()を設定
implementation("io.github.moriya-taichi:vulkano:0.1.0")
```

公開済みバージョンは上書きできないため、修正時は新しいバージョンを使います。[Centralの公開要件](https://central.sonatype.org/publish/requirements/)

ローカルからPortalへアップロードする場合は、同じ設定を`~/.gradle/gradle.properties`または`ORG_GRADLE_PROJECT_...`環境変数で渡し、次を実行します。プロパティ名はワークフローの`env`を参照してください。

```sh
./gradlew :vulkano:generateRepo -PVERSION_NAME=0.1.0 -PcentralRelease=true
python3 tools/verify-publication.py --version 0.1.0 --release
./gradlew :vulkano:publishToMavenCentral -PVERSION_NAME=0.1.0 -PcentralRelease=true
```

Androidのパブリケーションは`maven-publish`で定義し、Central Portalとの通信・署名連携にVanniktechのbaseプラグインを使用します。現行のGradle 8.11.1とAGP 8.9.2に対応する`0.34.0`へ固定しています。[プラグインの公開手順](https://vanniktech.github.io/gradle-maven-publish-plugin/central/)
