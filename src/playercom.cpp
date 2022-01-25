#include "stdafx.h"
#include "playercom.h"

/*
	;code:utf-8
	;PLAEYER.COM  #This is a program only on Hopstepz.
	;@Harumakkin

	; MGSDRV's sub-routines
	MGS_SYSCK	:= 0x6010
	MGS_INITM 	:= 0x6013
	MGS_PLYST 	:= 0x6016
	MGS_INTER	:= 0x601f
	MGS_MSVST 	:= 0x6022
	MGS_DATCK	:= 0x6028
	; Hopstepz sub-routines
	ST16MS		:= 0x0039
	WT16MS		:= 0x003A

		org	0x100

	scope playermgs
	begin:
		di
		ld		a, 4
		out		[0xfd], a

		call	MGS_SYSCK
		call	MGS_INITM

	volume_max:
		xor		a,a
		ld		b,0
		call	MGS_MSVST

	start_play:
		ld		de, 0x8000
		ld		b, 0xff
		ld		hl, 0xffff
		call	MGS_PLYST
		ei

	; H.TIMI を使用せずに、メインループ内でMGSDRVのMGS_INTERを周期的に呼び出す。
	; ST16MSとWT16MSは16msの間隔を作り出す処理で、ST16MSを呼び出してからWT16MSの呼び出しが、
	; 16ms経過していない場合は16msが経過するようにWT16MS内でsleep()を呼び出す。
	; ST16MSとWT16MSはHopstepz の独自実装処理です。
	loop:
		call	ST16MS
		call	MGS_INTER
		call	WT16MS
		jr		loop

	endscope ; playermgs
*/
static const uint8_t g_player_com[] = 
{
	0xF3, 0x3E, 0x04, 0xD3, 0xFD, 0xCD, 0x10, 0x60, 0xCD, 0x13, 0x60, 0xAF, 0x06, 0x00, 0xCD, 0x22,
	0x60, 0x11, 0x00, 0x80, 0x06, 0xFF, 0x21, 0xFF, 0xFF, 0xCD, 0x16, 0x60, 0xFB, 0xCD, 0x39, 0x00,
	0xCD, 0x1F, 0x60, 0xCD, 0x3A, 0x00, 0x18, 0xF5,
};

void GetBinaryPlayerCom(std::vector<uint8_t> *pBin)
{
	pBin->clear();
	size_t sz = sizeof(g_player_com);
	for( size_t t = 0; t < sz; ++t)
		pBin->push_back(g_player_com[t]);
	return;
}


