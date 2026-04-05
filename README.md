# RippleTrim

`RippleTrim` は AviUtl2 用のプラグインです。選択したオブジェクトの範囲を詰め、後続オブジェクトを前へ寄せます。

## 導入方法

1. `RippleTrim.aux2` を AviUtl2 のプラグイン配置先へ置きます。
2. 必要に応じて AviUtl2 を再起動します。

## AviUtl2 上での簡単な使い方

1. タイムラインで詰めたい範囲のオブジェクトを選択します。
2. タイムラインの編集メニュー、またはオブジェクトメニューから `リップル削除(重なり短縮)` を実行します。
3. 選択範囲が削除され、後ろのオブジェクトが前へ詰まります。

## ディレクトリ構成

- `src/`: プラグイン本体
- `external/aviutl2-plugin-sdk/`: AviUtl2 SDK の git submodule
- `scripts/build.ps1`: ローカルビルド用スクリプト
- `.github/workflows/build.yml`: CI ビルド

## SDK の入れ方

この repo は `aviutl2/aviutl2_sdk_mirror` を submodule として入れる前提です。

```powershell
git submodule add https://github.com/aviutl2/aviutl2_sdk_mirror.git external/aviutl2-plugin-sdk
git submodule update --init --recursive
```

## ビルド

```powershell
pwsh ./scripts/build.ps1
```

CMake を直接使う場合:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成物は `build/Release/RippleTrim.aux2` です。

## GitHub Actions

次のタイミングでビルドが走ります。

- push
- pull request
- 手動実行
- `repository_dispatch` の `sdk-updated`
