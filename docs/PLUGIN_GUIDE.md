# SharkSDR プラグイン開発ガイド

SharkSDR のプラグインは、`plugins/` フォルダに置かれた共有ライブラリ（`.dll` / `.so` / `.dylib`）です。
起動時に自動で読み込まれ、メニューの **Plugins → Plugin manager** から実行中に有効／無効を切り替えられます。

プラグインはアプリ本体と同じ `SharkSDR_core` ライブラリにリンクするので、**同じ ImGui コンテキスト**・同じ DSP 部品・同じウィジェットをそのまま使えます。

## 1. 最小のプラグイン

```cpp
// my_plugin.cpp
#include <SharkSDR/plugin.h>

class MyPlugin : public SharkSDR::Plugin {
public:
    void init(SharkSDR::Host& host) override {
        host.registerPanel("My panel", [&host] {
            ImGui::Text("VFO: %.0f Hz", host.frequency());
        });
    }
};

//           クラス      名前          バージョン  作者  説明
SHARKSDR_PLUGIN(MyPlugin, "My plugin", "0.1.0", "Me", "Shows the VFO frequency")
```

```cmake
# CMakeLists.txt
sharksdr_add_plugin(my_plugin SOURCES my_plugin.cpp)
```

`SHARKSDR_PLUGIN` マクロが、アプリが探す C のエントリポイント（API バージョン、ビルド署名、情報、生成・破棄）を定義します。

## 2. ビルド方法

どちらの方法でも、出力は `build/bin/plugins/` に置かれます。

**リポジトリ内に置く**: `plugins/hello_example` をコピーしてフォルダ名とターゲット名を変え、`plugins/CMakeLists.txt` の `SHARKSDR_BUNDLED_PLUGINS` に追加します。

**リポジトリの外に置く**: 本体のソースを変更せずにビルドに組み込めます。

```sh
cmake --preset release -DSHARKSDR_EXTRA_PLUGINS="/path/to/my_plugin;/path/to/another"
cmake --build --preset release
```

完成したプラグインファイルは、実行ファイル横の `plugins/`、ユーザー設定フォルダの `plugins/`、または `--plugins DIR` で指定したフォルダに置けば読み込まれます。

> **ABI について**: プラグインは C++ のオブジェクト（`std::function`、`std::shared_ptr`、`nlohmann::json` など）をアプリとやり取りするため、
> アプリと同じ系統のコンパイラ・同じ ImGui バージョンでビルドする必要があります（MSVC の場合は Debug/Release も一致）。
> 一致しない場合はプラグインマネージャに理由が表示され、読み込まれません。

## 3. 拡張ポイント

`init()` に渡される `SharkSDR::Host`（[`core/include/SharkSDR/host.h`](../core/include/SharkSDR/host.h)）から登録します。
登録したものは **プラグインのアンロード時に自動で解除** されます（解除の直後に `shutdown()` が呼ばれます）。

| メソッド | 用途 | 例 |
|---|---|---|
| `registerSource(std::shared_ptr<Source>)` | IQ ソース（ハードウェア・ネットワーク・ファイル…） | `signal_generator`, `rtl_tcp_source`, `soapy_source` |
| `registerDemodulator(name, factory)` | 復調モード（Radio パネルにボタンが増える） | `am_sync_demod` |
| `registerPanel(title, draw)` | サイドバーの折りたたみパネル | `recorder`, `bookmarks` |
| `registerMenu(draw)` | Tools メニューの項目 | `bookmarks`, `hello_example` |
| `registerFrameCallback(fn)` | 毎フレーム呼ばれる（独自ウィンドウ用） | `hello_example` |
| `registerSpectrumOverlay(draw)` | スペクトラム上への描画・クリック処理 | `bookmarks` |
| `addIqTap(cb)` / `addAudioTap(cb)` | IQ・復調音声の取得（DSP スレッドで呼ばれる） | `recorder` |

受信機の操作（`start/stop`、`frequency/setFrequency`、`mode/setMode`、`bandwidth/setBandwidth`、`signalLevel`、`sampleRate`、`centerFrequency` など）、
プラグイン専用の設定 `config()`（JSON、終了時に自動保存）、`log()`、`dataDirectory()` も Host から使えます。

## 4. ソースを作る

[`core/include/SharkSDR/source.h`](../core/include/SharkSDR/source.h) の `SharkSDR::Source` を実装します。

```cpp
class MySource : public SharkSDR::Source {
public:
    std::string name() const override { return "My SDR"; }

    bool start(SharkSDR::SampleSink& sink) override {
        quit_ = false;
        thread_ = std::thread([this, &sink] {
            std::vector<SharkSDR::Complex> buf(16384);
            while (!quit_) {
                // ... デバイスから読み取り、振幅 1.0 がフルスケールの複素数に変換 ...
                sink.pushSamples(buf.data(), buf.size());
            }
        });
        return true;              // 失敗時は false を返し、lastError() で理由を返す
    }
    void stop() override {         // 戻る前にストリーミングを完全に止めること
        quit_ = true;
        if (thread_.joinable()) thread_.join();
    }
    double sampleRate() const override { return rate_; }      // どのスレッドからも呼ばれる
    void tune(double hz) override { /* ハードウェアを再同調 */ }
    void drawSettings(bool running) override { /* ImGui で設定 UI */ }

private:
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<double> rate_{2.4e6};
};
```

- ストリームが自然に終わった（ファイル終端・切断など）ときは `sink.sourceStopped("理由")` を呼びます。アプリが UI スレッドで `stop()` を呼び直します。
- 周波数を変えられないソース（ファイル再生など）は `tunable()` で `false` を返し、`frequency()` で中心周波数を返します。
- サンプルレートは実行中に変えて構いません（`sampleRate()` の値が変わると DSP が追従します）。

