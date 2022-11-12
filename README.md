# HopStepZ
2022/11/12 Harumakkin

## What is this
RaspberryPiのアドオンボード RaMsxMuse を使用して、MGSデータを再生するときに使用するソフトウェアです。RaMsxMuse(RaSCC)はOPLL、PSG、SCC音源を搭載し MGSDRV.COMを使用することでMGS楽曲データを再生することができます。MGSDRV.COMは本来MSX上で動作するソフトウェアですが、HopStepZ は Z80 CPU と必要な一部のMSX-DOSファンクションをエミュレーションして、MGSDRV.COMを動作させるものです。HopStepZはMSXの完全なエミュレータではなく、MGSDRV.COMとRaMsxMuse(RaSCC)の組み合わせに特化したそれ専用のエミュレータです。大したことはしていません。

## Supported hardware
有効な組み合わせを次の表で示します。
|HopStepZ|RaMsxMuse|RaSCC|
|:-|:-|:-|
|Ver1.10|RaMsxMuse_R10B,R14A|RaSCC_R4A|
Ver1.01|RaMsxMuse_R10B|RaSCC_R2B|

## 使い方
### 用意するもの
- RaspberryPi （Zero WH／Zero 2／RaspberryPi4B）
- WiringPi の更新
- RaMsxMuse R14B アドオンボード 
- RaSCC R4A アドオンボード 
- MGSDRV.COM(Ver3.20) https://gigamix.hatenablog.com/entry/mgsdrv/
- MGS楽曲データファイル

### ビルド方法
```txt
$ git clone https://github.com/cliffgraph/HopStepZ.git
$ make
```
以上で、hopstepz が生成されます。

### 演奏開始
hopstepz、MGSDRV.COM、mgsファイルを同じディレクトリに置いて
```txt
$ ./hopstepz MGSDRV.COM file.mgs
```

### 演奏の止め方
[ctrl]+[c] で止めてください

### 演奏のテンポが遅い場合
SCC の再生はテンポ遅くなる場合があります。RaspberryPi4Bを使用している場合は、CPUの動作クロックを 2GHzにクロックアップしてみてください。ただしRaspberryPi4Bの保証が無くなる様ですので、それが承諾できない場合はクロックアップは行わないでください。

## 履歴
- **Ver 1.00** 2022/01/25 初版 
- **Ver 1.01** 2022/01/29 
音源チップのアクセスタイミングを調整した。RaspberryPi4はクロックアップせずに1.5GHzの動作でOK。ただし、arm_freq=1500、force_turbo=1を追加して、1.5GHz固定で動作させる必要あり
- **Ver 1.10** 2022/11/12      
RaSCC Rev.4A に対応（Rev.2Bには非対応となった）。SCCの楽曲のテンポが遅れてしまう問題をある程度解決した
Ctrl+Cで止めたときに音が残ラ内容にした（終了時にOPLL,SCCのレジスタを初期化するようにした）
CTRL+Cで停止時に小さい音が鳴り続けてしまうのを改善した。




以上
