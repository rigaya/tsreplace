
# tsreplace
by rigaya

tsの映像部分のみの置き換えを行うツールです。音声含め、他のパケットはそのままコピーします。

## 想定動作環境
### Windows
Windows 10/11 (x86/x64)  

### Linux
Ubuntu 20.04/22.04 (x64) ほか

## 使用方法
まず、オリジナルのtsをエンコードします。出力ファイルはmp4,mkv,ts等、timstampを保持できる形式にします(raw ES不可)。のちに作る映像置き換えtsファイルのシークを円滑にするため、GOP長はデフォルトより短めのほうがよいと思います。

```
QSVEncC64.exe -i <入力tsファイル> [インタレ解除等他のオプション] --gop-len 60 -o <置き換え映像ファイル>
```

次にtsreplaceを使って、エンコードした映像に置き換えたtsを作成します。
```
tsreplace.exe -i <入力tsファイル> -r <置き換え映像ファイル> -o <出力tsファイル>
```

また、下記のように、2段階に分けずエンコードしながら置き換えを行うこともできます。

```
QSVEncC64.exe -i <入力tsファイル> [インタレ解除等他のオプション] --gop-len 60 --output-format mpegts -o - | tsreplace.exe -i <入力tsファイル> -r - -o <出力tsファイル>
```

## オプション

### -o, --output &lt;string&gt;
出力tsファイルのファイルパス。

### -i, --input &lt;string&gt;
入力tsファイルのファイルパス。標準入力からは受け取れません。

### -r, --replace &lt;string&gt;
置き換える映像の入っているファイルのパス。"-"で標準入力になります。

timestampを保持できるコンテナ入りの映像を想定しており、raw ES等は考慮しません。

### --start-point &lt;string&gt;
置き換え時の時刻の起点を指定します。

- **パラメータ**
  - keyframe (デフォルト)  
    最初のキーフレームの時刻を起点とします。tsファイルを[QSVEncC](https://github.com/rigaya/QSVEnc)/[NVEncC](https://github.com/rigaya/NVEnc)/[VCEEncC](https://github.com/rigaya/VCEEnc)/[rkmppenc](https://github.com/rigaya/rkmppenc)でエンコードした場合に使用します。

  - firstframe  
    最初のフレームの時刻を起点とします。tsファイルをlwinput.auiで読み込みエンコードした場合に使用します。

  - firstpacket  
    映像・音声の最初のパケットの時刻を起点とします。tsファイルをffmpegでエンコードした場合に使用します。

### --add-aud
映像パケットごとにAUDを自動挿入します。(デフォルト：オン)

### --no-add-aud
映像パケットごとのAUDの自動挿入を無効にします。

### --add-headers
映像のキーフレームごとにヘッダを自動挿入します。(デフォルト：オン)

### --no-add-headers
映像のキーフレームごとのヘッダの自動挿入を無効にします。

## 制限事項

下記については、対応予定はありません。

- カット編集等の行われたtsおよび映像ファイルは、置き換え時の同期が困難なため非対応です。
- 音声・字幕等、映像以外に関わる処理
- 入力tsファイルの制限
  - 188byte tsのみ対応しています。
  - 解像度変更のあるtsについては動作は検証しません。
- 置き換え映像ファイルの制限
  - H.264/HEVCの置き換えのみ対応します。
  - 置き換えファイルはtimestampを保持できるコンテナ入りの映像を想定しています。
    ESでの動作は検証しません。

## ソースコードについて
- MITライセンスです。
- 本ソフトウェアでは、
  [ffmpeg](https://ffmpeg.org/)
  を使用しています。

## 謝辞
本ソフトウェア作成に当たり、
[tsreadex](https://github.com/xtne6f/tsreadex)を大変参考にさせていただきました。  
どうもありがとうございました。


## ソースの構成
Windows ... VCビルド  

文字コード: UTF-8-BOM  
改行: CRLF  
インデント: 空白x4  
