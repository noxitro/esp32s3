# ESP32-S3 3キー HIDマクロパッド

3つのボタンと3つの青色LEDを持つUSB HIDキーボードデバイス。各キーを押すと設定した文字列をPC/Androidに入力します。

## 機能

- ボタン押下中は対応するLEDが点灯
- 各ボタンの押下で任意の文字列をUSB HIDキーボードとして送信
- **configモード**: 3つのキーを同時に3秒以上長押しでトグル
  - シリアルモニタ(115200 baud)からコマンドで文字列を設定
  - 設定はNVSに保存され、電源を切っても保持
- configモード中は3つのLEDが一斉点滅

## ピン割り当て(XIAO ESP32-S3 マクロパッド基板)

| キー | ボタンGPIO (XIAOパッド) | LED GPIO (XIAOパッド) |
|---|---|---|
| 1 (上) | 1 (D0) | 4 (D3) |
| 2 (中) | 2 (D1) | 5 (D4) |
| 3 (下) | 3 (D2) | 6 (D5) |

ボタンは内部プルアップ・アクティブLOW(GNDへ接続)。LEDは220Ω抵抗を介して接続。

## 設定方法(3通り)

configモードに入る: **3キー同時3秒長押し** または シリアルで `config` 送信。configモード中はLEDが一斉点滅し、**WiFiアクセスポイントが起動**します。

1. **Web UI(WiFi)**: スマホ/PCをWiFi `MacroPad-Config`(パスワード `macropad123`)に接続し、ブラウザで `http://192.168.4.1/` を開く
2. **Web UI(WebSerial)**: [webui/index.html](webui/index.html) をChrome/Edgeで開き「シリアル接続」(configモードに自動で入ります)
3. **シリアルモニタ**(115200 baud):

| コマンド | 動作 |
|---|---|
| `config` | (通常モード時)configモードに入る |
| `1=<マクロ>` | キー1のマクロを設定(2, 3も同様)。即NVS保存 |
| `show` | 現在の設定を表示(通常モードでも使用可) |
| `test1` / `test2` / `test3` | 該当キーのマクロをその場で実行(動作確認用) |
| `exit` | 通常モードへ戻る(WiFi AP停止) |

## マクロ書式

| 書式 | 意味 | 例 |
|---|---|---|
| そのままの文字 | タイプされる(ASCII英数記号のみ。日本語直接入力は不可) | `hello` |
| `{ENTER}` `{TAB}` `{ESC}` `{F1}`〜`{F12}` `{UP}`等 | 特殊キー | `ls{ENTER}` |
| `{CTRL+C}` `{WIN+R}` `{CMD+SPACE}` | 修飾キー同時押し(CTRL/SHIFT/ALT/WIN/GUI/CMD/OPT) | `{CTRL+SHIFT+T}` |
| `{WAIT:500}` | 500ms待機(マクロ内の間合い) | `{WIN+R}{WAIT:400}notepad{ENTER}` |
| `{ZENHAN}` `{HENKAN}` `{MUHENKAN}` `{HIRAGANA}` | JIS IMEキー(Windows日本語切替は`{ZENHAN}`) | |
| `{EISU}` `{KANA}` | Mac の英数/かなキー | |
| `{BROWSER}` `{CALC}` `{VOL_UP}` `{VOL_DOWN}` `{MUTE}` `{MEDIA_PLAY}` `{AC_HOME}` `{AC_BACK}` | メディア/ランチャーキー(Consumer Control) | |

例:
- Windowsでブラウザを開いてURLへ: `{WIN+R}{WAIT:400}https://example.com{ENTER}`
- IME切替(Windows): `{ZENHAN}` / (Mac): `{EISU}` `{KANA}` / (Android): `{CTRL+SPACE}`
- 定型文+改行: `Best regards,{ENTER}Arlen`

**注意(仕様)**: 文字列は押下エッジで送信されるため、configモード突入のための長押し開始時に各キーの文字列が1回送信されます。実運用ではconfigモード切替はテキスト入力欄以外にフォーカスがある状態で行ってください。

## Wokwiシミュレーション

### ブラウザで試す

1. https://wokwi.com/projects/new/esp32-s3 を開く
2. `sketch.ino` と `diagram.json` の内容をそれぞれのタブに貼り付け
3. ▶ で実行。ボタンをクリックするとLED点灯+シリアルモニタに `SENT[n]: <text>` が出る

> **制約**: WokwiはUSB HIDのホスト側シミュレーションに対応していないため、
> 送信文字列はシリアル出力で確認します。HID動作の確認は実機で行ってください。

### 自動テスト(wokwi-cli)

前提: [arduino-cli](https://arduino.github.io/arduino-cli/)、[wokwi-cli](https://docs.wokwi.com/wokwi-ci/getting-started)、esp32コア(`arduino-cli core install esp32:esp32` — 要boardマネージャURL設定)、環境変数 `WOKWI_CLI_TOKEN`(https://wokwi.com/dashboard/ci で無料発行)。

```bash
./build.ps1
```

```bash
wokwi-cli --scenario test/scenario.yaml .
```

シナリオ内容: 起動確認 → 各ボタンで `SENT[n]` 確認 → 3キー長押しでconfigモード → `2=hello` 設定 → exit → キー2で `hello` 送信を確認。

## 実機への書き込み(HID有効)

Arduino IDEの場合:

1. ボード: **XIAO_ESP32S3**
2. **USB Mode: "USB-OTG (TinyUSB)"** ← これを選ばないとHIDが無効のままです(USB CDC On Boot: Enabled も推奨)
3. 書き込み後、PC/AndroidにUSB接続するとキーボードとして認識されます
4. OTGファーム動作中は自動書き込みが失敗することがある → XIAOの **B(BOOT)ボタンを押しながらUSBを挿す**とダウンロードモードに入れます

arduino-cliの場合:

```bash
./build.ps1 -Hid
```

その後 `arduino-cli upload -p <COMポート> --fqbn esp32:esp32:esp32s3:USBMode=default build/sketch` で書き込み。

Androidでは USB-C ケーブルで直結(OTG対応端末)。メモ帳アプリ等を開いてキーを押すと文字列が入力されます。

## 手動テストチェックリスト

- [ ] 各ボタン押下中、対応LEDが点灯する
- [ ] 各ボタン押下で文字列が送信される(実機: テキストエディタに入力される / Wokwi: シリアルに `SENT[n]`)
- [ ] 3キー同時3秒長押しで `CONFIG MODE` になりLEDが一斉点滅
- [ ] `1=abc` → `OK: key1 = abc`、`show` で反映確認
- [ ] `exit` で通常モードへ復帰、キー1で `abc` が送信される
- [ ] リセット(電源断)後も設定が保持されている(NVS)— **実機のみ**。Wokwiの「Restart」はフラッシュを初期化して再書き込みするため、シミュレーション上では設定は残りません(仕様)
