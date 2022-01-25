# HopStepZ
2022/01/25 Harumakkin


## What is this
RaspberryPiのアドオンボード RaMsxMuse を使用して、MGSデータを再生するときに使用するソフトウェアです。RaMsxMuse(RaSCC)はOPLL、PSG、SCC音源を搭載し MGSDRV.COMを使用することでMGS楽曲データを再生することができます。MGSDRV.COMは本来MSX上で動作するソフトウェアですが、HopStepZ は Z80 CPU と必要な一部のMSX-DOSファンクションをエミュレーションして、MGSDRV.COMを動作させるものです。HopStepZはMSXの完全なエミュレータではなく、MGSDRV.COMとRaMsxMuse(RaSCC)の組み合わせに特化したそれ専用のエミュレータです。大したことはしていません。

## 使い方
### 用意するもの
- RaspberryPi (RaMsxMuseのみの使用であれば RaspberryPi Zero WH、RaMsxMuse + RaSCCの組み合わせでは RaspberryPi 4B + 64ビット版RaspberryPi OS を推奨します）
- WiringPi の更新
- RaMsxMuse R10B アドオンボード 
- RaSCC R2B アドオンボード 
- MGSDRV.COM(Ver3.20) https://gigamix.hatenablog.com/entry/mgsdrv/
- MGS楽曲データファイル

### ビルド方法
```txt
$ git clone 
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
- 2022/01/25 初版 HopStepZ Ver 1.00 by Harumakkin

以上