## 5. 復調モードを作る

[`core/include/SharkSDR/demodulator.h`](../core/include/SharkSDR/demodulator.h) の `SharkSDR::Demodulator` を実装します。
周波数シフト・間引き・リサンプリング・チャンネルフィルタ・スケルチ・オーディオ LPF とリサンプリングはアプリ側が行うので、
**復調処理そのものだけ** を書けば済みます。

```cpp
class MyDemod : public SharkSDR::Demodulator {
public:
    double channelRate() const override { return 24000.0; }   // 入力 IQ のレート（出力音声も同じレート）
    double defaultBandwidth() const override { return 6000.0; }
    SharkSDR::BandShape bandShape() const override { return SharkSDR::BandShape::Center; }
    double audioCutoff() const override { return 3000.0; }     // これより上はアプリがカット

    void process(const SharkSDR::Complex* in, size_t n, SharkSDR::Stereo* out) override {
        for (size_t i = 0; i < n; i++) out[i] = SharkSDR::Stereo(std::abs(in[i]));
    }
};

host.registerDemodulator("MYMODE", [] { return std::make_unique<MyDemod>(); });
```

モード固有の設定は `drawSettings()`（Radio パネル内に表示）、保存は `loadState()` / `saveState()` で行います。
これらの呼び出し中は DSP スレッドが止まっているので、ロックは不要です。

## 6. スレッドのルール

| 呼ばれる場所 | 対象 |
|---|---|
| UI スレッド | `init` / `shutdown`、パネル・メニュー・フレームコールバック・オーバーレイ、`Source::start/stop/tune/drawSettings`、Host のほぼ全メソッド |
| DSP スレッド | IQ／オーディオタップ、`Demodulator::process` |
| ソース独自のスレッド | `SampleSink::pushSamples` / `sourceStopped` |

タップのコールバックは短く保ち、ブロックしないでください（ディスク書き込みなどは別スレッドへ。`plugins/recorder` 参照）。
`removeTap()` から戻った後は、そのコールバックが呼ばれないことが保証されます。

## 7. 使える部品

プラグインは `SharkSDR_core` の以下の部品をそのまま使えます。

- **DSP**（`SharkSDR/dsp/*.h`）: `Fft`、窓関数、`designLowpass` / `designComplexBandpass`（Kaiser 窓 FIR 設計）、`FirFilter`、`ComplexFirFilter`、`FirDecimator`、`PowerDecimator`、
  `Resampler<T>`（任意比の多相リサンプラ）、`Nco`、`Agc`、`RingBuffer`、`OnePole`、`DcBlocker`、
  `Receiver`（周波数シフトから復調・音声化までの受信チェーン一式。IQ タップと組み合わせて **追加の VFO** を作れます）、
  `createBuiltinDemodulator("NFM")` など組み込み復調器の生成
- **UI**（`SharkSDR/ui/*.h`）: `FrequencyDial`、`InputFrequency`、`PathPicker`（ファイル／フォルダ選択ダイアログ）、`LevelMeter`、`ToggleButton`、`HelpMarker`、
  `formatFrequency` などの書式関数、`Texture`（ImGui::Image 用の GPU テクスチャ）、`fonts()`
- **レイアウト**: サイドバーの並びを本体とそろえるための部品です。
  - `LabeledRow("Gain")`: 左にラベル、右にウィジェット（ラベル列の幅は全パネル共通）
  - `CheckboxRow("Squelch", &on)`: ラベルの前にチェックボックスを付けた行
  - `SameLineColumn()`: チェックボックスやラジオボタンを 2 列にそろえる
- **スライダー**: `ui::SliderFloat` / `ui::SliderInt` / `ui::VSliderFloat` は ImGui の同名関数と同じ引数です。
  ダブルクリック（または Ctrl+クリック）で数値を直接入力できます（`12.5k`、`-40 dB` のような単位付きも可、範囲内に丸めます）。
  入力が意味を持たないスライダー（再生位置など）は `ImGuiSliderFlags_NoInput` を付けてください。

```cpp
SharkSDR::ui::LabeledRow("Gain");
if (SharkSDR::ui::SliderFloat("##gain", &gainDb, 0.0f, 49.6f, "%.1f dB")) applyGain();
ImGui::Checkbox("Bias-T", &biasTee);
SharkSDR::ui::SameLineColumn();
ImGui::Checkbox("Offset", &offset);
```
- **ユーティリティ**: `WavWriter` / `WavReader`（`SharkSDR/util/wav.h`）、`TcpSocket`（`SharkSDR/util/net.h`）、パス関数（`SharkSDR/paths.h`）、ログ（`SharkSDR/log.h`）

## 8. 同梱プラグイン（サンプルとして）

| フォルダ | 内容 | 見どころ |
|---|---|---|
| `hello_example` | パネル・メニュー・独自ウィンドウ・オーディオタップ | テンプレート |
| `signal_generator` | 仮想バンドの信号発生器ソース | `Source` の実装、送信側の変調 |
| `file_source` | IQ ファイル再生 | 非同調ソース、`sourceStopped`、`PathPicker` |
| `rtl_tcp_source` | rtl_tcp クライアント | `TcpSocket`、実行中の設定変更 |
| `soapy_source` | SoapySDR | 外部ライブラリへの依存（`LIBRARIES`） |
| `rtlsdr_source` | librtlsdr | 非同期コールバック型ドライバ |
| `am_sync_demod` | 同期 AM（SAM） | `Demodulator` の実装、状態保存 |
| `recorder` | 録音 | タップ、書き込みスレッド |
| `bookmarks` | ブックマーク | スペクトラムオーバーレイ、設定の保存 |
