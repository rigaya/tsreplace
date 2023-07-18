
# tsreplace
by rigaya

**注意: 建設中です!**

tsの動画部分のみの置き換えを行うツールです。

## 想定動作環境
### Windows
Windows 10/11 (x86/x64)  

## 使用方法
```
tsreplace -i <入力tsファイル> --video-replace <置き換え映像ファイル> -o <出力tsファイル>
```

## ソースコードについて
- MITライセンスです。
- 本ソフトウェアでは、
  [ffmpeg](https://ffmpeg.org/)
  を使用しています。
- 本ソフトウェア作成に当たり、
  [tsreadex](https://github.com/xtne6f/tsreadex)を参考にさせていただきました。
  どうもありがとうございました。


### ソースの構成
Windows ... VCビルド  

文字コード: UTF-8-BOM  
改行: CRLF  
インデント: 空白x4  
