#include "stdafx.h"
#include "msxdef.h"
#include "CZ80MsxDos.h"
#include "CMsxMemSlotSystem.h"
#include "CMsxIoSystem.h"
#include <chrono>
#include <thread>	// for sleep_for

CZ80MsxDos::CZ80MsxDos()
{
	m_pMemSys = nullptr;
	m_pIoSys = nullptr;
	setup();
	ResetCpu();
	return;
}

CZ80MsxDos::~CZ80MsxDos()
{
	// do nothing
	return;
}

void CZ80MsxDos::ResetCpu()
{
	m_bHalt = false;
	m_bIFF1 = m_bIFF2 = false;
	m_IM = INTERRUPTMODE0;
	m_R.Reset();
	return;
}

void CZ80MsxDos::ResetCpu(const z80memaddr_t pc, const z80memaddr_t sp)
{
	m_bHalt = false;
	m_bIFF1 = m_bIFF2 = false;
	m_IM = INTERRUPTMODE0;
	m_R.Reset();
	m_R.PC = pc;
	m_R.SP = sp;
	return;
}

void CZ80MsxDos::Push16(const uint16_t w)
{
	m_pMemSys->Write(--m_R.SP, (w>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (w>>0)&0xff);
	return;
}

uint16_t CZ80MsxDos::Pop16()
{
	uint16_t v;
	v = m_pMemSys->Read(m_R.SP++);
	v |= m_pMemSys->Read(m_R.SP++) << 8;
	return v;
}

z80memaddr_t CZ80MsxDos::GetPC() const
{
	return m_R.PC;
}

z80memaddr_t CZ80MsxDos::GetSP() const
{
	return m_R.SP;
}

void CZ80MsxDos::SetSubSystem(
	CMsxMemSlotSystem *pMem, CMsxIoSystem *pIo)
{
	m_pMemSys = pMem;
	m_pIoSys = pIo;
	return;
}

void CZ80MsxDos::Execution()
{
	OpCodeMachine();
//	InterruptMachine();
	BiosFunctionCall();
	MsxDosFunctionCall();
	ExtendedBiosFunctionCall();
	MainRomFunctionCall();
	return;
}

void CZ80MsxDos::OpCodeMachine()
{
#if !defined(NDEBUG)
	m_PcHist.push_back(m_R);
#endif

	assert(m_pMemSys != nullptr);
	assert(m_pIoSys != nullptr);

	if( !m_bHalt ) {
		m_R.CodePC = m_R.PC++;
		m_R.Code = m_pMemSys->Read(m_R.CodePC);
		auto pFunc = OpCode_Single[m_R.Code].pFunc;
		(this->*pFunc)();
	}
	return;
}

void CZ80MsxDos::InterruptMachine()
{
	uint32_t VSYNC = 16600;			// 16.6ms
	if( m_Tim.GetTime() <= VSYNC )
		return;
	m_Tim.ResetBegin();

	if( !m_bIFF1 )
		return;
	// H.TIMI の呼び出し
	Push16(m_R.PC);
	m_R.PC = 0xFD9F;

	op_DI();

	// カウントアップ
	uint16_t v = m_pMemSys->ReadWord(0xFC9E);
	m_pMemSys->WriteWord(0xFC9E, ++v);
	return;
}

//
const static z80memaddr_t BIOS_RDSLT		= 0x000C;	// 指定スロットのメモリ・１バイト読み出し
const static z80memaddr_t BIOS_WRSLT		= 0x0014;	// 指定スロットのメモリ・１バイト書き込み
const static z80memaddr_t BIOS_CALSL		= 0x001C;	// インタースロット・コール
const static z80memaddr_t BIOS_ENASLT		= 0x0024;	// スロット切り替え
const static z80memaddr_t BIOS_CALLF 		= 0x0030;	// 別のスロットにあるルーチンを呼び出す
const static z80memaddr_t BIOS_KEYINT 		= 0x0038;	// タイマ割り込みの処理ルーチンを実行します。
const static z80memaddr_t BIOS_HSZ_ST16MS	= 0x0039;	// (HopStepZオリジナル)16msウェイトの基点
const static z80memaddr_t BIOS_HSZ_WT16MS	= 0x003A;	// (HopStepZオリジナル)16ms経過まで待つ

const static z80memaddr_t DOS_SYSTEMCALL	= 0x0005;	// DOSシステムコール
const static dosfuncno_t  DOS_CONOUT		= 0x02;		// コンソールへ 1 文字出力
const static dosfuncno_t  DOS_STROUT		= 0x09;		// コンソールへ 文字列の出力（'$'終端）
const static dosfuncno_t  DOS_TERM			= 0x62;		// エラーコードを伴った終了
const static dosfuncno_t  DOS_GENV			= 0x6B;		// 環境変数の獲得
const static dosfuncno_t  DOS_SENV			= 0x6C;		// 環境変数のセット
const static dosfuncno_t  DOS_DOSVER		= 0x6F;		// DOSのバージョン番号の獲得

/** PCの値を見張っていて、特定の位置にPCが来たら対応するファンクションを実行する
*/
void CZ80MsxDos::BiosFunctionCall()
{
	if( 0x0100 <= m_R.PC )
		return;
	switch(m_R.PC)
	{
		case BIOS_HSZ_ST16MS:
		{
			m_Tim16ms.ResetBegin();
			op_RET();
			break;
		}
		case BIOS_HSZ_WT16MS:
		{
			static const uint64_t VSYNCTIME = 16600;	// 16.6ms
			auto et = m_Tim16ms.GetTime();
			if( et < VSYNCTIME ){
				auto def = VSYNCTIME - et;
				std::this_thread::sleep_for(std::chrono::microseconds(def));
			}
			m_Tim16ms.ResetBegin();
			op_RET();
			break;
		}
		case BIOS_RDSLT:
		{
			auto pageNo = static_cast<MEMPAGENO>(m_R.GetHL()/Z80_PAGE_SIZE);
			SLOTNO base, ext;
			m_pMemSys->GetSlot(&base, &ext, pageNo);
			m_pMemSys->ChangeSlot(pageNo, static_cast<SLOTNO>(m_R.A&0x3), static_cast<SLOTNO>((m_R.A>>2)&0x3));
			m_R.A = m_pMemSys->Read(m_R.GetHL());
			m_pMemSys->ChangeSlot(pageNo, base, ext);
			op_DI();
			op_RET();
			break;
		}
		case BIOS_WRSLT:
		{
			DEBUG_BREAK;
			op_DI();
			op_RET();
			break;
		}
		case BIOS_CALSL:
		{
			SLOTNO base = static_cast<SLOTNO>((m_R.IY >> 8) & 0x03);
			SLOTNO ext  = static_cast<SLOTNO>((m_R.IY >>10) & 0x03);
			z80memaddr_t ad = m_R.IX;
			if (base == 0 && ext == 0 && ad == 0x183/*GETCPU*/) {
				m_R.A = 0x00; /*Z80モードで動いてます*/
			//	m_R.A = 0x11; /*R800+DRAMモードで動いてます*/
			}
			else {
				DEBUG_BREAK;
			}
			op_RET() ;
			break;
		}
		case BIOS_ENASLT:
		{
			auto pageNo = static_cast<MEMPAGENO>((m_R.H >> 6) & 0x03);
			SLOTNO base = static_cast<SLOTNO>((m_R.A >> 0) & 0x03);
			SLOTNO ext  = static_cast<SLOTNO>((m_R.A >> 2) & 0x03);
			m_pMemSys->ChangeSlot(pageNo, base, ext);
			m_pMemSys->Write(0xF341 + pageNo, m_R.A);
			op_DI();
			op_RET();
			break;
		}
		case BIOS_CALLF:
		{
			z80memaddr_t retad = Pop16();
			z80memaddr_t v;
			retad++;		// スロット番号は 0x80なので、ひとまず、、無視
			v = m_pMemSys->Read(retad++);
			v |= m_pMemSys->Read(retad++) << 8;
			Push16(retad);
			m_R.PC = v;
			break;
		}
		case BIOS_KEYINT:
		{
			op_RET();
		}
		
		// do nothing
		case 0x0000:
		case DOS_SYSTEMCALL:
			break; 

		default:
		{
			std::wcout
					<< _T("\n **HopStepZ >> Not supported BIOS call address = ")
					<< std::hex << (int)m_R.PC << _T("\n");
			DEBUG_BREAK;
			break;
		}

	}
	return;
}

/** 拡張BIOS（メモリマッパ）
 * @note
 * PCの値を見張っていて、特定の位置にPCが来たら対応するファンクションを実行する
 * 情報源：http://www.ascat.jp/tg/tg2.html
*/
void CZ80MsxDos::ExtendedBiosFunctionCall()
{
static const z80memaddr_t MM_ALL_SEG 	= 0xFF01;	// 16Kのセグメントを割り付ける
static const z80memaddr_t MM_FRE_SEG 	= 0xFF02;	// 16Kのセグメントを開放する
static const z80memaddr_t MM_RD_SEG		= 0xFF03;	// セグメント番号Ａの番地ＨＬの内容を読む
static const z80memaddr_t MM_WR_SEG		= 0xFF04;	// セグメント番号Ａの番地ＨＬにＥの値を書く
static const z80memaddr_t MM_CAL_SEG	= 0xFF05;	// インターセグメントコール（インデックスレジスタ）
static const z80memaddr_t MM_CALLS		= 0xFF06;	// インターセグメントコール（インラインパラメーター）
static const z80memaddr_t MM_PUT_PH		= 0xFF07;	// Ｈレジスタの上位２ビットのページを切り換える
static const z80memaddr_t MM_GET_PH		= 0xFF08;	// Ｈレジスタの上位２ビットのページのセグメント番号を得る
static const z80memaddr_t MM_PUT_P0		= 0xFF09;	// ページ０のセグメントを切り換える
static const z80memaddr_t MM_GET_P0		= 0xFF0A;	// ページ０の現在のセグメント番号を得る
static const z80memaddr_t MM_PUT_P1		= 0xFF0B;	// ページ１のセグメントを切り換える
static const z80memaddr_t MM_GET_P1		= 0xFF0C;	// ページ１の現在のセグメント番号を得る
static const z80memaddr_t MM_PUT_P2		= 0xFF0D;	// ページ２のセグメントを切り換える
static const z80memaddr_t MM_GET_P2		= 0xFF0E;	// ページ２の現在のセグメント番号を得る
static const z80memaddr_t MM_PUT_P3		= 0xFF0F;	// 何もせずに戻る
static const z80memaddr_t MM_GET_P3		= 0xFF10;	// ページ３の現在のセグメント番号を得る
const static z80memaddr_t BIOS_EXTBIO 	= 0xFFCA;	// 拡張BIOS

	if( m_R.PC < MM_ALL_SEG )
		return;
	switch(m_R.PC)
	{
		case BIOS_EXTBIO:
		{
			if( m_R.GetDE() == 0x0402 ){
				// マッパサポートルーチンの先頭アドレスを得る
				m_R.A = 16;			// CRam256k プライマリマッパの総セグメント数、
				m_R.B = SLOTNO_3;	// プライマリマッパのスロット番号、
				m_R.C = 16-4;		// Cにプライマリマッパの未使用セグメント数、(4つは既にDOSが使用しているとする）
				m_R.SetHL(0xFF00);	// HLにジャンプテーブルの先頭アドレスを返す。（仮に0xFF00と定めた）
				// 
				//  +0H　ALL_SEG　　 16Kのセグメントを割り付ける
				m_pMemSys->Write(0xFF00+ 0, 0xC3);
				m_pMemSys->Write(0xFF00+ 1, 0x01);
				m_pMemSys->Write(0xFF00+ 2, 0xFF);
				//  +3H　FRE_SEG　　 16Kのセグメントを開放する
				m_pMemSys->Write(0xFF00+ 3, 0xC3);
				m_pMemSys->Write(0xFF00+ 4, 0x02);
				m_pMemSys->Write(0xFF00+ 5, 0xFF);
				//  +6H　RD_SEG　　　セグメント番号Ａの番地ＨＬの内容を読む
				m_pMemSys->Write(0xFF00+ 6, 0xC3);
				m_pMemSys->Write(0xFF00+ 7, 0x03);
				m_pMemSys->Write(0xFF00+ 8, 0xFF);
				//  +9H　WR_SEG　　　セグメント番号Ａの番地ＨＬにＥの値を書く
				m_pMemSys->Write(0xFF00+ 9, 0xC3);
				m_pMemSys->Write(0xFF00+10, 0x04);
				m_pMemSys->Write(0xFF00+11, 0xFF);
				//  +CH　CAL_SEG　　 インターセグメントコール（インデックスレジスタ）
				m_pMemSys->Write(0xFF00+12, 0xC3);
				m_pMemSys->Write(0xFF00+13, 0x05);
				m_pMemSys->Write(0xFF00+14, 0xFF);
				//  +FH　CALLS　　　 インターセグメントコール（インラインパラメーター）
				m_pMemSys->Write(0xFF00+15, 0xC3);
				m_pMemSys->Write(0xFF00+16, 0x06);
				m_pMemSys->Write(0xFF00+17, 0xFF);
				// +12H　PUT_PH　　　Ｈレジスタの上位２ビットのページを切り換える
				m_pMemSys->Write(0xFF00+18, 0xC3);
				m_pMemSys->Write(0xFF00+19, 0x07);
				m_pMemSys->Write(0xFF00+20, 0xFF);
				// +15H　GET_PH　　　Ｈレジスタの上位２ビットのページのセグメント番号を得る
				m_pMemSys->Write(0xFF00+21, 0xC3);
				m_pMemSys->Write(0xFF00+22, 0x08);
				m_pMemSys->Write(0xFF00+23, 0xFF);
				// +18H　PUT_P0　　　ページ０のセグメントを切り換える
				m_pMemSys->Write(0xFF00+24, 0xC3);
				m_pMemSys->Write(0xFF00+25, 0x09);
				m_pMemSys->Write(0xFF00+26, 0xFF);
				// +1BH　GET_P0　　　ページ０の現在のセグメント番号を得る
				m_pMemSys->Write(0xFF00+27, 0xC3);
				m_pMemSys->Write(0xFF00+28, 0x0A);
				m_pMemSys->Write(0xFF00+29, 0xFF);
				// +1EH　PUT_P1　　　ページ１のセグメントを切り換える
				m_pMemSys->Write(0xFF00+30, 0xC3);
				m_pMemSys->Write(0xFF00+31, 0x0B);
				m_pMemSys->Write(0xFF00+32, 0xFF);
				// +21H　GET_P1　　　ページ１の現在のセグメント番号を得る
				m_pMemSys->Write(0xFF00+33, 0xC3);
				m_pMemSys->Write(0xFF00+34, 0x0C);
				m_pMemSys->Write(0xFF00+35, 0xFF);
				// +24H　PUT_P2　　　ページ２のセグメントを切り換える
				m_pMemSys->Write(0xFF00+36, 0xC3);
				m_pMemSys->Write(0xFF00+37, 0x0D);
				m_pMemSys->Write(0xFF00+38, 0xFF);
				// +27H　GET_P2　　　ページ２の現在のセグメント番号を得る
				m_pMemSys->Write(0xFF00+39, 0xC3);
				m_pMemSys->Write(0xFF00+40, 0x0E);
				m_pMemSys->Write(0xFF00+41, 0xFF);
				// +2AH　PUT_P3　　　何もせずに戻る
				m_pMemSys->Write(0xFF00+42, 0xC3);
				m_pMemSys->Write(0xFF00+43, 0x0F);
				m_pMemSys->Write(0xFF00+44, 0xFF);
				// +2DH　GET_P3　　　ページ３の現在のセグメント番号を得る
				m_pMemSys->Write(0xFF00+45, 0xC3);
				m_pMemSys->Write(0xFF00+46, 0x10);
				m_pMemSys->Write(0xFF00+47, 0xFF);
			}
			else if (m_R.GetDE() == 0xf000) {
				// 何者だろう？
			}
			else{
				std::wcout
					<< _T("\n **HopStepZ >> Not supported Extended-BIOS call device No. = ")
					<< std::hex << (int)m_R.D << _T("\n");
				DEBUG_BREAK;
			}
			op_RET();
			break;
		}
		case MM_ALL_SEG:	// 16Kのセグメントを割り付ける
		{
			int pageNo;
			if( patchingMapper(&pageNo, m_R.A+1)) {
				m_R.A = static_cast<uint8_t>(pageNo);
				m_R.F.C = 0;
			}
			else{
				m_R.F.C = 1;
			}
			op_RET();
			break;
		}
		case MM_FRE_SEG:	// 16Kのセグメントを開放する
		{
			m_MemoryMapper[m_R.A] = 0;
			m_R.F.C = 0;
			op_RET();
			break;
		}
		case MM_RD_SEG:		// セグメント番号Ａの番地ＨＬの内容を読む
			DEBUG_BREAK;
			op_RET();
			break;
		case MM_WR_SEG:		// セグメント番号Ａの番地ＨＬにＥの値を書く
			DEBUG_BREAK;
			op_RET();
			break;
		case MM_CAL_SEG:	// インターセグメントコール（インデックスレジスタ）
			DEBUG_BREAK;
			op_RET();
			break;
		case MM_CALLS:		// インターセグメントコール（インラインパラメーター）
			DEBUG_BREAK;
			op_RET();
			break;
		case MM_PUT_PH:		// Ｈレジスタの上位２ビットのページを切り換える
			DEBUG_BREAK;
			op_RET();
			break;
		case MM_GET_PH:		// Ｈレジスタの上位２ビットのページのセグメント番号を得る
			DEBUG_BREAK;
			op_RET();
			break;
		case MM_PUT_P0:		// ページ０のセグメントを切り換える
			m_pIoSys->Out(0xFC+0, m_R.A);
			op_RET();
			break;
		case MM_GET_P0:		// ページ０の現在のセグメント番号を得る
			m_R.A = m_pIoSys->In(0xFC+0);
			op_RET();
			break;
		case MM_PUT_P1:		// ページ１のセグメントを切り換える
			m_pIoSys->Out(0xFC+1, m_R.A);
			op_RET();
			break;
		case MM_GET_P1:		// ページ１の現在のセグメント番号を得る
			m_R.A = m_pIoSys->In(0xFC+1);
			op_RET();
			break;
		case MM_PUT_P2:		// ページ２のセグメントを切り換える
			m_pIoSys->Out(0xFC+2, m_R.A);
			op_RET();
			break;
		case MM_GET_P2:		// ページ２の現在のセグメント番号を得る
			m_R.A = m_pIoSys->In(0xFC+2);
			op_RET();
			break;
		case MM_PUT_P3:		// 何もせずに戻る
			m_pIoSys->Out(0xFC+3, m_R.A);
			op_RET();
			break;
		case MM_GET_P3:		// ページ３の現在のセグメント番号を得る
			m_R.A = m_pIoSys->In(0xFC+3);
			op_RET();
			break;
	}
	return;
}

/** MAIN-ROM
*/
void CZ80MsxDos::MainRomFunctionCall()
{
	if( m_R.PC != 0x4601 )
		return;
	SLOTNO base, ext;
	m_pMemSys->GetSlot(&base, &ext, MEMPAGE_1);
	// MAIN-ROM.NEWSTT
	if( base == SLOTNO_0 && ext == SLOTNO_0 ) {
		// 中身の実装は無し、ただ終了させるのみ
		m_R.PC = 0;
		m_pMemSys->ChangeSlot(MEMPAGE_0, SLOTNO_3, SLOTNO_0);
		m_pMemSys->ChangeSlot(MEMPAGE_1, SLOTNO_3, SLOTNO_0);
	}
	return;
}

/** PCの値を見張っていて、特定の位置にPCが来たら対応するファンクションを実行する
*/
void CZ80MsxDos::MsxDosFunctionCall()
{
	if( m_R.PC != DOS_SYSTEMCALL )
		return;
	auto no = static_cast<dosfuncno_t>(m_R.C);
	switch(no)
	{
		case DOS_CONOUT:
		{
			::wprintf(_T("%c"), m_R.E);
			break;
		}
		case DOS_STROUT:
		{
			for(z80memaddr_t ad = m_R.GetDE(); true; ++ad){
				uint8_t ch = m_pMemSys->Read(ad);
				if( ch == '$' )
					break;
				::wprintf(_T("%c"), ch);
			}
			break;
		}
		case DOS_TERM:
		{
			std::wcout << _T("\n **HopStepZ >> terminate with errocode=") << (int)m_R.B << _T("\n");
			m_R.PC =0;	// 終了
			break;
		}
		case DOS_GENV:
		{
			const z80memaddr_t src = m_R.GetHL();	// 環境変数名が格納された領域へのポインタ(\0ターミネータ)
			z80memaddr_t dest = m_R.GetDE();		// 環境変数の内容の格納先
			//int areaSize = m_R.B;					// 格納先のサイズ
		
			std::string name;
			m_pMemSys->ReadString(&name, src);
			if( name == "PARAMETERS" ) {
				static const uint8_t cmd[] = { '/','K','0',' ','/','Z' };
				for( int t = 0; t < static_cast<int>(sizeof(cmd)); ++t )
					m_pMemSys->Write(dest+t, cmd[t]);
				m_R.A = 0;
			}else if( name == "SHELL" ) {
				static const uint8_t cmd[] = { 'A',':','\\','C','O','M','M','A','N','D','2','.','C','O','M' };
				for( int t = 0; t < static_cast<int>(sizeof(cmd)); ++t )
					m_pMemSys->Write(dest+t, cmd[t]);
				m_R.A = 0;
			}else {
				m_pMemSys->Write(dest+0, '\0');
				m_R.A = 0;
			}
			m_R.F.Z = 1;
			break;
		}
		case DOS_SENV:
		{
			m_R.A = 0;
			// do nothing
			break;
		}
		case DOS_DOSVER:
		{
			m_R.SetBC(0x0230);	// Kernel バージョン 2.30
			m_R.SetDE(0x0220);	// MSXDOS.SYS バージョン 2.20
			m_R.A = 0;
			break;
		}
		default:
		{
			std::wcout << _T("\n **HopStepZ >> Not supported function no. ") << (int)no << _T("\n");
			DEBUG_BREAK;
			//　非対応ファンクションは、A=B=0でリターンする。
			m_R.A = 0;
			m_R.B = 0;
			break;
		}
	}
	op_RET();
	return;
}

bool CZ80MsxDos::patchingMapper(int *pPsegNo, int usersys)
{
	for( int t = 0; static_cast<int>(m_MemoryMapper.size()); ++t){
		if( m_MemoryMapper[t] == 0 ){
			m_MemoryMapper[t] = usersys;
			*pPsegNo = t;
			return true;
		}
	}
	return false;
}

void CZ80MsxDos::setup()
{
	// TODO: 即値になっている、将来直せ。
	// MemoryMapper[]の要素番号そのものがセグメント番号を示し、
	// 中の値は、割り当て済みかどうかを示している。0=未割当、1=ユーザー、2=システム
	for( int t = 0; t < 16; ++t)
		m_MemoryMapper.push_back(0);
	m_MemoryMapper[0] = 2;
	m_MemoryMapper[1] = 2;
	m_MemoryMapper[2] = 2;
	m_MemoryMapper[3] = 2;

	m_Tim.ResetBegin();

	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x00, &CZ80MsxDos::op_NOP));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x01, &CZ80MsxDos::op_LD_BC_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x02, &CZ80MsxDos::op_LD_memBC_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x03, &CZ80MsxDos::op_INC_BC));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x04, &CZ80MsxDos::op_INC_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x05, &CZ80MsxDos::op_DEC_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x06, &CZ80MsxDos::op_LD_B_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x07, &CZ80MsxDos::op_RLCA));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x08, &CZ80MsxDos::op_EX_AF_AF));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x09, &CZ80MsxDos::op_ADD_HL_BC));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x0A, &CZ80MsxDos::op_LD_A_memBC));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x0B, &CZ80MsxDos::op_DEC_BC));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x0C, &CZ80MsxDos::op_INC_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x0D, &CZ80MsxDos::op_DEC_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x0E, &CZ80MsxDos::op_LD_C_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x0F, &CZ80MsxDos::op_RRCA));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x10, &CZ80MsxDos::op_DJNZ_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x11, &CZ80MsxDos::op_LD_DE_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x12, &CZ80MsxDos::op_LD_memDE_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x13, &CZ80MsxDos::op_INC_DE));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x14, &CZ80MsxDos::op_INC_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x15, &CZ80MsxDos::op_DEC_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x16, &CZ80MsxDos::op_LD_D_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x17, &CZ80MsxDos::op_RLA));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x18, &CZ80MsxDos::op_JR_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x19, &CZ80MsxDos::op_ADD_HL_DE));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x1A, &CZ80MsxDos::op_LD_A_memDE));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x1B, &CZ80MsxDos::op_DEC_DE));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x1C, &CZ80MsxDos::op_INC_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x1D, &CZ80MsxDos::op_DEC_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x1E, &CZ80MsxDos::op_LD_E_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x1F, &CZ80MsxDos::op_RRA));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x20, &CZ80MsxDos::op_JR_nz_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x21, &CZ80MsxDos::op_LD_HL_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x22, &CZ80MsxDos::op_LD_memAD_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x23, &CZ80MsxDos::op_INC_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x24, &CZ80MsxDos::op_INC_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x25, &CZ80MsxDos::op_DEC_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x26, &CZ80MsxDos::op_LD_H_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x27, &CZ80MsxDos::op_DAA));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x28, &CZ80MsxDos::op_JR_z_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x29, &CZ80MsxDos::op_ADD_HL_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x2A, &CZ80MsxDos::op_LD_HL_memAD));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x2B, &CZ80MsxDos::op_DEC_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x2C, &CZ80MsxDos::op_INC_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x2D, &CZ80MsxDos::op_DEC_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x2E, &CZ80MsxDos::op_LD_L_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x2F, &CZ80MsxDos::op_CPL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x30, &CZ80MsxDos::op_JR_nc_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x31, &CZ80MsxDos::op_LD_SP_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x32, &CZ80MsxDos::op_LD_memAD_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x33, &CZ80MsxDos::op_INC_SP));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x34, &CZ80MsxDos::op_INC_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x35, &CZ80MsxDos::op_DEC_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x36, &CZ80MsxDos::op_LD_memHL_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x37, &CZ80MsxDos::op_SCF));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x38, &CZ80MsxDos::op_JR_C_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x39, &CZ80MsxDos::op_ADD_HL_SP));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x3A, &CZ80MsxDos::op_LD_A_memAD));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x3B, &CZ80MsxDos::op_DEC_SP));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x3C, &CZ80MsxDos::op_INC_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x3D, &CZ80MsxDos::op_DEC_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x3E, &CZ80MsxDos::op_LD_A_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x3F, &CZ80MsxDos::op_CCF));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x40, &CZ80MsxDos::op_LD_B_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x41, &CZ80MsxDos::op_LD_B_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x42, &CZ80MsxDos::op_LD_B_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x43, &CZ80MsxDos::op_LD_B_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x44, &CZ80MsxDos::op_LD_B_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x45, &CZ80MsxDos::op_LD_B_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x46, &CZ80MsxDos::op_LD_B_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x47, &CZ80MsxDos::op_LD_B_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x48, &CZ80MsxDos::op_LD_C_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x49, &CZ80MsxDos::op_LD_C_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x4A, &CZ80MsxDos::op_LD_C_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x4B, &CZ80MsxDos::op_LD_C_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x4C, &CZ80MsxDos::op_LD_C_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x4D, &CZ80MsxDos::op_LD_C_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x4E, &CZ80MsxDos::op_LD_C_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x4F, &CZ80MsxDos::op_LD_C_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x50, &CZ80MsxDos::op_LD_D_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x51, &CZ80MsxDos::op_LD_D_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x52, &CZ80MsxDos::op_LD_D_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x53, &CZ80MsxDos::op_LD_D_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x54, &CZ80MsxDos::op_LD_D_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x55, &CZ80MsxDos::op_LD_D_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x56, &CZ80MsxDos::op_LD_D_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x57, &CZ80MsxDos::op_LD_D_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x58, &CZ80MsxDos::op_LD_E_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x59, &CZ80MsxDos::op_LD_E_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x5A, &CZ80MsxDos::op_LD_E_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x5B, &CZ80MsxDos::op_LD_E_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x5C, &CZ80MsxDos::op_LD_E_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x5D, &CZ80MsxDos::op_LD_E_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x5E, &CZ80MsxDos::op_LD_E_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x5F, &CZ80MsxDos::op_LD_E_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x60, &CZ80MsxDos::op_LD_H_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x61, &CZ80MsxDos::op_LD_H_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x62, &CZ80MsxDos::op_LD_H_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x63, &CZ80MsxDos::op_LD_H_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x64, &CZ80MsxDos::op_LD_H_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x65, &CZ80MsxDos::op_LD_H_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x66, &CZ80MsxDos::op_LD_H_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x67, &CZ80MsxDos::op_LD_H_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x68, &CZ80MsxDos::op_LD_L_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x69, &CZ80MsxDos::op_LD_L_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x6A, &CZ80MsxDos::op_LD_L_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x6B, &CZ80MsxDos::op_LD_L_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x6C, &CZ80MsxDos::op_LD_L_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x6D, &CZ80MsxDos::op_LD_L_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x6E, &CZ80MsxDos::op_LD_L_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x6F, &CZ80MsxDos::op_LD_L_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x70, &CZ80MsxDos::op_LD_memHL_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x71, &CZ80MsxDos::op_LD_memHL_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x72, &CZ80MsxDos::op_LD_memHL_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x73, &CZ80MsxDos::op_LD_memHL_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x74, &CZ80MsxDos::op_LD_memHL_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x75, &CZ80MsxDos::op_LD_memHL_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x76, &CZ80MsxDos::op_HALT));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x77, &CZ80MsxDos::op_LD_memHL_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x78, &CZ80MsxDos::op_LD_A_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x79, &CZ80MsxDos::op_LD_A_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x7A, &CZ80MsxDos::op_LD_A_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x7B, &CZ80MsxDos::op_LD_A_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x7C, &CZ80MsxDos::op_LD_A_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x7D, &CZ80MsxDos::op_LD_A_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x7E, &CZ80MsxDos::op_LD_A_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x7F, &CZ80MsxDos::op_LD_A_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x80, &CZ80MsxDos::op_ADD_A_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x81, &CZ80MsxDos::op_ADD_A_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x82, &CZ80MsxDos::op_ADD_A_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x83, &CZ80MsxDos::op_ADD_A_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x84, &CZ80MsxDos::op_ADD_A_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x85, &CZ80MsxDos::op_ADD_A_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x86, &CZ80MsxDos::op_ADD_A_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x87, &CZ80MsxDos::op_ADD_A_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x88, &CZ80MsxDos::op_ADC_A_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x89, &CZ80MsxDos::op_ADC_A_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x8A, &CZ80MsxDos::op_ADC_A_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x8B, &CZ80MsxDos::op_ADC_A_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x8C, &CZ80MsxDos::op_ADC_A_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x8D, &CZ80MsxDos::op_ADC_A_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x8E, &CZ80MsxDos::op_ADC_A_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x8F, &CZ80MsxDos::op_ADC_A_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x90, &CZ80MsxDos::op_SUB_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x91, &CZ80MsxDos::op_SUB_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x92, &CZ80MsxDos::op_SUB_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x93, &CZ80MsxDos::op_SUB_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x94, &CZ80MsxDos::op_SUB_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x95, &CZ80MsxDos::op_SUB_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x96, &CZ80MsxDos::op_SUB_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x97, &CZ80MsxDos::op_SUB_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x98, &CZ80MsxDos::op_SBC_A_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x99, &CZ80MsxDos::op_SBC_A_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x9A, &CZ80MsxDos::op_SBC_A_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x9B, &CZ80MsxDos::op_SBC_A_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x9C, &CZ80MsxDos::op_SBC_A_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x9D, &CZ80MsxDos::op_SBC_A_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x9E, &CZ80MsxDos::op_SBC_A_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0x9F, &CZ80MsxDos::op_SBC_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA0, &CZ80MsxDos::op_AND_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA1, &CZ80MsxDos::op_AND_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA2, &CZ80MsxDos::op_AND_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA3, &CZ80MsxDos::op_AND_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA4, &CZ80MsxDos::op_AND_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA5, &CZ80MsxDos::op_AND_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA6, &CZ80MsxDos::op_AND_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA7, &CZ80MsxDos::op_AND_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA8, &CZ80MsxDos::op_XOR_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xA9, &CZ80MsxDos::op_XOR_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xAA, &CZ80MsxDos::op_XOR_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xAB, &CZ80MsxDos::op_XOR_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xAC, &CZ80MsxDos::op_XOR_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xAD, &CZ80MsxDos::op_XOR_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xAE, &CZ80MsxDos::op_XOR_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xAF, &CZ80MsxDos::op_XOR_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB0, &CZ80MsxDos::op_OR_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB1, &CZ80MsxDos::op_OR_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB2, &CZ80MsxDos::op_OR_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB3, &CZ80MsxDos::op_OR_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB4, &CZ80MsxDos::op_OR_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB5, &CZ80MsxDos::op_OR_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB6, &CZ80MsxDos::op_OR_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB7, &CZ80MsxDos::op_OR_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB8, &CZ80MsxDos::op_CP_B));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xB9, &CZ80MsxDos::op_CP_C));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xBA, &CZ80MsxDos::op_CP_D));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xBB, &CZ80MsxDos::op_CP_E));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xBC, &CZ80MsxDos::op_CP_H));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xBD, &CZ80MsxDos::op_CP_L));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xBE, &CZ80MsxDos::op_CP_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xBF, &CZ80MsxDos::op_CP_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC0, &CZ80MsxDos::op_RET_nz));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC1, &CZ80MsxDos::op_POP_BC));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC2, &CZ80MsxDos::op_JP_nz_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC3, &CZ80MsxDos::op_JP_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC4, &CZ80MsxDos::op_CALL_nz_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC5, &CZ80MsxDos::op_PUSH_BC));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC6, &CZ80MsxDos::op_ADD_A_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC7, &CZ80MsxDos::op_RST_0h));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC8, &CZ80MsxDos::op_RET_z));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xC9, &CZ80MsxDos::op_RET));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xCA, &CZ80MsxDos::op_JP_z_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xCB, &CZ80MsxDos::op_EXTENDED_1));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xCC, &CZ80MsxDos::op_CALL_z_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xCD, &CZ80MsxDos::op_CALL_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xCE, &CZ80MsxDos::op_ADC_A_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xCF, &CZ80MsxDos::op_RST_8h));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD0, &CZ80MsxDos::op_RET_nc));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD1, &CZ80MsxDos::op_POP_DE));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD2, &CZ80MsxDos::op_JP_nc_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD3, &CZ80MsxDos::op_OUT_memv_A));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD4, &CZ80MsxDos::op_CALL_nc_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD5, &CZ80MsxDos::op_PUSH_DE));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD6, &CZ80MsxDos::op_SUB_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD7, &CZ80MsxDos::op_RST_10h));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD8, &CZ80MsxDos::op_RET_c));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xD9, &CZ80MsxDos::op_EXX));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xDA, &CZ80MsxDos::op_JP_c_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xDB, &CZ80MsxDos::op_IN_A_memv));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xDC, &CZ80MsxDos::op_CALL_c_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xDD, &CZ80MsxDos::op_EXTENDED_2IX));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xDE, &CZ80MsxDos::op_SBC_A_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xDF, &CZ80MsxDos::op_RST_18h));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE0, &CZ80MsxDos::op_RET_po));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE1, &CZ80MsxDos::op_POP_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE2, &CZ80MsxDos::op_JP_po_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE3, &CZ80MsxDos::op_EX_memSP_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE4, &CZ80MsxDos::op_CALL_po_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE5, &CZ80MsxDos::op_PUSH_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE6, &CZ80MsxDos::op_AND_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE7, &CZ80MsxDos::op_RST_20h));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE8, &CZ80MsxDos::op_RET_pe));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xE9, &CZ80MsxDos::op_JP_memHL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xEA, &CZ80MsxDos::op_JP_pe_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xEB, &CZ80MsxDos::op_EX_DE_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xEC, &CZ80MsxDos::op_CALL_pe_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xED, &CZ80MsxDos::op_EXTENDED_3));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xEE, &CZ80MsxDos::op_XOR_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xEF, &CZ80MsxDos::op_RST_28h));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF0, &CZ80MsxDos::op_RET_p));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF1, &CZ80MsxDos::op_POP_AF));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF2, &CZ80MsxDos::op_JP_p_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF3, &CZ80MsxDos::op_DI));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF4, &CZ80MsxDos::op_CALL_p_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF5, &CZ80MsxDos::op_PUSH_AF));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF6, &CZ80MsxDos::op_OR_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF7, &CZ80MsxDos::op_RST_30h));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF8, &CZ80MsxDos::op_RET_m));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xF9, &CZ80MsxDos::op_LD_SP_HL));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xFA, &CZ80MsxDos::op_JP_m_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xFB, &CZ80MsxDos::op_EI));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xFC, &CZ80MsxDos::op_CALL_m_ad));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xFD, &CZ80MsxDos::op_EXTENDED_4IY));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xFE, &CZ80MsxDos::op_CP_v));
	OpCode_Single.push_back(Z80OPECODE_FUNC( 0xFF, &CZ80MsxDos::op_RST_38h));

	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x00, &CZ80MsxDos::op_RLC_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x01, &CZ80MsxDos::op_RLC_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x02, &CZ80MsxDos::op_RLC_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x03, &CZ80MsxDos::op_RLC_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x04, &CZ80MsxDos::op_RLC_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x05, &CZ80MsxDos::op_RLC_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x06, &CZ80MsxDos::op_RLC_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x07, &CZ80MsxDos::op_RLC_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x08, &CZ80MsxDos::op_RRC_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x09, &CZ80MsxDos::op_RRC_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x0A, &CZ80MsxDos::op_RRC_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x0B, &CZ80MsxDos::op_RRC_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x0C, &CZ80MsxDos::op_RRC_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x0D, &CZ80MsxDos::op_RRC_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x0E, &CZ80MsxDos::op_RRC_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x0F, &CZ80MsxDos::op_RRC_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x10, &CZ80MsxDos::op_RL_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x11, &CZ80MsxDos::op_RL_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x12, &CZ80MsxDos::op_RL_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x13, &CZ80MsxDos::op_RL_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x14, &CZ80MsxDos::op_RL_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x15, &CZ80MsxDos::op_RL_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x16, &CZ80MsxDos::op_RL_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x17, &CZ80MsxDos::op_RL_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x18, &CZ80MsxDos::op_RR_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x19, &CZ80MsxDos::op_RR_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x1A, &CZ80MsxDos::op_RR_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x1B, &CZ80MsxDos::op_RR_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x1C, &CZ80MsxDos::op_RR_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x1D, &CZ80MsxDos::op_RR_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x1E, &CZ80MsxDos::op_RR_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x1F, &CZ80MsxDos::op_RR_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x20, &CZ80MsxDos::op_SLA_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x21, &CZ80MsxDos::op_SLA_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x22, &CZ80MsxDos::op_SLA_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x23, &CZ80MsxDos::op_SLA_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x24, &CZ80MsxDos::op_SLA_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x25, &CZ80MsxDos::op_SLA_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x26, &CZ80MsxDos::op_SLA_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x27, &CZ80MsxDos::op_SLA_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x28, &CZ80MsxDos::op_SRA_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x29, &CZ80MsxDos::op_SRA_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x2A, &CZ80MsxDos::op_SRA_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x2B, &CZ80MsxDos::op_SRA_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x2C, &CZ80MsxDos::op_SRA_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x2D, &CZ80MsxDos::op_SRA_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x2E, &CZ80MsxDos::op_SRA_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x2F, &CZ80MsxDos::op_SRA_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x30, &CZ80MsxDos::op_SLL_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x31, &CZ80MsxDos::op_SLL_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x32, &CZ80MsxDos::op_SLL_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x33, &CZ80MsxDos::op_SLL_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x34, &CZ80MsxDos::op_SLL_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x35, &CZ80MsxDos::op_SLL_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x36, &CZ80MsxDos::op_SLL_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x37, &CZ80MsxDos::op_SLL_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x38, &CZ80MsxDos::op_SRL_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x39, &CZ80MsxDos::op_SRL_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x3A, &CZ80MsxDos::op_SRL_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x3B, &CZ80MsxDos::op_SRL_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x3C, &CZ80MsxDos::op_SRL_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x3D, &CZ80MsxDos::op_SRL_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x3E, &CZ80MsxDos::op_SRL_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x3F, &CZ80MsxDos::op_SRL_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x40, &CZ80MsxDos::op_BIT_0_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x41, &CZ80MsxDos::op_BIT_0_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x42, &CZ80MsxDos::op_BIT_0_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x43, &CZ80MsxDos::op_BIT_0_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x44, &CZ80MsxDos::op_BIT_0_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x45, &CZ80MsxDos::op_BIT_0_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x46, &CZ80MsxDos::op_BIT_0_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x47, &CZ80MsxDos::op_BIT_0_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x48, &CZ80MsxDos::op_BIT_1_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x49, &CZ80MsxDos::op_BIT_1_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x4A, &CZ80MsxDos::op_BIT_1_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x4B, &CZ80MsxDos::op_BIT_1_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x4C, &CZ80MsxDos::op_BIT_1_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x4D, &CZ80MsxDos::op_BIT_1_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x4E, &CZ80MsxDos::op_BIT_1_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x4F, &CZ80MsxDos::op_BIT_1_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x50, &CZ80MsxDos::op_BIT_2_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x51, &CZ80MsxDos::op_BIT_2_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x52, &CZ80MsxDos::op_BIT_2_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x53, &CZ80MsxDos::op_BIT_2_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x54, &CZ80MsxDos::op_BIT_2_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x55, &CZ80MsxDos::op_BIT_2_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x56, &CZ80MsxDos::op_BIT_2_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x57, &CZ80MsxDos::op_BIT_2_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x58, &CZ80MsxDos::op_BIT_3_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x59, &CZ80MsxDos::op_BIT_3_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x5A, &CZ80MsxDos::op_BIT_3_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x5B, &CZ80MsxDos::op_BIT_3_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x5C, &CZ80MsxDos::op_BIT_3_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x5D, &CZ80MsxDos::op_BIT_3_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x5E, &CZ80MsxDos::op_BIT_3_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x5F, &CZ80MsxDos::op_BIT_3_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x60, &CZ80MsxDos::op_BIT_4_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x61, &CZ80MsxDos::op_BIT_4_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x62, &CZ80MsxDos::op_BIT_4_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x63, &CZ80MsxDos::op_BIT_4_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x64, &CZ80MsxDos::op_BIT_4_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x65, &CZ80MsxDos::op_BIT_4_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x66, &CZ80MsxDos::op_BIT_4_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x67, &CZ80MsxDos::op_BIT_4_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x68, &CZ80MsxDos::op_BIT_5_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x69, &CZ80MsxDos::op_BIT_5_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x6A, &CZ80MsxDos::op_BIT_5_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x6B, &CZ80MsxDos::op_BIT_5_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x6C, &CZ80MsxDos::op_BIT_5_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x6D, &CZ80MsxDos::op_BIT_5_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x6E, &CZ80MsxDos::op_BIT_5_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x6F, &CZ80MsxDos::op_BIT_5_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x70, &CZ80MsxDos::op_BIT_6_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x71, &CZ80MsxDos::op_BIT_6_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x72, &CZ80MsxDos::op_BIT_6_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x73, &CZ80MsxDos::op_BIT_6_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x74, &CZ80MsxDos::op_BIT_6_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x75, &CZ80MsxDos::op_BIT_6_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x76, &CZ80MsxDos::op_BIT_6_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x77, &CZ80MsxDos::op_BIT_6_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x78, &CZ80MsxDos::op_BIT_7_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x79, &CZ80MsxDos::op_BIT_7_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x7A, &CZ80MsxDos::op_BIT_7_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x7B, &CZ80MsxDos::op_BIT_7_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x7C, &CZ80MsxDos::op_BIT_7_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x7D, &CZ80MsxDos::op_BIT_7_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x7E, &CZ80MsxDos::op_BIT_7_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x7F, &CZ80MsxDos::op_BIT_7_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x80, &CZ80MsxDos::op_RES_0_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x81, &CZ80MsxDos::op_RES_0_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x82, &CZ80MsxDos::op_RES_0_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x83, &CZ80MsxDos::op_RES_0_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x84, &CZ80MsxDos::op_RES_0_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x85, &CZ80MsxDos::op_RES_0_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x86, &CZ80MsxDos::op_RES_0_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x87, &CZ80MsxDos::op_RES_0_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x88, &CZ80MsxDos::op_RES_1_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x89, &CZ80MsxDos::op_RES_1_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x8A, &CZ80MsxDos::op_RES_1_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x8B, &CZ80MsxDos::op_RES_1_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x8C, &CZ80MsxDos::op_RES_1_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x8D, &CZ80MsxDos::op_RES_1_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x8E, &CZ80MsxDos::op_RES_1_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x8F, &CZ80MsxDos::op_RES_1_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x90, &CZ80MsxDos::op_RES_2_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x91, &CZ80MsxDos::op_RES_2_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x92, &CZ80MsxDos::op_RES_2_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x93, &CZ80MsxDos::op_RES_2_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x94, &CZ80MsxDos::op_RES_2_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x95, &CZ80MsxDos::op_RES_2_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x96, &CZ80MsxDos::op_RES_2_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x97, &CZ80MsxDos::op_RES_2_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x98, &CZ80MsxDos::op_RES_3_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x99, &CZ80MsxDos::op_RES_3_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x9A, &CZ80MsxDos::op_RES_3_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x9B, &CZ80MsxDos::op_RES_3_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x9C, &CZ80MsxDos::op_RES_3_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x9D, &CZ80MsxDos::op_RES_3_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x9E, &CZ80MsxDos::op_RES_3_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0x9F, &CZ80MsxDos::op_RES_3_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA0, &CZ80MsxDos::op_RES_4_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA1, &CZ80MsxDos::op_RES_4_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA2, &CZ80MsxDos::op_RES_4_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA3, &CZ80MsxDos::op_RES_4_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA4, &CZ80MsxDos::op_RES_4_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA5, &CZ80MsxDos::op_RES_4_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA6, &CZ80MsxDos::op_RES_4_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA7, &CZ80MsxDos::op_RES_4_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA8, &CZ80MsxDos::op_RES_5_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xA9, &CZ80MsxDos::op_RES_5_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xAA, &CZ80MsxDos::op_RES_5_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xAB, &CZ80MsxDos::op_RES_5_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xAC, &CZ80MsxDos::op_RES_5_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xAD, &CZ80MsxDos::op_RES_5_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xAE, &CZ80MsxDos::op_RES_5_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xAF, &CZ80MsxDos::op_RES_5_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB0, &CZ80MsxDos::op_RES_6_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB1, &CZ80MsxDos::op_RES_6_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB2, &CZ80MsxDos::op_RES_6_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB3, &CZ80MsxDos::op_RES_6_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB4, &CZ80MsxDos::op_RES_6_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB5, &CZ80MsxDos::op_RES_6_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB6, &CZ80MsxDos::op_RES_6_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB7, &CZ80MsxDos::op_RES_6_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB8, &CZ80MsxDos::op_RES_7_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xB9, &CZ80MsxDos::op_RES_7_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xBA, &CZ80MsxDos::op_RES_7_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xBB, &CZ80MsxDos::op_RES_7_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xBC, &CZ80MsxDos::op_RES_7_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xBD, &CZ80MsxDos::op_RES_7_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xBE, &CZ80MsxDos::op_RES_7_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xBF, &CZ80MsxDos::op_RES_7_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC0, &CZ80MsxDos::op_SET_0_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC1, &CZ80MsxDos::op_SET_0_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC2, &CZ80MsxDos::op_SET_0_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC3, &CZ80MsxDos::op_SET_0_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC4, &CZ80MsxDos::op_SET_0_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC5, &CZ80MsxDos::op_SET_0_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC6, &CZ80MsxDos::op_SET_0_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC7, &CZ80MsxDos::op_SET_0_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC8, &CZ80MsxDos::op_SET_1_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xC9, &CZ80MsxDos::op_SET_1_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xCA, &CZ80MsxDos::op_SET_1_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xCB, &CZ80MsxDos::op_SET_1_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xCC, &CZ80MsxDos::op_SET_1_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xCD, &CZ80MsxDos::op_SET_1_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xCE, &CZ80MsxDos::op_SET_1_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xCF, &CZ80MsxDos::op_SET_1_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD0, &CZ80MsxDos::op_SET_2_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD1, &CZ80MsxDos::op_SET_2_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD2, &CZ80MsxDos::op_SET_2_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD3, &CZ80MsxDos::op_SET_2_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD4, &CZ80MsxDos::op_SET_2_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD5, &CZ80MsxDos::op_SET_2_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD6, &CZ80MsxDos::op_SET_2_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD7, &CZ80MsxDos::op_SET_2_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD8, &CZ80MsxDos::op_SET_3_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xD9, &CZ80MsxDos::op_SET_3_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xDA, &CZ80MsxDos::op_SET_3_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xDB, &CZ80MsxDos::op_SET_3_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xDC, &CZ80MsxDos::op_SET_3_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xDD, &CZ80MsxDos::op_SET_3_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xDE, &CZ80MsxDos::op_SET_3_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xDF, &CZ80MsxDos::op_SET_3_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE0, &CZ80MsxDos::op_SET_4_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE1, &CZ80MsxDos::op_SET_4_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE2, &CZ80MsxDos::op_SET_4_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE3, &CZ80MsxDos::op_SET_4_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE4, &CZ80MsxDos::op_SET_4_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE5, &CZ80MsxDos::op_SET_4_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE6, &CZ80MsxDos::op_SET_4_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE7, &CZ80MsxDos::op_SET_4_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE8, &CZ80MsxDos::op_SET_5_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xE9, &CZ80MsxDos::op_SET_5_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xEA, &CZ80MsxDos::op_SET_5_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xEB, &CZ80MsxDos::op_SET_5_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xEC, &CZ80MsxDos::op_SET_5_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xED, &CZ80MsxDos::op_SET_5_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xEE, &CZ80MsxDos::op_SET_5_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xEF, &CZ80MsxDos::op_SET_5_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF0, &CZ80MsxDos::op_SET_6_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF1, &CZ80MsxDos::op_SET_6_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF2, &CZ80MsxDos::op_SET_6_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF3, &CZ80MsxDos::op_SET_6_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF4, &CZ80MsxDos::op_SET_6_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF5, &CZ80MsxDos::op_SET_6_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF6, &CZ80MsxDos::op_SET_6_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF7, &CZ80MsxDos::op_SET_6_A));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF8, &CZ80MsxDos::op_SET_7_B));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xF9, &CZ80MsxDos::op_SET_7_C));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xFA, &CZ80MsxDos::op_SET_7_D));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xFB, &CZ80MsxDos::op_SET_7_E));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xFC, &CZ80MsxDos::op_SET_7_H));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xFD, &CZ80MsxDos::op_SET_7_L));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xFE, &CZ80MsxDos::op_SET_7_memHL));
	OpCode_Extended1.push_back(Z80OPECODE_FUNC(0xFF, &CZ80MsxDos::op_SET_7_A));

	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x00, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x01, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x02, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x03,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x04,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x05,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x06, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x07, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x08, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x09, &CZ80MsxDos::op_ADD_IX_BC		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x0A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x0B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x0C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x0D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x0E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x0F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x10, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x11, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x12, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x13, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x14, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x15, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x16, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x17, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x18, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x19, &CZ80MsxDos::op_ADD_IX_DE		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x1A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x1B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x1C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x1D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x1E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x1F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x20, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x21, &CZ80MsxDos::op_LD_IX_ad			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x22, &CZ80MsxDos::op_LD_memAD_IX		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x23, &CZ80MsxDos::op_INC_IX			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x24, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x25, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x26, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x27, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x28, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x29, &CZ80MsxDos::op_ADD_IX_IX		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x2A, &CZ80MsxDos::op_LD_IX_memAD		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x2B, &CZ80MsxDos::op_DEC_IX			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x2C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x2D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x2E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x2F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x30, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x31, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x32, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x33, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x34, &CZ80MsxDos::op_INC_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x35, &CZ80MsxDos::op_DEC_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x36, &CZ80MsxDos::op_LD_memIXpV_v		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x37, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x38, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x39, &CZ80MsxDos::op_ADD_IX_SP		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x3A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x3B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x3C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x3D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x3E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x3F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x40, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x41, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x42, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x43, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x44, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x45, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x46, &CZ80MsxDos::op_LD_B_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x47, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x48, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x49, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x4A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x4B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x4C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x4D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x4E, &CZ80MsxDos::op_LD_C_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x4F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x50, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x51, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x52, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x53, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x54, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x55, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x56, &CZ80MsxDos::op_LD_D_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x57, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x58, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x59, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x5A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x5B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x5C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x5D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x5E, &CZ80MsxDos::op_LD_E_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x5F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x60, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x61, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x62, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x63, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x64, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x65, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x66, &CZ80MsxDos::op_LD_H_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x67, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x68, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x69, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x6A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x6B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x6C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x6D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x6E, &CZ80MsxDos::op_LD_L_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x6F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x70, &CZ80MsxDos::op_LD_memIXpV_B		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x71, &CZ80MsxDos::op_LD_memIXpV_C		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x72, &CZ80MsxDos::op_LD_memIXpV_D		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x73, &CZ80MsxDos::op_LD_memIXpV_E		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x74, &CZ80MsxDos::op_LD_memIXpV_H		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x75, &CZ80MsxDos::op_LD_memIXpV_L		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x76, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x77, &CZ80MsxDos::op_LD_memIXpV_A		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x78, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x79, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x7A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x7B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x7C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x7D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x7E, &CZ80MsxDos::op_LD_A_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x7F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x80, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x81, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x82, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x83, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x84, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x85, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x86, &CZ80MsxDos::op_ADD_A_memIXpV	));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x87, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x88, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x89, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x8A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x8B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x8C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x8D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x8E, &CZ80MsxDos::op_ADC_A_memIXpV	));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x8F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x90, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x91, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x92, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x93, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x94, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x95, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x96, &CZ80MsxDos::op_SUB_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x97, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x98, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x99, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x9A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x9B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x9C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x9D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x9E, &CZ80MsxDos::op_SBC_A_memIXpV	));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0x9F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA6, &CZ80MsxDos::op_AND_memIXpV			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xA9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xAA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xAB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xAC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xAD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xAE, &CZ80MsxDos::op_XOR_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xAF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB6, &CZ80MsxDos::op_OR_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xB9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xBA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xBB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xBC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xBD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xBE, &CZ80MsxDos::op_CP_memIXpV		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xBF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xC9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xCA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xCB, &CZ80MsxDos::op_EXTENDED_2IX2	));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xCC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xCD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xCE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xCF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xD9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xDA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xDB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xDC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xDD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xDE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xDF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE1, &CZ80MsxDos::op_POP_IX			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE3, &CZ80MsxDos::op_EX_memSP_IX		));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE5, &CZ80MsxDos::op_PUSH_IX			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xE9, &CZ80MsxDos::op_JP_memIX			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xEA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xEB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xEC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xED, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xEE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xEF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xF9, &CZ80MsxDos::op_LD_SP_IX			));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xFA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xFB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xFC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xFD,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xFE,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX.push_back(Z80OPECODE_FUNC( 0xFF,	&CZ80MsxDos::op_UNDEFINED));

	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x00, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x01, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x02, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x03, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x04, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x05, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x06, &CZ80MsxDos::op_RLC_memVpIX		));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x07, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x08, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x09, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x0A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x0B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x0C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x0D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x0E, &CZ80MsxDos::op_RRC_memVpIX		));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x0F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x10, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x11, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x12, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x13, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x14, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x15, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x16, &CZ80MsxDos::op_RL_memVpIX		));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x17, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x18, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x19, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x1A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x1B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x1C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x1D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x1E, &CZ80MsxDos::op_RR_memVpIX		));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x1F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x20, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x21, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x22, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x23, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x24, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x25, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x26, &CZ80MsxDos::op_SLA_memVpIX		));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x27, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x28, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x29, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x2A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x2B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x2C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x2D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x2E, &CZ80MsxDos::op_SRA_memVpIX		));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x2F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x30, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x31, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x32, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x33, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x34, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x35, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x36, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x37, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x38, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x39, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x3A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x3B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x3C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x3D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x3E, &CZ80MsxDos::op_SRL_memVpIX		));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x3F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x40, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x41, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x42, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x43, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x44, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x45, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x46, &CZ80MsxDos::op_BIT_0_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x47, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x48, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x49, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x4A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x4B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x4C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x4D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x4E, &CZ80MsxDos::op_BIT_1_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x4F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x50, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x51, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x52, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x53, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x54, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x55, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x56, &CZ80MsxDos::op_BIT_2_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x57, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x58, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x59, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x5A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x5B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x5C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x5D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x5E, &CZ80MsxDos::op_BIT_3_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x5F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x60, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x61, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x62, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x63, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x64, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x65, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x66, &CZ80MsxDos::op_BIT_4_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x67, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x68, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x69, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x6A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x6B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x6C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x6D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x6E, &CZ80MsxDos::op_BIT_5_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x6F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x70, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x71, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x72, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x73, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x74, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x75, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x76, &CZ80MsxDos::op_BIT_6_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x77, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x78, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x79, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x7A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x7B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x7C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x7D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x7E, &CZ80MsxDos::op_BIT_7_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x7F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x80, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x81, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x82, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x83, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x84, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x85, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x86, &CZ80MsxDos::op_RES_0_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x87, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x88, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x89, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x8A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x8B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x8C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x8D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x8E, &CZ80MsxDos::op_RES_1_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x8F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x90, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x91, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x92, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x93, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x94, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x95, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x96, &CZ80MsxDos::op_RES_2_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x97, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x98, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x99, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x9A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x9B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x9C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x9D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x9E, &CZ80MsxDos::op_RES_3_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0x9F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA6, &CZ80MsxDos::op_RES_4_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xA9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xAA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xAB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xAC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xAD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xAE, &CZ80MsxDos::op_RES_5_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xAF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB6, &CZ80MsxDos::op_RES_6_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xB9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xBA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xBB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xBC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xBD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xBE, &CZ80MsxDos::op_RES_7_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xBF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC6, &CZ80MsxDos::op_SET_0_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xC9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xCA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xCB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xCC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xCD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xCE, &CZ80MsxDos::op_SET_1_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xCF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD6, &CZ80MsxDos::op_SET_2_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xD9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xDA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xDB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xDC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xDD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xDE, &CZ80MsxDos::op_SET_3_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xDF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE6, &CZ80MsxDos::op_SET_4_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xE9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xEA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xEB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xEC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xED, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xEE, &CZ80MsxDos::op_SET_5_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xEF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF6, &CZ80MsxDos::op_SET_6_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xF9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xFA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xFB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xFC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xFD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xFE, &CZ80MsxDos::op_SET_7_memVpIX	));
	OpCode_Extended2IX2.push_back(Z80OPECODE_FUNC( 0xFF, &CZ80MsxDos::op_UNDEFINED));

	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x00, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x01, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x02, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x03,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x04,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x05,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x06, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x07, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x08, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x09, &CZ80MsxDos::op_ADD_IY_BC		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x0A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x0B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x0C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x0D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x0E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x0F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x10, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x11, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x12, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x13, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x14, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x15, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x16, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x17, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x18, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x19, &CZ80MsxDos::op_ADD_IY_DE		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x1A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x1B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x1C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x1D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x1E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x1F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x20, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x21, &CZ80MsxDos::op_LD_IY_ad			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x22, &CZ80MsxDos::op_LD_memAD_IY		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x23, &CZ80MsxDos::op_INC_IY			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x24, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x25, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x26, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x27, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x28, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x29, &CZ80MsxDos::op_ADD_IY_IY		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x2A, &CZ80MsxDos::op_LD_IY_memAD		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x2B, &CZ80MsxDos::op_DEC_IY			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x2C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x2D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x2E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x2F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x30, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x31, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x32, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x33, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x34, &CZ80MsxDos::op_INC_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x35, &CZ80MsxDos::op_DEC_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x36, &CZ80MsxDos::op_LD_memIYpV_v		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x37, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x38, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x39, &CZ80MsxDos::op_ADD_IY_SP		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x3A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x3B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x3C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x3D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x3E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x3F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x40, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x41, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x42, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x43, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x44, &CZ80MsxDos::op_LD_B_IYH		));	// undoc.Z80
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x45, &CZ80MsxDos::op_LD_B_IYL		));	// undoc.Z80
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x46, &CZ80MsxDos::op_LD_B_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x47, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x48, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x49, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x4A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x4B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x4C, &CZ80MsxDos::op_LD_C_IYH		));	// undoc.Z80
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x4D, &CZ80MsxDos::op_LD_C_IYL		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x4E, &CZ80MsxDos::op_LD_C_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x4F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x50, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x51, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x52, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x53, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x54, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x55, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x56, &CZ80MsxDos::op_LD_D_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x57, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x58, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x59, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x5A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x5B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x5C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x5D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x5E, &CZ80MsxDos::op_LD_E_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x5F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x60, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x61, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x62, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x63, &CZ80MsxDos::op_LD_IYH_E		));	// undoc.Z80
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x64, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x65, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x66, &CZ80MsxDos::op_LD_H_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x67, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x68, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x69, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x6A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x6B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x6C, &CZ80MsxDos::op_LD_IYL_IYH		));	// undoc.Z80
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x6D, &CZ80MsxDos::op_LD_IYL_IYL		));	// undoc.Z80
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x6E, &CZ80MsxDos::op_LD_L_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x6F, &CZ80MsxDos::op_LD_IYL_A		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x70, &CZ80MsxDos::op_LD_memIYpV_B	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x71, &CZ80MsxDos::op_LD_memIYpV_C	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x72, &CZ80MsxDos::op_LD_memIYpV_D	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x73, &CZ80MsxDos::op_LD_memIYpV_E	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x74, &CZ80MsxDos::op_LD_memIYpV_H	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x75, &CZ80MsxDos::op_LD_memIYpV_L	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x76, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x77, &CZ80MsxDos::op_LD_memIYpV_A	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x78, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x79, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x7A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x7B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x7C, &CZ80MsxDos::op_LD_A_IYH			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x7D, &CZ80MsxDos::op_LD_A_IYL			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x7E, &CZ80MsxDos::op_LD_A_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x7F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x80, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x81, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x82, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x83, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x84, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x85, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x86, &CZ80MsxDos::op_ADD_A_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x87, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x88, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x89, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x8A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x8B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x8C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x8D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x8E, &CZ80MsxDos::op_ADC_A_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x8F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x90, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x91, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x92, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x93, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x94, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x95, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x96, &CZ80MsxDos::op_SUB_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x97, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x98, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x99, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x9A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x9B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x9C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x9D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x9E, &CZ80MsxDos::op_SBC_A_memIYpV	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0x9F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA6, &CZ80MsxDos::op_AND_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xA9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xAA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xAB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xAC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xAD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xAE, &CZ80MsxDos::op_XOR_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xAF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB6, &CZ80MsxDos::op_OR_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xB9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xBA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xBB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xBC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xBD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xBE, &CZ80MsxDos::op_CP_memIYpV		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xBF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xC9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xCA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xCB, &CZ80MsxDos::op_EXTENDED_4IY2	));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xCC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xCD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xCE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xCF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xD9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xDA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xDB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xDC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xDD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xDE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xDF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE1, &CZ80MsxDos::op_POP_IY			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE3, &CZ80MsxDos::op_EX_memSP_IY		));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE5, &CZ80MsxDos::op_PUSH_IY			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xE9, &CZ80MsxDos::op_JP_memIY			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xEA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xEB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xEC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xED, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xEE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xEF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xF9, &CZ80MsxDos::op_LD_SP_IY			));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xFA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xFB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xFC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xFD,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xFE,	&CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY.push_back(Z80OPECODE_FUNC( 0xFF,	&CZ80MsxDos::op_UNDEFINED));

	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x00, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x01, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x02, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x03, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x04, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x05, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x06, &CZ80MsxDos::op_RLC_memVpIY		));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x07, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x08, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x09, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x0A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x0B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x0C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x0D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x0E, &CZ80MsxDos::op_RRC_memVpIY		));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x0F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x10, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x11, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x12, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x13, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x14, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x15, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x16, &CZ80MsxDos::op_RL_memVpIY		));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x17, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x18, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x19, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x1A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x1B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x1C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x1D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x1E, &CZ80MsxDos::op_RR_memVpIY		));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x1F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x20, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x21, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x22, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x23, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x24, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x25, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x26, &CZ80MsxDos::op_SLA_memVpIY		));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x27, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x28, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x29, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x2A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x2B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x2C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x2D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x2E, &CZ80MsxDos::op_SRA_memVpIY		));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x2F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x30, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x31, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x32, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x33, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x34, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x35, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x36, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x37, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x38, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x39, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x3A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x3B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x3C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x3D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x3E, &CZ80MsxDos::op_SRL_memVpIY		));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x3F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x40, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x41, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x42, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x43, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x44, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x45, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x46, &CZ80MsxDos::op_BIT_0_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x47, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x48, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x49, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x4A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x4B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x4C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x4D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x4E, &CZ80MsxDos::op_BIT_1_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x4F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x50, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x51, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x52, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x53, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x54, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x55, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x56, &CZ80MsxDos::op_BIT_2_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x57, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x58, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x59, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x5A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x5B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x5C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x5D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x5E, &CZ80MsxDos::op_BIT_3_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x5F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x60, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x61, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x62, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x63, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x64, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x65, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x66, &CZ80MsxDos::op_BIT_4_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x67, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x68, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x69, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x6A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x6B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x6C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x6D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x6E, &CZ80MsxDos::op_BIT_5_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x6F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x70, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x71, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x72, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x73, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x74, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x75, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x76, &CZ80MsxDos::op_BIT_6_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x77, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x78, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x79, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x7A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x7B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x7C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x7D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x7E, &CZ80MsxDos::op_BIT_7_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x7F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x80, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x81, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x82, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x83, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x84, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x85, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x86, &CZ80MsxDos::op_RES_0_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x87, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x88, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x89, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x8A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x8B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x8C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x8D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x8E, &CZ80MsxDos::op_RES_1_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x8F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x90, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x91, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x92, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x93, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x94, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x95, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x96, &CZ80MsxDos::op_RES_2_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x97, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x98, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x99, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x9A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x9B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x9C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x9D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x9E, &CZ80MsxDos::op_RES_3_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0x9F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA6, &CZ80MsxDos::op_RES_4_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xA9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xAA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xAB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xAC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xAD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xAE, &CZ80MsxDos::op_RES_5_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xAF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB6, &CZ80MsxDos::op_RES_6_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xB9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xBA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xBB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xBC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xBD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xBE, &CZ80MsxDos::op_RES_7_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xBF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC6, &CZ80MsxDos::op_SET_0_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xC9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xCA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xCB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xCC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xCD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xCE, &CZ80MsxDos::op_SET_1_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xCF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD6, &CZ80MsxDos::op_SET_2_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xD9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xDA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xDB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xDC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xDD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xDE, &CZ80MsxDos::op_SET_3_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xDF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE6, &CZ80MsxDos::op_SET_4_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xE9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xEA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xEB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xEC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xED, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xEE, &CZ80MsxDos::op_SET_5_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xEF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF6, &CZ80MsxDos::op_SET_6_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xF9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xFA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xFB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xFC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xFD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xFE, &CZ80MsxDos::op_SET_7_memVpIY	));
	OpCode_Extended4IY2.push_back(Z80OPECODE_FUNC( 0xFF, &CZ80MsxDos::op_UNDEFINED));

#ifdef NDEBUG
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x00, &CZ80MsxDos::op_UNDEFINED ));
#else
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x00, &CZ80MsxDos::op_DEBUGBREAK ));
#endif
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x01, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x02, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x03, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x04, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x05, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x06, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x07, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x08, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x09, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x0A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x0B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x0C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x0D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x0E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x0F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x10, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x11, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x12, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x13, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x14, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x15, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x16, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x17, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x18, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x19, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x1A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x1B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x1C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x1D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x1E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x1F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x20, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x21, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x22, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x23, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x24, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x25, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x26, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x27, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x28, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x29, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x2A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x2B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x2C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x2D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x2E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x2F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x30, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x31, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x32, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x33, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x34, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x35, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x36, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x37, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x38, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x39, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x3A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x3B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x3C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x3D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x3E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x3F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x40, &CZ80MsxDos::op_IN_B_memC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x41, &CZ80MsxDos::op_OUT_memC_B	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x42, &CZ80MsxDos::op_SBC_HL_BC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x43, &CZ80MsxDos::op_LD_memAD_BC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x44, &CZ80MsxDos::op_NEG			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x45, &CZ80MsxDos::op_RETN			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x46, &CZ80MsxDos::op_IM_0			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x47, &CZ80MsxDos::op_LD_i_A		));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x48, &CZ80MsxDos::op_IN_C_memC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x49, &CZ80MsxDos::op_OUT_memC_C	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x4A, &CZ80MsxDos::op_ADC_HL_BC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x4B, &CZ80MsxDos::op_LD_BC_memAD	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x4C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x4D, &CZ80MsxDos::op_RETI			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x4E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x4F, &CZ80MsxDos::op_LD_R_A		));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x50, &CZ80MsxDos::op_IN_D_memC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x51, &CZ80MsxDos::op_OUT_memC_D	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x52, &CZ80MsxDos::op_SBC_HL_DE	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x53, &CZ80MsxDos::op_LD_memAD_DE	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x54, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x55, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x56, &CZ80MsxDos::op_IM_1			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x57, &CZ80MsxDos::op_LD_A_i		));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x58, &CZ80MsxDos::op_IN_E_memC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x59, &CZ80MsxDos::op_OUT_memC_E	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x5A, &CZ80MsxDos::op_ADC_HL_DE	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x5B, &CZ80MsxDos::op_LD_DE_memAD	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x5C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x5D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x5E, &CZ80MsxDos::op_IM_2			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x5F, &CZ80MsxDos::op_LD_A_R		));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x60, &CZ80MsxDos::op_IN_H_memC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x61, &CZ80MsxDos::op_OUT_memC_H	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x62, &CZ80MsxDos::op_SBC_HL_HL	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x63, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x64, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x65, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x66, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x67, &CZ80MsxDos::op_RRD			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x68, &CZ80MsxDos::op_IN_L_memC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x69, &CZ80MsxDos::op_OUT_memC_L	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x6A, &CZ80MsxDos::op_ADC_HL_HL	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x6B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x6C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x6D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x6E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x6F, &CZ80MsxDos::op_RLD			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x70, &CZ80MsxDos::op_UNDEFINED));	// &CZ80MsxDos::op_IN_memC
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x71, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x72, &CZ80MsxDos::op_SBC_HL_SP	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x73, &CZ80MsxDos::op_LD_memAD_SP	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x74, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x75, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x76, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x77, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x78, &CZ80MsxDos::op_IN_A_memC	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x79, &CZ80MsxDos::op_OUT_memC_A	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x7A, &CZ80MsxDos::op_ADC_HL_SP	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x7B, &CZ80MsxDos::op_LD_SP_memAD	));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x7C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x7D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x7E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x7F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x80, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x81, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x82, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x83, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x84, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x85, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x86, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x87, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x88, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x89, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x8A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x8B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x8C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x8D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x8E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x8F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x90, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x91, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x92, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x93, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x94, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x95, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x96, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x97, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x98, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x99, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x9A, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x9B, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x9C, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x9D, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x9E, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0x9F, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA0, &CZ80MsxDos::op_LDI			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA1, &CZ80MsxDos::op_CPI			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA2, &CZ80MsxDos::op_INI			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA3, &CZ80MsxDos::op_OUTI			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA8, &CZ80MsxDos::op_LDD			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xA9, &CZ80MsxDos::op_CPD			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xAA, &CZ80MsxDos::op_IND			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xAB, &CZ80MsxDos::op_OUTD			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xAC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xAD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xAE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xAF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB0, &CZ80MsxDos::op_LDIR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB1, &CZ80MsxDos::op_CPIR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB2, &CZ80MsxDos::op_INIR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB3, &CZ80MsxDos::op_OTIR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB8, &CZ80MsxDos::op_LDDR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xB9, &CZ80MsxDos::op_CPDR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xBA, &CZ80MsxDos::op_INDR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xBB, &CZ80MsxDos::op_OUTR			));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xBC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xBD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xBE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xBF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xC9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xCA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xCB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xCC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xCD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xCE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xCF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xD9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xDA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xDB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xDC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xDD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xDE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xDF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xE9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xEA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xEB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xEC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xED, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xEE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xEF, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF0, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF1, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF2, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF3, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF4, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF5, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF6, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF7, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF8, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xF9, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xFA, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xFB, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xFC, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xFD, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xFE, &CZ80MsxDos::op_UNDEFINED));
	OpCode_Extended3.push_back(Z80OPECODE_FUNC( 0xFF, &CZ80MsxDos::op_UNDEFINED));
	return;
};


void CZ80MsxDos::op_UNDEFINED()
{
	std::wcout
		<< _T("\n **HopStepZ >> Undefined Z80 operation code in ")
		<< std::hex << std::setfill(_T('0')) << std::setw(4) << (int)m_R.PC << _T("h\n");
	assert(false);
}
// single
void CZ80MsxDos::op_NOP()
{
	// do nothing
	return;
}
void CZ80MsxDos::op_LD_BC_ad()
{
	m_R.C = m_pMemSys->Read(m_R.PC++);
	m_R.B = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_LD_memBC_A()
{
	m_pMemSys->Write(m_R.GetBC(), m_R.A);
	return;
}
void CZ80MsxDos::op_INC_BC()
{
	// 16ビットINCはフラグを変化させない
	m_R.SetBC( static_cast<uint16_t>(m_R.GetBC()+1) );
	return;
}
void CZ80MsxDos::op_INC_B()
{
	m_R.Inc8( &m_R.B );
	return;
}
void CZ80MsxDos::op_DEC_B()
{
	m_R.Dec8( &m_R.B );
	return;
}
void CZ80MsxDos::op_LD_B_v()
{
	// フラグ変化なし
	m_R.B = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_RLCA()
{
	const uint8_t temp = (m_R.A >> 7) & 0x01;
	m_R.A = (m_R.A << 1) | temp;
	//
	m_R.F.C = temp;
	m_R.F.Z = m_R.F.Z;
	m_R.F.PV= m_R.F.PV;
	m_R.F.S = m_R.F.S;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_EX_AF_AF()
{
	// フラグの状態は変化しない
	m_R.Swap(&m_R.A, &m_R.Ad);
	m_R.Swap(&m_R.F, &m_R.Fd);
	return;
}
void CZ80MsxDos::op_ADD_HL_BC()
{
	uint16_t hl = m_R.GetHL();
	m_R.Add16(&hl, m_R.GetBC());
	m_R.SetHL(hl);
	return;
}
void CZ80MsxDos::op_LD_A_memBC()
{
	// フラグ変化なし
	m_R.A = m_pMemSys->Read(m_R.GetBC());
	return;
}

void CZ80MsxDos::op_DEC_BC()
{
	// 16ビットDECはフラグを変化させない
	m_R.SetBC( static_cast<uint16_t>(m_R.GetBC()-1) );
	return;
}
void CZ80MsxDos::op_INC_C()
{
	m_R.Inc8(&m_R.C);
	return;
}
void CZ80MsxDos::op_DEC_C()
{
	m_R.Dec8(&m_R.C);
	return;
}
void CZ80MsxDos::op_LD_C_v()
{
	// フラグ変化なし
	m_R.C = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_RRCA()
{
	const uint8_t temp = m_R.A & 0x01;
	m_R.A = (m_R.A >> 1) | (temp << 7);
	//
	m_R.F.C = temp;
	m_R.F.Z = m_R.F.Z;
	m_R.F.PV= m_R.F.PV;
	m_R.F.S = m_R.F.S;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_DJNZ_v()
{
	m_R.B--;
	if( m_R.B == 0 ){
		m_R.PC++;	// v を読み捨て
	}
	else{
		int8_t off = m_pMemSys->ReadInt8(m_R.PC);
		m_R.PC = static_cast<uint16_t>(static_cast<int32_t>(m_R.PC-1) + 2 + off);
	}
	return;
}
void CZ80MsxDos::op_LD_DE_ad()
{
	m_R.E = m_pMemSys->Read(m_R.PC++);
	m_R.D = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_LD_memDE_A()
{
	uint16_t ad = m_R.GetDE();
	m_pMemSys->Write(ad, m_R.A);
	return;
}
void CZ80MsxDos::op_INC_DE()
{
	// 16ビットINCはフラグを変化させない
	m_R.SetDE(static_cast<uint16_t>(m_R.GetDE() + 1));
	return;
}
void CZ80MsxDos::op_INC_D()
{
	m_R.Inc8(&m_R.D);
	return;
}
void CZ80MsxDos::op_DEC_D()
{
	m_R.Dec8(&m_R.D);
	return;
}
void CZ80MsxDos::op_LD_D_v()
{
	// フラグ変化なし
	m_R.D = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_RLA()
{
	const uint8_t temp = (m_R.A >> 7) & 0x01;
	m_R.A = (m_R.A << 1) | static_cast<uint8_t>(m_R.F.C);
	//
	m_R.F.C = temp;
	m_R.F.Z = m_R.F.Z;
	m_R.F.PV= m_R.F.PV;
	m_R.F.S = m_R.F.S;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_JR_v()
{
	int8_t off = m_pMemSys->ReadInt8(m_R.PC);
	m_R.PC = static_cast<uint16_t>(static_cast<int32_t>(m_R.PC-1) + 2 + off);
	return;
}
void CZ80MsxDos::op_ADD_HL_DE()
{
	uint16_t hl = m_R.GetHL();
	m_R.Add16(&hl, m_R.GetDE());
	m_R.SetHL(hl);
	return;
}
void CZ80MsxDos::op_LD_A_memDE()
{
	uint16_t ad = m_R.GetDE();
	m_R.A = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_DEC_DE()
{
	// 16ビットDECはフラグを変化させない
	m_R.SetDE( static_cast<uint16_t>(m_R.GetDE()-1) );
	return;
}
void CZ80MsxDos::op_INC_E()
{
	m_R.Inc8(&m_R.E);
	return;
}
void CZ80MsxDos::op_DEC_E()
{
	m_R.Dec8(&m_R.E);
	return;
}
void CZ80MsxDos::op_LD_E_v()
{
	// フラグ変化なし
	m_R.E = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_RRA()
{
	const uint8_t temp = m_R.A & 0x01;
	m_R.A = (m_R.A >> 1) | (m_R.F.C << 7);
	//
	m_R.F.C = temp;
	m_R.F.Z = m_R.F.Z;
	m_R.F.PV= m_R.F.PV;
	m_R.F.S = m_R.F.S;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_JR_nz_v()
{
	if( m_R.F.Z == 0 ){
		int8_t off = m_pMemSys->ReadInt8(m_R.PC);
		m_R.PC = static_cast<uint16_t>(static_cast<int32_t>(m_R.PC-1) + 2 + off);
	}
	else{
		m_R.PC++;	// v を読み捨て
	}
	return;
}
void CZ80MsxDos::op_LD_HL_ad()
{
	m_R.L = m_pMemSys->Read(m_R.PC++);
	m_R.H = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_LD_memAD_HL()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_pMemSys->Write(ad+0, m_R.L);
	m_pMemSys->Write(ad+1, m_R.H);
	return;
}
void CZ80MsxDos::op_INC_HL()
{
	// 16ビットINCはフラグを変化させない
	m_R.SetHL( static_cast<uint16_t>(m_R.GetHL()+1) );
	return;
}
void CZ80MsxDos::op_INC_H()
{
	m_R.Inc8(&m_R.H);
	return;
}
void CZ80MsxDos::op_DEC_H()
{
	m_R.Dec8(&m_R.H);
	return;
}
void CZ80MsxDos::op_LD_H_v()
{
	// フラグ変化なし
	m_R.H = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_DAA()
{
	const uint8_t Lo = (m_R.A >> 0) & 0x0f;
	const uint8_t Hi = (m_R.A >> 4) & 0x0f;
	// ややこしいので、Z80ファミリハンドブック(第七版) P.86の機能表をそのままIF構文で実装する
	// 動けばいいいんだよ
	uint8_t v = 0;
	if( m_R.F.N == 0 ){
		if( m_R.F.C == 0 && (0x0 <= Hi && Hi <= 0x9) && m_R.F.H == 0 && (0x0 <= Lo && Lo <= 0x9)) v = 0x00, m_R.F.C = 0;
		if( m_R.F.C == 0 && (0x0 <= Hi && Hi <= 0x8) && m_R.F.H == 0 && (0xA <= Lo && Lo <= 0xF)) v = 0x06, m_R.F.C = 0;
		if( m_R.F.C == 0 && (0x0 <= Hi && Hi <= 0x9) && m_R.F.H == 1 && (0x0 <= Lo && Lo <= 0x3)) v = 0x06, m_R.F.C = 0;
		if( m_R.F.C == 0 && (0xA <= Hi && Hi <= 0xF) && m_R.F.H == 1 && (0x0 <= Lo && Lo <= 0x9)) v = 0x60, m_R.F.C = 1;
		if( m_R.F.C == 0 && (0x9 <= Hi && Hi <= 0xF) && m_R.F.H == 0 && (0xA <= Lo && Lo <= 0xF)) v = 0x66, m_R.F.C = 1;
		if( m_R.F.C == 0 && (0xA <= Hi && Hi <= 0xF) && m_R.F.H == 1 && (0x0 <= Lo && Lo <= 0x3)) v = 0x66, m_R.F.C = 1;
		if( m_R.F.C == 1 && (0x0 <= Hi && Hi <= 0x2) && m_R.F.H == 0 && (0x0 <= Lo && Lo <= 0x9)) v = 0x60, m_R.F.C = 1;
		if( m_R.F.C == 1 && (0x0 <= Hi && Hi <= 0x2) && m_R.F.H == 0 && (0xA <= Lo && Lo <= 0xF)) v = 0x66, m_R.F.C = 1;
		if( m_R.F.C == 1 && (0x0 <= Hi && Hi <= 0x3) && m_R.F.H == 1 && (0x0 <= Lo && Lo <= 0x3)) v = 0x66, m_R.F.C = 1;
		m_R.A += v;
		m_R.F.H = ((m_R.F.C!=0)||(Hi<((m_R.A>>4)&0x0F)))?1:0;
	}else{
		if( m_R.F.C == 0 && (0x0 <= Hi && Hi <= 0x9) && m_R.F.H == 0 && (0x0 <= Lo && Lo <= 0x9)) v = 0x00, m_R.F.C = 0;
		if( m_R.F.C == 0 && (0x0 <= Hi && Hi <= 0x8) && m_R.F.H == 1 && (0x6 <= Lo && Lo <= 0xF)) v = 0xFA, m_R.F.C = 0;
		if( m_R.F.C == 1 && (0x7 <= Hi && Hi <= 0xF) && m_R.F.H == 0 && (0x0 <= Lo && Lo <= 0x9)) v = 0xA0, m_R.F.C = 1;
		if( m_R.F.C == 1 && (0x6 <= Hi && Hi <= 0xF) && m_R.F.H == 1 && (0x6 <= Lo && Lo <= 0xF)) v = 0x9A, m_R.F.C = 1;
		m_R.A += v;
		m_R.F.H = ((m_R.F.C!=0)||(((m_R.A>>4)&0xF)<Hi))?1:0;
	}
	m_R.F.Z = (m_R.A==0)?1:0;		// 0になったか
	m_R.F.PV= CZ80Regs::CheckParytyEven(m_R.A);
	m_R.F.S = (m_R.A>=0x80)?1:0;	// 符号付きか
	m_R.F.N = m_R.F.N;				// 変化なし
	return;
}
void CZ80MsxDos::op_JR_z_v()
{
	if( m_R.F.Z != 0 ){
		int8_t off = m_pMemSys->ReadInt8(m_R.PC);
		m_R.PC = static_cast<uint16_t>(static_cast<int32_t>(m_R.PC-1) + 2 + off);
	}
	else{
		m_R.PC++;	// v を読み捨て
	}
	return;
}
void CZ80MsxDos::op_ADD_HL_HL()
{
	uint16_t hl = m_R.GetHL();
	m_R.Add16(&hl, m_R.GetHL());
	m_R.SetHL(hl);
	return;
}
void CZ80MsxDos::op_LD_HL_memAD()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_R.L = m_pMemSys->Read(ad+0);
	m_R.H = m_pMemSys->Read(ad+1);
	return;
}
void CZ80MsxDos::op_DEC_HL()
{
	// 16ビットDECはフラグを変化させない
	m_R.SetHL( static_cast<uint16_t>(m_R.GetHL()-1) );
	return;
}
void CZ80MsxDos::op_INC_L()
{
	m_R.Inc8(&m_R.L);
	return;
}
void CZ80MsxDos::op_DEC_L()
{
	m_R.Dec8(&m_R.L);
	return;
}
void CZ80MsxDos::op_LD_L_v()
{
	// フラグ変化なし
	m_R.L = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_CPL()
{
	m_R.A = m_R.A ^ 0xff;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = m_R.F.Z;	 // Zが変化しないことに注意
	m_R.F.PV= m_R.F.PV;
	m_R.F.S = m_R.F.S;
	m_R.F.N = 1;
	m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_JR_nc_v()
{
	if( m_R.F.C == 0 ){
		int8_t off = m_pMemSys->ReadInt8(m_R.PC);
		m_R.PC = static_cast<uint16_t>(static_cast<int32_t>(m_R.PC-1) + 2 + off);
	}
	else{
		m_R.PC++;	// vを読み捨て
	}
	return;
}
void CZ80MsxDos::op_LD_SP_ad()
{
	m_R.SP = m_pMemSys->Read(m_R.PC++);
	m_R.SP |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	return;
}
void CZ80MsxDos::op_LD_memAD_A()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_pMemSys->Write(ad, m_R.A);
	return;
}
void CZ80MsxDos::op_INC_SP()
{
	// 16ビットINCはフラグを変化させない
	++m_R.SP;
	return;
}
void CZ80MsxDos::op_INC_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t temp = m_pMemSys->Read(ad);
	m_R.Inc8(&temp);
	m_pMemSys->Write(ad, temp);
	return;
}
void CZ80MsxDos::op_DEC_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t temp = m_pMemSys->Read(ad);
	m_R.Dec8(&temp);
	m_pMemSys->Write(ad, temp);
	return;
}
void CZ80MsxDos::op_LD_memHL_v()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SCF()
{
	m_R.F.C = 1;
	return;
}
void CZ80MsxDos::op_JR_C_v()
{
	if( m_R.F.C != 0 ){
		int8_t off = m_pMemSys->ReadInt8(m_R.PC);
		m_R.PC = static_cast<uint16_t>(static_cast<int32_t>(m_R.PC-1) + 2 + off);
	}
	else{
		m_R.PC++;	// v を読み捨て
	}
	return;
}
void CZ80MsxDos::op_ADD_HL_SP()
{
	uint16_t hl = m_R.GetHL();
	m_R.Add16(&hl, m_R.SP);
	m_R.SetHL(hl);
	return;
}
void CZ80MsxDos::op_LD_A_memAD()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_R.A = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_DEC_SP()
{
	// 16ビットDECはフラグを変化させない
	--m_R.SP;
	return;
}
void CZ80MsxDos::op_INC_A()
{
	m_R.Inc8(&m_R.A);
	return;
}
void CZ80MsxDos::op_DEC_A()
{
	m_R.Dec8(&m_R.A);
	return;
}

void CZ80MsxDos::op_LD_A_v()
{
	m_R.A = m_pMemSys->Read(m_R.PC++);
	return;
}
void CZ80MsxDos::op_CCF()
{
	m_R.F.C = (m_R.F.C==0)?1:0;
	return;
}
void CZ80MsxDos::op_LD_B_B()
{
	m_R.B = m_R.B;
	return;
}
void CZ80MsxDos::op_LD_B_C()
{
	m_R.B = m_R.C;
	return;
}
void CZ80MsxDos::op_LD_B_D()
{
	m_R.B = m_R.D;
	return;
}
void CZ80MsxDos::op_LD_B_E()
{
	m_R.B = m_R.E;
	return;
}
void CZ80MsxDos::op_LD_B_H()
{
	m_R.B = m_R.H;
	return;
}
void CZ80MsxDos::op_LD_B_L()
{
	m_R.B = m_R.L;
	return;
}
void CZ80MsxDos::op_LD_B_memHL()
{
	uint16_t ad = m_R.GetHL();
	m_R.B = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_B_A()
{
	m_R.B = m_R.A;
	return;
}
void CZ80MsxDos::op_LD_C_B()
{
	m_R.C = m_R.B;
	return;
}
void CZ80MsxDos::op_LD_C_C()
{
	m_R.C = m_R.C;
	return;
}
void CZ80MsxDos::op_LD_C_D()
{
	m_R.C = m_R.D;
	return;
}
void CZ80MsxDos::op_LD_C_E()
{
	m_R.C = m_R.E;
	return;
}
void CZ80MsxDos::op_LD_C_H()
{
	m_R.C = m_R.H;
	return;
}
void CZ80MsxDos::op_LD_C_L()
{
	m_R.C = m_R.L;
	return;
}
void CZ80MsxDos::op_LD_C_memHL()
{
	uint16_t ad = m_R.GetHL();
	m_R.C = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_C_A()
{
	m_R.C = m_R.A;
	return;
}
void CZ80MsxDos::op_LD_D_B()
{
	m_R.D = m_R.B;
	return;
}
void CZ80MsxDos::op_LD_D_C()
{
	m_R.D = m_R.C;
	return;
}
void CZ80MsxDos::op_LD_D_D()
{
	m_R.D = m_R.D;
	return;
}
void CZ80MsxDos::op_LD_D_E()
{
	m_R.D = m_R.E;
	return;
}
void CZ80MsxDos::op_LD_D_H()
{
	m_R.D = m_R.H;
	return;
}
void CZ80MsxDos::op_LD_D_L()
{
	m_R.D = m_R.L;
	return;
}
void CZ80MsxDos::op_LD_D_memHL()
{
	uint16_t ad = m_R.GetHL();
	m_R.D = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_D_A()
{
	m_R.D = m_R.A;
	return;
}
void CZ80MsxDos::op_LD_E_B()
{
	m_R.E = m_R.B;
	return;
}
void CZ80MsxDos::op_LD_E_C()
{
	m_R.E = m_R.C;
	return;
}
void CZ80MsxDos::op_LD_E_D()
{
	m_R.E = m_R.D;
	return;
}
void CZ80MsxDos::op_LD_E_E()
{
	m_R.E = m_R.E;
	return;
}
void CZ80MsxDos::op_LD_E_H()
{
	m_R.E = m_R.H;
	return;
}
void CZ80MsxDos::op_LD_E_L()
{
	m_R.E = m_R.L;
	return;
}
void CZ80MsxDos::op_LD_E_memHL()
{
	uint16_t ad = m_R.GetHL();
	m_R.E = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_E_A()
{
	m_R.E = m_R.A;
	return;
}
void CZ80MsxDos::op_LD_H_B()
{
	m_R.H = m_R.B;
	return;
}
void CZ80MsxDos::op_LD_H_C()
{
	m_R.H = m_R.C;
	return;
}
void CZ80MsxDos::op_LD_H_D()
{
	m_R.H = m_R.D;
	return;
}
void CZ80MsxDos::op_LD_H_E()
{
	m_R.H = m_R.E;
	return;
}
void CZ80MsxDos::op_LD_H_H()
{
	m_R.H = m_R.H;
	return;
}
void CZ80MsxDos::op_LD_H_L()
{
	m_R.H = m_R.L;
	return;
}
void CZ80MsxDos::op_LD_H_memHL()
{
	uint16_t ad = m_R.GetHL();
	m_R.H = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_H_A()
{
	m_R.H = m_R.A;
	return;
}
void CZ80MsxDos::op_LD_L_B()
{
	m_R.L = m_R.B;
	return;
}
void CZ80MsxDos::op_LD_L_C()
{
	m_R.L = m_R.C;
	return;
}
void CZ80MsxDos::op_LD_L_D()
{
	m_R.L = m_R.D;
	return;
}
void CZ80MsxDos::op_LD_L_E()
{
	m_R.L = m_R.E;
	return;
}
void CZ80MsxDos::op_LD_L_H()
{
	m_R.L = m_R.H;
	return;
}
void CZ80MsxDos::op_LD_L_L()
{
	m_R.L = m_R.L;
	return;
}
void CZ80MsxDos::op_LD_L_memHL()
{
	uint16_t ad = m_R.GetHL();
	m_R.L = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_L_A()
{
	m_R.L = m_R.A;
	return;
}
void CZ80MsxDos::op_LD_memHL_B()
{
	uint16_t ad = m_R.GetHL();
	m_pMemSys->Write(ad, m_R.B);
	return;
}
void CZ80MsxDos::op_LD_memHL_C()
{
	uint16_t ad = m_R.GetHL();
	m_pMemSys->Write(ad, m_R.C);
	return;
}
void CZ80MsxDos::op_LD_memHL_D()
{
	uint16_t ad = m_R.GetHL();
	m_pMemSys->Write(ad, m_R.D);
	return;
}
void CZ80MsxDos::op_LD_memHL_E()
{
	uint16_t ad = m_R.GetHL();
	m_pMemSys->Write(ad, m_R.E);
	return;
}
void CZ80MsxDos::op_LD_memHL_H()
{
	uint16_t ad = m_R.GetHL();
	m_pMemSys->Write(ad, m_R.H);
	return;
}
void CZ80MsxDos::op_LD_memHL_L()
{
	uint16_t ad = m_R.GetHL();
	m_pMemSys->Write(ad, m_R.L);
	return;
}
void CZ80MsxDos::op_HALT()
{
	m_bHalt = true;
	return;
}
void CZ80MsxDos::op_LD_memHL_A()
{
	uint16_t ad = m_R.GetHL();
	m_pMemSys->Write(ad, m_R.A);
	return;
}
void CZ80MsxDos::op_LD_A_B()
{
	m_R.A = m_R.B;
	return;
}
void CZ80MsxDos::op_LD_A_C()
{
	m_R.A = m_R.C;
	return;
}
void CZ80MsxDos::op_LD_A_D()
{
	m_R.A = m_R.D;
	return;
}
void CZ80MsxDos::op_LD_A_E()
{
	m_R.A = m_R.E;
	return;
}
void CZ80MsxDos::op_LD_A_H()
{
	m_R.A = m_R.H;
	return;
}
void CZ80MsxDos::op_LD_A_L()
{
	m_R.A = m_R.L;
	return;
}
void CZ80MsxDos::op_LD_A_memHL()
{
	uint16_t ad = m_R.GetHL();
	m_R.A = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_A_A()
{
	m_R.A = m_R.A;
	return;
}

void CZ80MsxDos::op_ADD_A_B()
{
	m_R.Add8(&m_R.A, m_R.B);
	return;
}
void CZ80MsxDos::op_ADD_A_C()
{
	m_R.Add8(&m_R.A, m_R.C);
	return;
}
void CZ80MsxDos::op_ADD_A_D()
{
	m_R.Add8(&m_R.A, m_R.D);
	return;
}
void CZ80MsxDos::op_ADD_A_E()
{
	m_R.Add8(&m_R.A, m_R.E);
	return;
}
void CZ80MsxDos::op_ADD_A_H()
{
	m_R.Add8(&m_R.A, m_R.H);
	return;
}
void CZ80MsxDos::op_ADD_A_L()
{
	m_R.Add8(&m_R.A, m_R.L);
	return;
}
void CZ80MsxDos::op_ADD_A_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Add8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_ADD_A_A()
{
	m_R.Add8(&m_R.A, m_R.A);
	return;
}
void CZ80MsxDos::op_ADC_A_B()
{
	m_R.Add8Cy(&m_R.A, m_R.B, m_R.F.C);
	return;
}
void CZ80MsxDos::op_ADC_A_C()
{
	m_R.Add8Cy(&m_R.A, m_R.C, m_R.F.C);
	return;
}
void CZ80MsxDos::op_ADC_A_D()
{
	m_R.Add8Cy(&m_R.A, m_R.D, m_R.F.C);
	return;
}
void CZ80MsxDos::op_ADC_A_E()
{
	m_R.Add8Cy(&m_R.A, m_R.E, m_R.F.C);
	return;
}
void CZ80MsxDos::op_ADC_A_H()
{
	m_R.Add8Cy(&m_R.A, m_R.H, m_R.F.C);
	return;
}
void CZ80MsxDos::op_ADC_A_L()
{
	m_R.Add8Cy(&m_R.A, m_R.L, m_R.F.C);
	return;
}
void CZ80MsxDos::op_ADC_A_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Add8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_ADC_A_A()
{
	m_R.Add8Cy(&m_R.A, m_R.A, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SUB_B()
{
	m_R.Sub8(&m_R.A, m_R.B);
	return;
}
void CZ80MsxDos::op_SUB_C()
{
	m_R.Sub8(&m_R.A, m_R.C);
	return;
}
void CZ80MsxDos::op_SUB_D()
{
	m_R.Sub8(&m_R.A, m_R.D);
	return;
}
void CZ80MsxDos::op_SUB_E()
{
	m_R.Sub8(&m_R.A, m_R.E);
	return;
}
void CZ80MsxDos::op_SUB_H()
{
	m_R.Sub8(&m_R.A, m_R.H);
	return;
}
void CZ80MsxDos::op_SUB_L()
{
	m_R.Sub8(&m_R.A, m_R.L);
	return;
}
void CZ80MsxDos::op_SUB_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sub8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_SUB_A()
{
	m_R.Sub8(&m_R.A, m_R.A);
	return;
}
void CZ80MsxDos::op_SBC_A_B()
{
	m_R.Sub8Cy(&m_R.A, m_R.B, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SBC_A_C()
{
	m_R.Sub8Cy(&m_R.A, m_R.C, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SBC_A_D()
{
	m_R.Sub8Cy(&m_R.A, m_R.D, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SBC_A_E()
{
	m_R.Sub8Cy(&m_R.A, m_R.E, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SBC_A_H()
{
	m_R.Sub8Cy(&m_R.A, m_R.H, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SBC_A_L()
{
	m_R.Sub8Cy(&m_R.A, m_R.L, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SBC_A_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sub8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SBC_A()
{
	m_R.Sub8Cy(&m_R.A, m_R.A, m_R.F.C);
	return;
}
void CZ80MsxDos::op_AND_B()
{
	m_R.And8(&m_R.A, m_R.B);
	return;
}
void CZ80MsxDos::op_AND_C()
{
	m_R.And8(&m_R.A, m_R.C);
	return;
}
void CZ80MsxDos::op_AND_D()
{
	m_R.And8(&m_R.A, m_R.D);
	return;
}
void CZ80MsxDos::op_AND_E()
{
	m_R.And8(&m_R.A, m_R.E);
	return;
}
void CZ80MsxDos::op_AND_H()
{
	m_R.And8(&m_R.A, m_R.H);
	return;
}
void CZ80MsxDos::op_AND_L()
{
	m_R.And8(&m_R.A, m_R.L);
	return;
}
void CZ80MsxDos::op_AND_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.And8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_AND_A()
{
	m_R.And8(&m_R.A, m_R.A);
	return;
}
void CZ80MsxDos::op_XOR_B()
{
	m_R.Xor8(&m_R.A, m_R.B);
	return;
}
void CZ80MsxDos::op_XOR_C()
{
	m_R.Xor8(&m_R.A, m_R.C);
	return;
}
void CZ80MsxDos::op_XOR_D()
{
	m_R.Xor8(&m_R.A, m_R.D);
	return;
}
void CZ80MsxDos::op_XOR_E()
{
	m_R.Xor8(&m_R.A, m_R.E);
	return;
}
void CZ80MsxDos::op_XOR_H()
{
	m_R.Xor8(&m_R.A, m_R.H);
	return;
}
void CZ80MsxDos::op_XOR_L()
{
	m_R.Xor8(&m_R.A, m_R.L);
	return;
}
void CZ80MsxDos::op_XOR_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Xor8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_XOR_A()
{
	m_R.Xor8(&m_R.A, m_R.A);
	return;
}
void CZ80MsxDos::op_OR_B()
{
	m_R.Or8(&m_R.A, m_R.B);
	return;
}
void CZ80MsxDos::op_OR_C()
{
	m_R.Or8(&m_R.A, m_R.C);
	return;
}
void CZ80MsxDos::op_OR_D()
{
	m_R.Or8(&m_R.A, m_R.D);
	return;
}
void CZ80MsxDos::op_OR_E()
{
	m_R.Or8(&m_R.A, m_R.E);
	return;
}
void CZ80MsxDos::op_OR_H()
{
	m_R.Or8(&m_R.A, m_R.H);
	return;
}
void CZ80MsxDos::op_OR_L()
{
	m_R.Or8(&m_R.A, m_R.L);
	return;
}
void CZ80MsxDos::op_OR_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Or8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_OR_A()
{
	m_R.Or8(&m_R.A, m_R.A);
	return;
}
void CZ80MsxDos::op_CP_B()
{
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, m_R.B);
	return;
}
void CZ80MsxDos::op_CP_C()
{
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, m_R.C);
	return;
}
void CZ80MsxDos::op_CP_D()
{
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, m_R.D);
	return;
}
void CZ80MsxDos::op_CP_E()
{
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, m_R.E);
	return;
}
void CZ80MsxDos::op_CP_H()
{
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, m_R.H);
	return;
}
void CZ80MsxDos::op_CP_L()
{
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, m_R.L);
	return;
}
void CZ80MsxDos::op_CP_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, v);
	return;
}
void CZ80MsxDos::op_CP_A()
{
	uint8_t temp = m_R.A;
	m_R.Sub8(&temp, m_R.A);
	return;
}
void CZ80MsxDos::op_RET_nz()
{
	if( m_R.F.Z == 0 ){
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_POP_BC()
{
	m_R.C = m_pMemSys->Read(m_R.SP++);
	m_R.B = m_pMemSys->Read(m_R.SP++);
	return;
}
void CZ80MsxDos::op_JP_nz_ad()
{
	if( m_R.F.Z == 0 ){
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_JP_ad()
{
	uint16_t temp = m_pMemSys->Read(m_R.PC++);
	temp |= m_pMemSys->Read(m_R.PC) << 8;
	m_R.PC = temp;
	return;
}
void CZ80MsxDos::op_CALL_nz_ad()
{
	if( m_R.F.Z == 0 ){
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_PUSH_BC()
{
	m_pMemSys->Write(--m_R.SP, m_R.B);
	m_pMemSys->Write(--m_R.SP, m_R.C);
	return;
}
void CZ80MsxDos::op_ADD_A_v()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.Add8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_RST_0h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x00;
	return;
}
void CZ80MsxDos::op_RET_z()
{
	if( m_R.F.Z != 0 ){
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_RET()
{
	m_R.PC = m_pMemSys->Read(m_R.SP++);
	m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	return;
}
void CZ80MsxDos::op_JP_z_ad()
{
	if( m_R.F.Z != 0 ){
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_EXTENDED_1()
{
	uint8_t opcd = m_pMemSys->Read(m_R.PC++);
	auto pFunc = OpCode_Extended1[opcd].pFunc;
	(this->*pFunc)();
	return;
}
void CZ80MsxDos::op_CALL_z_ad()
{
	if( m_R.F.Z != 0 ){
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_CALL_ad()
{
	uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
	destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = destAddr;
	return;
}
void CZ80MsxDos::op_ADC_A_v()
{
	uint8_t v= m_pMemSys->Read(m_R.PC++);
	m_R.Add8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_RST_8h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x08;
	return;
}
void CZ80MsxDos::op_RET_nc()
{
	if( m_R.F.C == 0 ){
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_POP_DE()
{
	m_R.E = m_pMemSys->Read(m_R.SP++);
	m_R.D = m_pMemSys->Read(m_R.SP++);
	return;
}
void CZ80MsxDos::op_JP_nc_ad()
{
	if( m_R.F.C == 0 ){
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_OUT_memv_A()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_pIoSys->Out(v, m_R.A);
	return;
}
void CZ80MsxDos::op_CALL_nc_ad()
{
	if( m_R.F.C == 0 ){
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_PUSH_DE()
{
	m_pMemSys->Write(--m_R.SP, m_R.D);
	m_pMemSys->Write(--m_R.SP, m_R.E);
	return;
}
void CZ80MsxDos::op_SUB_v()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.Sub8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_RST_10h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x10;
	return;
}
void CZ80MsxDos::op_RET_c()
{
	if( m_R.F.C != 0 ){
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_EXX()
{
	m_R.Exchange();
	return;
}
void CZ80MsxDos::op_JP_c_ad()
{
	if( m_R.F.C != 0 ){
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_IN_A_memv()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.A = m_pIoSys->In(v);
	return;
}
void CZ80MsxDos::op_CALL_c_ad()
{
	if( m_R.F.C != 0 ){
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_EXTENDED_2IX()
{
	uint8_t opcd = m_pMemSys->Read(m_R.PC++);
	auto pFunc = OpCode_Extended2IX[opcd].pFunc;
	(this->*pFunc)();
	return;
}
void CZ80MsxDos::op_SBC_A_v()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.Sub8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_RST_18h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x18;
	return;
}
void CZ80MsxDos::op_RET_po()
{
	if( m_R.F.PV == 0 ){	// PO
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_POP_HL()
{
	m_R.L = m_pMemSys->Read(m_R.SP++);
	m_R.H = m_pMemSys->Read(m_R.SP++);
	return;
}
void CZ80MsxDos::op_JP_po_ad()
{
	if( m_R.F.PV == 0 ){
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_EX_memSP_HL()
{
	uint8_t rl = m_R.L, rh = m_R.H;
	m_R.L = m_pMemSys->Read(m_R.SP+0);
	m_R.H = m_pMemSys->Read(m_R.SP+1);
	m_pMemSys->Write(m_R.SP+0, rl);
	m_pMemSys->Write(m_R.SP+1, rh);
	return;
}
void CZ80MsxDos::op_CALL_po_ad()
{
	if( m_R.F.PV == 0 ){
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_PUSH_HL()
{
	m_pMemSys->Write(--m_R.SP, m_R.H);
	m_pMemSys->Write(--m_R.SP, m_R.L);
	return;
}
void CZ80MsxDos::op_AND_v()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.And8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_RST_20h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x20;
	return;
}
void CZ80MsxDos::op_RET_pe()
{
	if( m_R.F.PV != 0 ){	// PE
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_JP_memHL()
{
	m_R.PC = m_R.GetHL();	// HLの値そのものがアドレス値である。
	return;
}

void CZ80MsxDos::op_JP_pe_ad()
{
	if( m_R.F.PV != 0 ){ // PE
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_EX_DE_HL()
{
	uint16_t temp = m_R.GetDE();
	m_R.SetDE(m_R.GetHL());
	m_R.SetHL(temp);
	return;
}
void CZ80MsxDos::op_CALL_pe_ad()
{
	if( m_R.F.PV != 0 ){ // PE
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_EXTENDED_3()
{
	uint8_t opcd = m_pMemSys->Read(m_R.PC++);
	auto pFunc = OpCode_Extended3[opcd].pFunc;
	(this->*pFunc)();
	return;
}
void CZ80MsxDos::op_XOR_v()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.Xor8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_RST_28h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x28;
	return;
}
void CZ80MsxDos::op_RET_p()
{
	if( m_R.F.S == 0 ){	// P
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_POP_AF()
{
	m_R.F.Set(m_pMemSys->Read(m_R.SP++));
	m_R.A = m_pMemSys->Read(m_R.SP++);
	return;
}
void CZ80MsxDos::op_JP_p_ad()
{
	if( m_R.F.S == 0 ){ // P
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_DI()
{
	m_bIFF1 = m_bIFF2 = false;
	return;
}
void CZ80MsxDos::op_CALL_p_ad()
{
	if( m_R.F.S == 0 ){ // P
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_PUSH_AF()
{
	m_pMemSys->Write(--m_R.SP, m_R.A);
	m_pMemSys->Write(--m_R.SP, m_R.F.Get());
	return;
}
void CZ80MsxDos::op_OR_v()
{
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.Or8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_RST_30h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x30;
	return;
}
void CZ80MsxDos::op_RET_m()
{
	if( m_R.F.S != 0 ){	// M
		m_R.PC = m_pMemSys->Read(m_R.SP++);
		m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	}
	return;
}
void CZ80MsxDos::op_LD_SP_HL()
{
	m_R.SP = m_R.GetHL();
	return;
}
void CZ80MsxDos::op_JP_m_ad()
{
	if( m_R.F.S != 0 ){ // M
		uint16_t temp = m_pMemSys->Read(m_R.PC++);
		temp |= m_pMemSys->Read(m_R.PC) << 8;
		m_R.PC = temp;
	}
	else{
		m_R.PC += 2;	// nnを読み捨て
	}
	return;
}
void CZ80MsxDos::op_EI()
{
	m_bIFF1 = m_bIFF2 = true;
	return;
}
void CZ80MsxDos::op_CALL_m_ad()
{
	if( m_R.F.S != 0 ){ // M
		uint16_t destAddr = m_pMemSys->Read(m_R.PC++);
		destAddr |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
		m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
		m_R.PC = destAddr;
	}
	else{
		m_R.PC += 2;
	}
	return;
}
void CZ80MsxDos::op_EXTENDED_4IY()
{
	uint8_t opcd = m_pMemSys->Read(m_R.PC++);
	auto pFunc = OpCode_Extended4IY[opcd].pFunc;
	(this->*pFunc)();
	return;
}
void CZ80MsxDos::op_CP_v()
{
	uint8_t tempA = m_R.A;
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_R.Sub8(&tempA, v);
	return;
}
void CZ80MsxDos::op_RST_38h()
{
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>8)&0xff);
	m_pMemSys->Write(--m_R.SP, (m_R.PC>>0)&0xff);
	m_R.PC = 0x38;
	return;
}

// extended1
void CZ80MsxDos::op_RLC_B()
{
	m_R.Rlc8(&m_R.B);
	return;
}
void CZ80MsxDos::op_RLC_C()
{
	m_R.Rlc8(&m_R.C);
	return;
}
void CZ80MsxDos::op_RLC_D()
{
	m_R.Rlc8(&m_R.D);
	return;
}
void CZ80MsxDos::op_RLC_E()
{
	m_R.Rlc8(&m_R.E);
	return;
}
void CZ80MsxDos::op_RLC_H()
{
	m_R.Rlc8(&m_R.H);
	return;
}
void CZ80MsxDos::op_RLC_L()
{
	m_R.Rlc8(&m_R.L);
	return;
}
void CZ80MsxDos::op_RLC_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rlc8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RLC_A()
{
	m_R.Rlc8(&m_R.A);
	return;
}
void CZ80MsxDos::op_RRC_B()
{
	m_R.Rrc8(&m_R.B);
	return;
}
void CZ80MsxDos::op_RRC_C()
{
	m_R.Rrc8(&m_R.C);
	return;
}
void CZ80MsxDos::op_RRC_D()
{
	m_R.Rrc8(&m_R.D);
	return;
}
void CZ80MsxDos::op_RRC_E()
{
	m_R.Rrc8(&m_R.E);
	return;
}
void CZ80MsxDos::op_RRC_H()
{
	m_R.Rrc8(&m_R.H);
	return;
}
void CZ80MsxDos::op_RRC_L()
{
	m_R.Rrc8(&m_R.L);
	return;
}
void CZ80MsxDos::op_RRC_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rrc8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RRC_A()
{
	m_R.Rrc8(&m_R.A);
	return;
}
void CZ80MsxDos::op_RL_B()
{
	m_R.Rl8(&m_R.B);
	return;
}
void CZ80MsxDos::op_RL_C()
{
	m_R.Rl8(&m_R.C);
	return;
}
void CZ80MsxDos::op_RL_D()
{
	m_R.Rl8(&m_R.D);
	return;
}
void CZ80MsxDos::op_RL_E()
{
	m_R.Rl8(&m_R.E);
	return;
}
void CZ80MsxDos::op_RL_H()
{
	m_R.Rl8(&m_R.H);
	return;
}
void CZ80MsxDos::op_RL_L()
{
	m_R.Rl8(&m_R.L);
	return;
}
void CZ80MsxDos::op_RL_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rl8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RL_A()
{
	m_R.Rl8(&m_R.A);
	return;
}
void CZ80MsxDos::op_RR_B()
{
	m_R.Rr8(&m_R.B);
	return;
}
void CZ80MsxDos::op_RR_C()
{
	m_R.Rr8(&m_R.C);
	return;
}
void CZ80MsxDos::op_RR_D()
{
	m_R.Rr8(&m_R.D);
	return;
}
void CZ80MsxDos::op_RR_E()
{
	m_R.Rr8(&m_R.E);
	return;
}
void CZ80MsxDos::op_RR_H()
{
	m_R.Rr8(&m_R.H);
	return;
}
void CZ80MsxDos::op_RR_L()
{
	m_R.Rr8(&m_R.L);
	return;
}
void CZ80MsxDos::op_RR_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rr8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RR_A()
{
	m_R.Rr8(&m_R.A);
	return;
}
void CZ80MsxDos::op_SLA_B()
{
	m_R.Sla8(&m_R.B);
	return;
}
void CZ80MsxDos::op_SLA_C()
{
	m_R.Sla8(&m_R.C);
	return;
}
void CZ80MsxDos::op_SLA_D()
{
	m_R.Sla8(&m_R.D);
	return;
}
void CZ80MsxDos::op_SLA_E()
{
	m_R.Sla8(&m_R.E);
	return;
}
void CZ80MsxDos::op_SLA_H()
{
	m_R.Sla8(&m_R.H);
	return;
}
void CZ80MsxDos::op_SLA_L()
{
	m_R.Sla8(&m_R.L);
	return;
}
void CZ80MsxDos::op_SLA_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sla8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SLA_A()
{
	m_R.Sla8(&m_R.A);
	return;
}
void CZ80MsxDos::op_SRA_B()
{
	m_R.Sra8(&m_R.B);
	return;
}
void CZ80MsxDos::op_SRA_C()
{
	m_R.Sra8(&m_R.C);
	return;
}
void CZ80MsxDos::op_SRA_D()
{
	m_R.Sra8(&m_R.D);
	return;
}
void CZ80MsxDos::op_SRA_E()
{
	m_R.Sra8(&m_R.E);
	return;
}
void CZ80MsxDos::op_SRA_H()
{
	m_R.Sra8(&m_R.H);
	return;
}
void CZ80MsxDos::op_SRA_L()
{
	m_R.Sra8(&m_R.L);
	return;
}
void CZ80MsxDos::op_SRA_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sra8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SRA_A()
{
	m_R.Sra8(&m_R.L);
	return;
}
void CZ80MsxDos::op_SLL_B()	// undoc.Z80
{
	m_R.Sll8(&m_R.B);
	return;
}
void CZ80MsxDos::op_SLL_C()	// undoc.Z80
{
	m_R.Sll8(&m_R.C);
	return;
}
void CZ80MsxDos::op_SLL_D()	// undoc.Z80
{
	m_R.Sll8(&m_R.D);
	return;
}
void CZ80MsxDos::op_SLL_E()	// undoc.Z80
{
	m_R.Sll8(&m_R.E);
	return;
}
void CZ80MsxDos::op_SLL_H()	// undoc.Z80
{
	m_R.Sll8(&m_R.H);
	return;
}
void CZ80MsxDos::op_SLL_L()	// undoc.Z80
{
	m_R.Sll8(&m_R.L);
	return;
}
void CZ80MsxDos::op_SLL_memHL()	// undoc.Z80
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sll8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SLL_A()	// undoc.Z80
{
	m_R.Sll8(&m_R.A);
	return;
}
void CZ80MsxDos::op_SRL_B()
{
	m_R.Srl8(&m_R.B);
	return;
}
void CZ80MsxDos::op_SRL_C()
{
	m_R.Srl8(&m_R.C);
	return;
}
void CZ80MsxDos::op_SRL_D()
{
	m_R.Srl8(&m_R.D);
	return;
}
void CZ80MsxDos::op_SRL_E()
{
	m_R.Srl8(&m_R.E);
	return;
}
void CZ80MsxDos::op_SRL_H()
{
	m_R.Srl8(&m_R.H);
	return;
}
void CZ80MsxDos::op_SRL_L()
{
	m_R.Srl8(&m_R.L);
	return;
}
void CZ80MsxDos::op_SRL_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Srl8(&v);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SRL_A()
{
	m_R.Srl8(&m_R.A);
	return;
}
void CZ80MsxDos::op_BIT_0_B()
{
	m_R.F.Z = (m_R.B & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_0_C()
{
	m_R.F.Z = (m_R.C & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_0_D()
{
	m_R.F.Z = (m_R.D & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_0_E()
{
	m_R.F.Z = (m_R.E & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_0_H()
{
	m_R.F.Z = (m_R.H & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_0_L()
{
	m_R.F.Z = (m_R.L & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_0_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = (v & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_0_A()
{
	m_R.F.Z = (m_R.A & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_B()
{
	m_R.F.Z = ((m_R.B>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_C()
{
	m_R.F.Z = ((m_R.C>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_D()
{
	m_R.F.Z = ((m_R.D>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_E()
{
	m_R.F.Z = ((m_R.E>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_H()
{
	m_R.F.Z = ((m_R.H>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_L()
{
	m_R.F.Z = ((m_R.L>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_A()
{
	m_R.F.Z = ((m_R.A>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_B()
{
	m_R.F.Z = ((m_R.B>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_C()
{
	m_R.F.Z = ((m_R.C>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_D()
{
	m_R.F.Z = ((m_R.D>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_E()
{
	m_R.F.Z = ((m_R.E>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_H()
{
	m_R.F.Z = ((m_R.H>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_L()
{
	m_R.F.Z = ((m_R.L>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_A()
{
	m_R.F.Z = ((m_R.A>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_B()
{
	m_R.F.Z = ((m_R.B>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_C()
{
	m_R.F.Z = ((m_R.C>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_D()
{
	m_R.F.Z = ((m_R.D>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_E()
{
	m_R.F.Z = ((m_R.E>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_H()
{
	m_R.F.Z = ((m_R.H>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_L()
{
	m_R.F.Z = ((m_R.L>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_A()
{
	m_R.F.Z = ((m_R.A>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_B()
{
	m_R.F.Z = ((m_R.B>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_C()
{
	m_R.F.Z = ((m_R.C>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_D()
{
	m_R.F.Z = ((m_R.D>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_E()
{
	m_R.F.Z = ((m_R.E>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_H()
{
	m_R.F.Z = ((m_R.H>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_L()
{
	m_R.F.Z = ((m_R.L>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_A()
{
	m_R.F.Z = ((m_R.A>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_B()
{
	m_R.F.Z = ((m_R.B>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_C()
{
	m_R.F.Z = ((m_R.C>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_D()
{
	m_R.F.Z = ((m_R.D>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_E()
{
	m_R.F.Z = ((m_R.E>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_H()
{
	m_R.F.Z = ((m_R.H>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_L()
{
	m_R.F.Z = ((m_R.L>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_A()
{
	m_R.F.Z = ((m_R.A>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_B()
{
	m_R.F.Z = ((m_R.B>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_C()
{
	m_R.F.Z = ((m_R.C>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_D()
{
	m_R.F.Z = ((m_R.D>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_E()
{
	m_R.F.Z = ((m_R.E>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_H()
{
	m_R.F.Z = ((m_R.H>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_L()
{
	m_R.F.Z = ((m_R.L>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_A()
{
	m_R.F.Z = ((m_R.A>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_B()
{
	m_R.F.Z = ((m_R.B>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_C()
{
	m_R.F.Z = ((m_R.C>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_D()
{
	m_R.F.Z = ((m_R.D>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_E()
{
	m_R.F.Z = ((m_R.E>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_H()
{
	m_R.F.Z = ((m_R.H>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_L()
{
	m_R.F.Z = ((m_R.L>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_A()
{
	m_R.F.Z = ((m_R.A>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_RES_0_B()
{
	m_R.B &= ((0x01 << 0) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_0_C()
{
	m_R.C &= ((0x01 << 0) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_0_D()
{
	m_R.D &= ((0x01 << 0) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_0_E()
{
	m_R.E &= ((0x01 << 0) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_0_H()
{
	m_R.H &= ((0x01 << 0) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_0_L()
{
	m_R.L &= ((0x01 << 0) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_0_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 0) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_0_A()
{
	m_R.A &= ((0x01 << 0) ^ 0xFF);
	return;
}

void CZ80MsxDos::op_RES_1_B()
{
	m_R.B &= ((0x01 << 1) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_1_C()
{
	m_R.C &= ((0x01 << 1) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_1_D()
{
	m_R.D &= ((0x01 << 1) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_1_E()
{
	m_R.E &= ((0x01 << 1) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_1_H()
{
	m_R.H &= ((0x01 << 1) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_1_L()
{
	m_R.L &= ((0x01 << 1) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_1_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 1) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_1_A()
{
	m_R.A &= ((0x01 << 1) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_2_B()
{
	m_R.B &= ((0x01 << 2) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_2_C()
{
	m_R.C &= ((0x01 << 2) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_2_D()
{
	m_R.D &= ((0x01 << 2) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_2_E()
{
	m_R.E &= ((0x01 << 2) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_2_H()
{
	m_R.H &= ((0x01 << 2) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_2_L()
{
	m_R.L &= ((0x01 << 2) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_2_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 2) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_2_A()
{
	m_R.A &= ((0x01 << 2) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_3_B()
{
	m_R.B &= ((0x01 << 3) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_3_C()
{
	m_R.C &= ((0x01 << 3) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_3_D()
{
	m_R.D &= ((0x01 << 3) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_3_E()
{
	m_R.E &= ((0x01 << 3) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_3_H()
{
	m_R.H &= ((0x01 << 3) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_3_L()
{
	m_R.L &= ((0x01 << 3) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_3_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 3) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_3_A()
{
	m_R.A &= ((0x01 << 3) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_4_B()
{
	m_R.B &= ((0x01 << 4) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_4_C()
{
	m_R.C &= ((0x01 << 4) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_4_D()
{
	m_R.D &= ((0x01 << 4) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_4_E()
{
	m_R.E &= ((0x01 << 4) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_4_H()
{
	m_R.H &= ((0x01 << 4) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_4_L()
{
	m_R.L &= ((0x01 << 4) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_4_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 4) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_4_A()
{
	m_R.A &= ((0x01 << 4) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_5_B()
{
	m_R.B &= ((0x01 << 5) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_5_C()
{
	m_R.C &= ((0x01 << 5) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_5_D()
{
	m_R.D &= ((0x01 << 5) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_5_E()
{
	m_R.E &= ((0x01 << 5) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_5_H()
{
	m_R.H &= ((0x01 << 5) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_5_L()
{
	m_R.L &= ((0x01 << 5) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_5_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 5) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_5_A()
{
	m_R.A &= ((0x01 << 5) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_6_B()
{
	m_R.B &= ((0x01 << 6) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_6_C()
{
	m_R.C &= ((0x01 << 6) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_6_D()
{
	m_R.D &= ((0x01 << 6) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_6_E()
{
	m_R.E &= ((0x01 << 6) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_6_H()
{
	m_R.H &= ((0x01 << 6) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_6_L()
{
	m_R.L &= ((0x01 << 6) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_6_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 6) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_6_A()
{
	m_R.A &= ((0x01 << 6) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_7_B()
{
	m_R.B &= ((0x01 << 7) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_7_C()
{
	m_R.C &= ((0x01 << 7) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_7_D()
{
	m_R.D &= ((0x01 << 7) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_7_E()
{
	m_R.E &= ((0x01 << 7) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_7_H()
{
	m_R.H &= ((0x01 << 7) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_7_L()
{
	m_R.L &= ((0x01 << 7) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_RES_7_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 7) ^ 0xFF);
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_RES_7_A()
{
	m_R.A &= ((0x01 << 7) ^ 0xFF);
	return;
}
void CZ80MsxDos::op_SET_0_B()
{
	m_R.B |= 0x01 << 0;
	return;
}
void CZ80MsxDos::op_SET_0_C()
{
	m_R.C |= 0x01 << 0;
	return;
}
void CZ80MsxDos::op_SET_0_D()
{
	m_R.D |= 0x01 << 0;
	return;
}
void CZ80MsxDos::op_SET_0_E()
{
	m_R.E |= 0x01 << 0;
	return;
}
void CZ80MsxDos::op_SET_0_H()
{
	m_R.H |= 0x01 << 0;
	return;
}
void CZ80MsxDos::op_SET_0_L()
{
	m_R.L |= 0x01 << 0;
	return;
}
void CZ80MsxDos::op_SET_0_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 0;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_0_A()
{
	m_R.A |= 0x01 << 0;
	return;
}
void CZ80MsxDos::op_SET_1_B()
{
	m_R.B |= 0x01 << 1;
	return;
}
void CZ80MsxDos::op_SET_1_C()
{
	m_R.C |= 0x01 << 1;
	return;
}
void CZ80MsxDos::op_SET_1_D()
{
	m_R.D |= 0x01 << 1;
	return;
}
void CZ80MsxDos::op_SET_1_E()
{
	m_R.E |= 0x01 << 1;
	return;
}
void CZ80MsxDos::op_SET_1_H()
{
	m_R.H |= 0x01 << 1;
	return;
}
void CZ80MsxDos::op_SET_1_L()
{
	m_R.L |= 0x01 << 1;
	return;
}
void CZ80MsxDos::op_SET_1_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 1;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_1_A()
{
	m_R.A |= 0x01 << 1;
	return;
}
void CZ80MsxDos::op_SET_2_B()
{
	m_R.B |= 0x01 << 2;
	return;
}
void CZ80MsxDos::op_SET_2_C()
{
	m_R.C |= 0x01 << 2;
	return;
}
void CZ80MsxDos::op_SET_2_D()
{
	m_R.D |= 0x01 << 2;
	return;
}
void CZ80MsxDos::op_SET_2_E()
{
	m_R.E |= 0x01 << 2;
	return;
}
void CZ80MsxDos::op_SET_2_H()
{
	m_R.H |= 0x01 << 2;
	return;
}
void CZ80MsxDos::op_SET_2_L()
{
	m_R.L |= 0x01 << 2;
	return;
}
void CZ80MsxDos::op_SET_2_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 2;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_2_A()
{
	m_R.A |= 0x01 << 2;
	return;
}
void CZ80MsxDos::op_SET_3_B()
{
	m_R.B |= 0x01 << 3;
	return;
}
void CZ80MsxDos::op_SET_3_C()
{
	m_R.C |= 0x01 << 3;
	return;
}
void CZ80MsxDos::op_SET_3_D()
{
	m_R.D |= 0x01 << 3;
	return;
}
void CZ80MsxDos::op_SET_3_E()
{
	m_R.E |= 0x01 << 3;
	return;
}
void CZ80MsxDos::op_SET_3_H()
{
	m_R.H |= 0x01 << 3;
	return;
}
void CZ80MsxDos::op_SET_3_L()
{
	m_R.L |= 0x01 << 3;
	return;
}
void CZ80MsxDos::op_SET_3_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 3;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_3_A()
{
	m_R.A |= 0x01 << 3;
	return;
}
void CZ80MsxDos::op_SET_4_B()
{
	m_R.B |= 0x01 << 4;
	return;
}
void CZ80MsxDos::op_SET_4_C()
{
	m_R.C |= 0x01 << 4;
	return;
}
void CZ80MsxDos::op_SET_4_D()
{
	m_R.D |= 0x01 << 4;
	return;
}
void CZ80MsxDos::op_SET_4_E()
{
	m_R.E |= 0x01 << 4;
	return;
}
void CZ80MsxDos::op_SET_4_H()
{
	m_R.H |= 0x01 << 4;
	return;
}
void CZ80MsxDos::op_SET_4_L()
{
	m_R.L |= 0x01 << 4;
	return;
}
void CZ80MsxDos::op_SET_4_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 4;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_4_A()
{
	m_R.A |= 0x01 << 4;
	return;
}
void CZ80MsxDos::op_SET_5_B()
{
	m_R.B |= 0x01 << 5;
	return;
}
void CZ80MsxDos::op_SET_5_C()
{
	m_R.C |= 0x01 << 5;
	return;
}
void CZ80MsxDos::op_SET_5_D()
{
	m_R.D |= 0x01 << 5;
	return;
}
void CZ80MsxDos::op_SET_5_E()
{
	m_R.E |= 0x01 << 5;
	return;
}
void CZ80MsxDos::op_SET_5_H()
{
	m_R.H |= 0x01 << 5;
	return;
}
void CZ80MsxDos::op_SET_5_L()
{
	m_R.L |= 0x01 << 5;
	return;
}
void CZ80MsxDos::op_SET_5_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 5;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_5_A()
{
	m_R.A |= 0x01 << 5;
	return;
}
void CZ80MsxDos::op_SET_6_B()
{
	m_R.B |= 0x01 << 6;
	return;
}
void CZ80MsxDos::op_SET_6_C()
{
	m_R.C |= 0x01 << 6;
	return;
}
void CZ80MsxDos::op_SET_6_D()
{
	m_R.D |= 0x01 << 6;
	return;
}
void CZ80MsxDos::op_SET_6_E()
{
	m_R.E |= 0x01 << 6;
	return;
}
void CZ80MsxDos::op_SET_6_H()
{
	m_R.H |= 0x01 << 6;
	return;
}
void CZ80MsxDos::op_SET_6_L()
{
	m_R.L |= 0x01 << 6;
	return;
}
void CZ80MsxDos::op_SET_6_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 6;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_6_A()
{
	m_R.A |= 0x01 << 6;
	return;
}
void CZ80MsxDos::op_SET_7_B()
{
	m_R.B |= 0x01 << 7;
	return;
}
void CZ80MsxDos::op_SET_7_C()
{
	m_R.C |= 0x01 << 7;
	return;
}
void CZ80MsxDos::op_SET_7_D()
{
	m_R.D |= 0x01 << 7;
	return;
}
void CZ80MsxDos::op_SET_7_E()
{
	m_R.E |= 0x01 << 7;
	return;
}
void CZ80MsxDos::op_SET_7_H()
{
	m_R.H |= 0x01 << 7;
	return;
}
void CZ80MsxDos::op_SET_7_L()
{
	m_R.L |= 0x01 << 7;
	return;
}
void CZ80MsxDos::op_SET_7_memHL()
{
	uint16_t ad = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 7;
	m_pMemSys->Write(m_R.GetHL(), v);
	return;
}
void CZ80MsxDos::op_SET_7_A()
{
	m_R.A |= 0x01 << 7;
	return;
}

// extended2 for IX
void CZ80MsxDos::op_ADD_IX_BC()
{
	m_R.Add16(&m_R.IX, m_R.GetBC());
	return;
}
void CZ80MsxDos::op_ADD_IX_DE()
{
	m_R.Add16(&m_R.IX, m_R.GetDE());
	return;
}
void CZ80MsxDos::op_LD_IX_ad()
{
	m_R.IX = m_pMemSys->Read(m_R.PC++);
	m_R.IX |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	return;
}
void CZ80MsxDos::op_LD_memAD_IX()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_pMemSys->Write(ad+0, (m_R.IX>>0)&0xff);
	m_pMemSys->Write(ad+1, (m_R.IX>>8)&0xff);
	return;
}
void CZ80MsxDos::op_INC_IX()
{
	++m_R.IX;
	return;
}
void CZ80MsxDos::op_ADD_IX_IX()
{
	m_R.Add16(&m_R.IX, m_R.IX);
	return;
}
void CZ80MsxDos::op_LD_IX_memAD()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_R.IX = m_pMemSys->Read(ad + 0);
	m_R.IX |= static_cast<uint16_t>(m_pMemSys->Read(ad + 1)) << 8;
	return;
}
void CZ80MsxDos::op_DEC_IX()
{
	--m_R.IX;
	return;
}
void CZ80MsxDos::op_INC_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Inc8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_DEC_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Dec8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_v()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_ADD_IX_SP()
{
	m_R.Add16(&m_R.IX, m_R.SP);
	return;
}
void CZ80MsxDos::op_LD_B_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_R.B = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_C_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_R.C = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_D_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_R.D = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_E_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_R.E = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_H_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_R.H = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_L_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_R.L = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_B()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.B);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_C()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.C);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_D()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.D);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_E()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.E);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_H()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.H);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_L()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.L);
	return;
}
void CZ80MsxDos::op_LD_memIXpV_A()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.A);
	return;
}
void CZ80MsxDos::op_LD_A_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	m_R.A = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_ADD_A_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Add8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_ADC_A_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Add8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SUB_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sub8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_SBC_A_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sub8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_AND_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.And8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_XOR_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Xor8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_OR_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Or8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_CP_memIXpV()
{
	uint16_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	uint8_t tempA = m_R.A;
	m_R.Sub8(&tempA, v);
	return;
}
void CZ80MsxDos::op_EXTENDED_2IX2()
{
	// このマシンコードは、DDh+CBh+nn+vv の形式になっていて、
	// データ部nnがコードの途中に位置していることに注意する
	// このメソッドが呼ばれた時点で、DDh+CBhまではデコードされているからPCの位置はnnを示している。
	// 子メソッドを呼び出す時はこの位置を維持する。子メソッドから戻ったら06hの分のPC++を行う。
	uint8_t vv = m_pMemSys->Read(m_R.PC+1);
	auto pFunc = OpCode_Extended2IX2[vv].pFunc;
	(this->*pFunc)();
	m_R.PC++;
	return;
}
void CZ80MsxDos::op_POP_IX()
{
	m_R.IX = m_pMemSys->Read(m_R.SP++);
	m_R.IX |= static_cast<uint16_t>(m_pMemSys->Read(m_R.SP++)) << 8;
	return;
}
void CZ80MsxDos::op_EX_memSP_IX()
{
	uint8_t ixl = (m_R.IX >> 0) & 0xFF;
	uint8_t ixh = (m_R.IX >> 8) & 0xFF;
	m_R.IX = m_pMemSys->Read(m_R.SP+0);
	m_R.IX |= static_cast<uint16_t>(m_pMemSys->Read(m_R.SP+1)) < 8;;
	m_pMemSys->Write(m_R.SP+0, ixl);
	m_pMemSys->Write(m_R.SP+1, ixh);
	return;
}
void CZ80MsxDos::op_PUSH_IX()
{
	m_pMemSys->Write(--m_R.SP, (m_R.IX>>8)&0xFF);
	m_pMemSys->Write(--m_R.SP, (m_R.IX>>0)&0xFF);
	return;
}
void CZ80MsxDos::op_JP_memIX()
{
	m_R.PC = m_R.IX;	// IXの値そのものがアドレス値である
	return;
}
void CZ80MsxDos::op_LD_SP_IX()
{
	m_R.SP = m_R.IX;
	return;
}

// extended2 for IX - 2
void CZ80MsxDos::op_RLC_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rlc8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RRC_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rrc8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RL_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rl8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RR_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rr8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SLA_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sla8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SRA_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sra8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SRL_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Srl8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_BIT_0_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = (v & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_RES_0_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 0) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_1_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 1) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_2_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 2) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_3_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 3) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_4_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 4) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_5_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 5) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_6_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 6) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_7_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 7) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_0_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 0;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_1_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 1;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_2_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 2;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_3_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 3;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_4_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 4;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_5_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 5;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_6_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 6;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_7_memVpIX()
{
	z80memaddr_t ad = m_R.IX + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 7;
	m_pMemSys->Write(ad, v);
	return;
}

// extended3 - for debug
void CZ80MsxDos::op_DEBUGBREAK()
{
	DEBUG_BREAK;
	return;

}

// extended3
void CZ80MsxDos::op_IN_B_memC()
{
	// ※Bレジスタによる16bitアドレスは考慮しない。
	m_R.B = m_pIoSys->In(m_R.C);
	m_R.SetFlagByIN(m_R.B);
	return;
}
void CZ80MsxDos::op_OUT_memC_B()
{
	m_pIoSys->Out(m_R.C, m_R.B);
	return;
}
void CZ80MsxDos::op_SBC_HL_BC()
{
	uint16_t v = m_R.GetHL();
	m_R.Sub16Cy(&v, m_R.GetBC(), m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_LD_memAD_BC()
{
	z80memaddr_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	m_pMemSys->Write(ad+0, m_R.C);
	m_pMemSys->Write(ad+1, m_R.B);
	return;
}
void CZ80MsxDos::op_NEG()
{
	if( m_R.A == 0 )
	{
		m_R.F.C = 0;
		m_R.F.Z = 1;
		m_R.F.PV= 0;
		m_R.F.S = 0;
		m_R.F.N = 1;
		m_R.F.H = 0;
	}
	else{
		m_R.A = (0 - m_R.A) & 0xFF;
		//
		m_R.F.C = 1;
		m_R.F.Z = 0;
		m_R.F.PV= 0;
		m_R.F.S = (m_R.A>>7)&0x1;
		m_R.F.N = 1;
		m_R.F.H = 1;
	}
	return;
}
void CZ80MsxDos::op_RETN()
{
	m_R.PC = m_pMemSys->Read(m_R.SP++);
	m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	m_bIFF1 = m_bIFF2;
	return;
}
void CZ80MsxDos::op_IM_0()
{
	m_IM = INTERRUPTMODE0;
	return;
}
void CZ80MsxDos::op_LD_i_A()
{
	m_R.I = m_R.A;
	return;
}
void CZ80MsxDos::op_IN_C_memC()
{
	m_R.C = m_pIoSys->In(m_R.C);
	m_R.SetFlagByIN(m_R.C);
	return;
}
void CZ80MsxDos::op_OUT_memC_C()
{
	m_pIoSys->Out(m_R.C, m_R.C);
	return;
}
void CZ80MsxDos::op_ADC_HL_BC()
{
	uint16_t v = m_R.GetHL();
	m_R.Add16Cy(&v, m_R.GetBC(), m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_LD_BC_memAD()
{
	z80memaddr_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	m_R.C = m_pMemSys->Read(ad+0);
	m_R.B = m_pMemSys->Read(ad+1);
	return;
}
void CZ80MsxDos::op_RETI()
{
	m_R.PC = m_pMemSys->Read(m_R.SP++);
	m_R.PC |= m_pMemSys->Read(m_R.SP++) << 8;
	m_bIFF1 = m_bIFF2;
	return;
}
void CZ80MsxDos::op_LD_R_A()
{
	m_R.R = m_R.A;
	return;
}
void CZ80MsxDos::op_IN_D_memC()
{
	m_R.D = m_pIoSys->In(m_R.C);
	m_R.SetFlagByIN(m_R.D);
	return;
}
void CZ80MsxDos::op_OUT_memC_D()
{
	m_pIoSys->Out(m_R.C, m_R.D);
	return;
}
void CZ80MsxDos::op_SBC_HL_DE()
{
	uint16_t v = m_R.GetHL();
	m_R.Sub16Cy(&v, m_R.GetDE(), m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_LD_memAD_DE()
{
	z80memaddr_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	m_pMemSys->Write(ad+0, m_R.E);
	m_pMemSys->Write(ad+1, m_R.D);
	return;
}
void CZ80MsxDos::op_IM_1()
{
	m_IM = INTERRUPTMODE1;
	return;
}
void CZ80MsxDos::op_LD_A_i()
{
	m_R.A = m_R.I;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.A==0)?1:0;
	m_R.F.PV = (m_bIFF2)?1:0;
	m_R.F.S = (m_R.A>>7)&0x01;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_IN_E_memC()
{
	m_R.E = m_pIoSys->In(m_R.C);
	m_R.SetFlagByIN(m_R.E);
	return;
}
void CZ80MsxDos::op_OUT_memC_E()
{
	m_pIoSys->Out(m_R.C, m_R.E);
	return;
}
void CZ80MsxDos::op_ADC_HL_DE()
{
	uint16_t v = m_R.GetHL();
	m_R.Add16Cy(&v, m_R.GetDE(), m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_LD_DE_memAD()
{
	z80memaddr_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	m_R.E = m_pMemSys->Read(ad+0);
	m_R.D = m_pMemSys->Read(ad+1);
	return;
}
void CZ80MsxDos::op_IM_2()
{
	m_IM = INTERRUPTMODE2;
	return;
}
void CZ80MsxDos::op_LD_A_R()
{
	m_R.A = m_R.R;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.A==0)?1:0;
	m_R.F.PV = (m_bIFF2)?1:0;
	m_R.F.S = (m_R.A>>7)&0x01;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_IN_H_memC()
{
	m_R.H = m_pIoSys->In(m_R.C);
	m_R.SetFlagByIN(m_R.H);
	return;
}
void CZ80MsxDos::op_OUT_memC_H()
{
	m_pIoSys->Out(m_R.C, m_R.H);
	return;
}
void CZ80MsxDos::op_SBC_HL_HL()
{
	uint16_t v = m_R.GetHL();
	m_R.Sub16Cy(&v, v, m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_RRD()
{
	uint8_t mem = m_pMemSys->Read(m_R.GetHL());
	uint8_t temp = mem&0x0F;
	mem >>= 4;
	mem |= (m_R.A<<4) & 0xF0;
	m_R.A = (m_R.A&0xF0) | temp;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.A==0)?1:0;
	m_R.F.PV = m_R.CheckParytyEven(m_R.A);
	m_R.F.S = (m_R.A>>7)&0x01;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_IN_L_memC()
{
	m_R.L = m_pIoSys->In(m_R.C);
	m_R.SetFlagByIN(m_R.L);
	return;
}
void CZ80MsxDos::op_OUT_memC_L()
{
	m_pIoSys->Out(m_R.C, m_R.L);
	return;
}
void CZ80MsxDos::op_ADC_HL_HL()
{
	uint16_t v = m_R.GetHL();
	m_R.Add16Cy(&v, v, m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_RLD()
{
	uint8_t mem = m_pMemSys->Read(m_R.GetHL());
	uint8_t temp = (mem>>4)&0x0F;
	mem |= (mem<<4) | (m_R.A&0x0F);
	m_R.A = (m_R.A&0xF0) | temp;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.A==0)?1:0;
	m_R.F.PV = m_R.CheckParytyEven(m_R.A);
	m_R.F.S = (m_R.A>>7)&0x01;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
//void CZ80MsxDos::op_IN_memC()
// {
// 	assert(false);
// }
void CZ80MsxDos::op_SBC_HL_SP()
{
	uint16_t v = m_R.GetHL();
	m_R.Sub16Cy(&v, m_R.SP, m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_LD_memAD_SP()
{
	z80memaddr_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	m_pMemSys->Write(ad+0, (m_R.SP>>0)&0xff);
	m_pMemSys->Write(ad+1, (m_R.SP>>8)&0xff);
	return;
}
void CZ80MsxDos::op_IN_A_memC()
{
	m_R.A = m_pIoSys->In(m_R.C);
	m_R.SetFlagByIN(m_R.A);
	return;
}
void CZ80MsxDos::op_OUT_memC_A()
{
	m_pIoSys->Out(m_R.C, m_R.A);
	return;
}
void CZ80MsxDos::op_ADC_HL_SP()
{
	uint16_t v = m_R.GetHL();
	m_R.Add16Cy(&v, m_R.SP, m_R.F.C);
	m_R.SetHL(v);
	return;
}
void CZ80MsxDos::op_LD_SP_memAD()
{
	z80memaddr_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	m_R.SP = m_pMemSys->Read(ad+0);
	m_R.SP |= static_cast<uint16_t>(m_pMemSys->Read(ad+1)) << 8;
	return;
}
void CZ80MsxDos::op_LDI()
{
	uint16_t hl = m_R.GetHL();
	uint16_t de = m_R.GetDE();
	uint16_t bc = m_R.GetBC();
	uint8_t v = m_pMemSys->Read(hl);
	m_pMemSys->Write(de, v);
	m_R.SetHL(hl+1);
	m_R.SetDE(de+1);
	m_R.SetBC(--bc);
	m_R.F.PV = (bc==0)?0:1;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_CPI()
{
	uint8_t a = m_R.A;
	uint16_t hl = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(hl);
	uint16_t bc = m_R.GetBC();
	a -= v;
	m_R.SetHL(++hl);
	m_R.SetBC(--bc);
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (a==0)?1:0;
	m_R.F.PV = (bc==0)?0:1;
	m_R.F.S = (a>>7)&0x01;
	m_R.F.N = 1;
	m_R.H = ((m_R.A&0x0F)<(v&0x0F))?1:0; // ビット4への桁借りが生じたか
	return;
}
void CZ80MsxDos::op_INI()
{
	uint16_t hl = m_R.GetHL();
	uint8_t v = m_pIoSys->In(m_R.C);
	m_pMemSys->Write(hl, v);
	m_R.SetHL(hl+1);
	m_R.B--;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.B==0)?1:0;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_OUTI()
{
	uint16_t hl = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(hl);
	m_pIoSys->Out(m_R.C, v);
	m_R.SetHL(hl+1);
	m_R.B--;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.B==0)?1:0;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_LDD()
{
	uint16_t hl = m_R.GetHL();
	uint16_t de = m_R.GetDE();
	uint16_t bc = m_R.GetBC();
	uint8_t v = m_pMemSys->Read(hl);
	m_pMemSys->Write(de, v);
	m_R.SetHL(hl-1);
	m_R.SetDE(de-1);
	m_R.SetBC(--bc);
	m_R.F.PV = (bc==0)?0:1;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_CPD()
{
	uint8_t a = m_R.A;
	uint16_t hl = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(hl);
	uint16_t bc = m_R.GetBC();
	a -= v;
	m_R.SetHL(--hl);
	m_R.SetBC(--bc);
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (a==0)?1:0;
	m_R.F.PV = (bc==0)?0:1;
	m_R.F.S = (a>>7)&0x01;
	m_R.F.N = 1;
	m_R.H = ((m_R.A&0x0F)<(v&0x0F))?1:0; // ビット4への桁借りが生じたか
	return;
}
void CZ80MsxDos::op_IND()
{
	uint16_t hl = m_R.GetHL();
	uint8_t v = m_pIoSys->In(m_R.C);
	m_pMemSys->Write(hl, v);
	m_R.SetHL(hl-1);
	m_R.B--;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.B==0)?1:0;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_OUTD()
{
	uint16_t hl = m_R.GetHL();
	uint8_t v = m_pMemSys->Read(hl);
	m_pIoSys->Out(m_R.C, v);
	m_R.SetHL(hl-1);
	m_R.B--;
	//
	m_R.F.C = m_R.F.C;
	m_R.F.Z = (m_R.B==0)?1:0;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_LDIR()
{
	uint16_t hl = m_R.GetHL();
	uint16_t de = m_R.GetDE();
	uint16_t bc = m_R.GetBC();
	do{
		uint8_t v = m_pMemSys->Read(hl);
		m_pMemSys->Write(de, v);
		++hl, ++de, --bc;
	} while(bc != 0);
	m_R.SetHL(hl);
	m_R.SetDE(de);
	m_R.SetBC(bc);
	m_R.F.PV = 0;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_CPIR()
{
	uint8_t a = m_R.A;
	uint16_t hl = m_R.GetHL();
	uint16_t bc = m_R.GetBC();
	do{
		uint8_t v = m_pMemSys->Read(hl);
		++hl, --bc;
		if(v == a ){
			m_R.SetHL(hl);
			m_R.SetBC(bc);
			m_R.F.Z = 1;
			m_R.F.PV = 1;
			m_R.F.S = 0;
			m_R.F.N = 1;
			m_R.F.H = 0;
			return;
		}
	}while(bc!=0);
	//
	m_R.SetHL(hl);
	m_R.SetBC(bc);
	//
	m_R.F.Z = 0;
	m_R.F.PV = 0;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_INIR()
{
	uint16_t hl = m_R.GetHL();
	do{
		uint8_t v = m_pIoSys->In(m_R.C);
		m_pMemSys->Write(hl, v);
		++hl,m_R.B--;
	}while(m_R.B!=0);
	//
	m_R.SetHL(hl);
	m_R.F.Z = 1;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_OTIR()
{
	uint16_t hl = m_R.GetHL();
	do{
		uint8_t v = m_pMemSys->Read(hl);
		m_pIoSys->Out(m_R.C, v);
		++hl, m_R.B--;
	}while(m_R.B!=0);
	m_R.SetHL(hl);
	m_R.F.Z = 1;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_LDDR()
{
	uint16_t hl = m_R.GetHL();
	uint16_t de = m_R.GetDE();
	uint16_t bc = m_R.GetBC();
	do{
		uint8_t v = m_pMemSys->Read(hl);
		m_pMemSys->Write(de, v);
		--hl, --de, --bc;
	} while(bc != 0);
	m_R.SetHL(hl);
	m_R.SetDE(de);
	m_R.SetBC(bc);
	m_R.F.PV = 0;
	m_R.F.N = 0;
	m_R.F.H = 0;
	return;
}
void CZ80MsxDos::op_CPDR()
{
	uint8_t a = m_R.A;
	uint16_t hl = m_R.GetHL();
	uint16_t bc = m_R.GetBC();
	do{
		uint8_t v = m_pMemSys->Read(hl);
		--hl, --bc;
		if (v == a) {
			m_R.SetHL(hl);
			m_R.SetBC(bc);
			m_R.F.Z = 1;
			m_R.F.PV = 1;
			m_R.F.S = 0;
			m_R.F.N = 1;
			m_R.F.H = 0;
			return;
		}
	}while(bc!=0);
	//
	m_R.SetHL(hl);
	m_R.SetBC(bc);
	//
	m_R.F.Z = 0;
	m_R.F.PV = 0;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_INDR()
{
	uint16_t hl = m_R.GetHL();
	do{
		uint8_t v = m_pIoSys->In(m_R.C);
		m_pMemSys->Write(hl, v);
		--hl,m_R.B--;
	}while(m_R.B!=0);
	//
	m_R.SetHL(hl);
	m_R.F.Z = 1;
	m_R.F.N = 1;
	return;
}
void CZ80MsxDos::op_OUTR()
{
	uint16_t hl = m_R.GetHL();
	do{
		uint8_t v = m_pMemSys->Read(hl);
		m_pIoSys->Out(m_R.C, v);
		--hl, m_R.B--;
	}while(m_R.B!=0);
	m_R.SetHL(hl);
	m_R.F.Z = 1;
	m_R.F.N = 1;
	return;
}

// extended4 for IY
void CZ80MsxDos::op_ADD_IY_BC()
{
	m_R.Add16(&m_R.IY, m_R.GetBC());
	return;
}
void CZ80MsxDos::op_ADD_IY_DE()
{
	m_R.Add16(&m_R.IY, m_R.GetDE());
	return;
}
void CZ80MsxDos::op_LD_IY_ad()
{
	m_R.IY = m_pMemSys->Read(m_R.PC++);
	m_R.IY |= static_cast<uint16_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	return;
}
void CZ80MsxDos::op_LD_memAD_IY()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_pMemSys->Write(ad+0, (m_R.IY>>0)&0xff);
	m_pMemSys->Write(ad+1, (m_R.IY>>8)&0xff);
	return;
}
void CZ80MsxDos::op_INC_IY()
{
	++m_R.IY;
	return;
}
void CZ80MsxDos::op_ADD_IY_IY()
{
	m_R.Add16(&m_R.IY, m_R.IY);
	return;
}
void CZ80MsxDos::op_LD_IY_memAD()
{
	uint16_t ad = m_pMemSys->Read(m_R.PC++);
	ad |= static_cast<z80memaddr_t>(m_pMemSys->Read(m_R.PC++)) << 8;
	//
	m_R.IY = m_pMemSys->Read(ad + 0);
	m_R.IY |= static_cast<uint16_t>(m_pMemSys->Read(ad + 1)) << 8;
	return;
}
void CZ80MsxDos::op_DEC_IY()
{
	--m_R.IY;
	return;
}
void CZ80MsxDos::op_INC_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Inc8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_DEC_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Dec8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_LD_memIYpV_v()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_ADD_IY_SP()
{
	m_R.Add16(&m_R.IY, m_R.SP);
	return;
}
void CZ80MsxDos::op_LD_B_IYH()	// undoc.Z80
{
	m_R.B = (m_R.IY >> 8 ) & 0xFF;
	return;
}
void CZ80MsxDos::op_LD_B_IYL()	// undoc.Z80
{
	m_R.B = m_R.IY & 0xFF;
	return;
}
void CZ80MsxDos::op_LD_B_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_R.B = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_C_IYH()	// undoc.Z80
{
	m_R.C = (m_R.IY >> 8) & 0xFF;
	return;
}
void CZ80MsxDos::op_LD_C_IYL()	// undoc.Z80
{
	m_R.C = m_R.IY & 0xFF;
	return;
}
void CZ80MsxDos::op_LD_C_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_R.C = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_D_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_R.D = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_E_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_R.E = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_IYH_E()	// undoc.Z80
{
	m_R.IY = (m_R.IY & 0xFF) | (static_cast<uint16_t>(m_R.E) << 8);
	return;
}
void CZ80MsxDos::op_LD_H_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_R.H = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_IYL_IYH()	// undoc.Z80	// blueMSXの逆アセンブラだと LD_IYL,H と表示されるがそれは間違いっぽい
{
	m_R.IY = (m_R.IY & 0xFF00) | ((m_R.IY >> 8) & 0xFF);
	return;
}
void CZ80MsxDos::op_LD_IYL_IYL()	// undoc.Z80
{
	// do nothing
	return;
}
void CZ80MsxDos::op_LD_L_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_R.L = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_LD_IYL_A()	// undoc.Z80
{
	m_R.IY = (m_R.IY & 0xFF00) | m_R.A;
	return;
}
void CZ80MsxDos::op_LD_memIYpV_B()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.B);
	return;
}
void CZ80MsxDos::op_LD_memIYpV_C()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.C);
	return;
}
void CZ80MsxDos::op_LD_memIYpV_D()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.D);
	return;
}
void CZ80MsxDos::op_LD_memIYpV_E()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.E);
	return;
}
void CZ80MsxDos::op_LD_memIYpV_H()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.H);
	return;
}
void CZ80MsxDos::op_LD_memIYpV_L()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.L);
	return;
}
void CZ80MsxDos::op_LD_memIYpV_A()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_pMemSys->Write(ad, m_R.A);
	return;
}
void CZ80MsxDos::op_LD_A_IYH()	// undoc.Z80
{
	m_R.A = static_cast<uint8_t>((m_R.IY>>8)&0xFF);
	return;
}
void CZ80MsxDos::op_LD_A_IYL()	// undoc.Z80
{
	m_R.A = static_cast<uint8_t>(m_R.IY&0xFF);
	return;
}
void CZ80MsxDos::op_LD_A_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	m_R.A = m_pMemSys->Read(ad);
	return;
}
void CZ80MsxDos::op_ADD_A_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Add8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_ADC_A_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Add8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_SUB_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sub8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_SBC_A_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sub8Cy(&m_R.A, v, m_R.F.C);
	return;
}
void CZ80MsxDos::op_AND_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.And8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_XOR_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Xor8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_OR_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Or8(&m_R.A, v);
	return;
}
void CZ80MsxDos::op_CP_memIYpV()
{
	uint16_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	uint8_t tempA = m_R.A;
	m_R.Sub8(&tempA, v);
	return;
}
void CZ80MsxDos::op_EXTENDED_4IY2()
{
	// このマシンコードは、DDh+CBh+nn+vv の形式になっていて、
	// データ部nnがコードの途中に位置していることに注意する
	// このメソッドが呼ばれた時点で、DDh+CBhまではデコードされているからPCの位置はnnを示している。
	// 子メソッドを呼び出す時はこの位置を維持する。子メソッドから戻ったら06hの分のPC++を行う。
	uint8_t vv = m_pMemSys->Read(m_R.PC+1);
	auto pFunc = OpCode_Extended4IY2[vv].pFunc;
	(this->*pFunc)();
	m_R.PC++;
	return;
}
void CZ80MsxDos::op_POP_IY()
{
	m_R.IY = m_pMemSys->Read(m_R.SP++);
	m_R.IY |= static_cast<uint16_t>(m_pMemSys->Read(m_R.SP++)) << 8;
	return;
}
void CZ80MsxDos::op_EX_memSP_IY()
{
	uint8_t iyl = (m_R.IY >> 0) & 0xFF;
	uint8_t iyh = (m_R.IY >> 8) & 0xFF;
	m_R.IY = m_pMemSys->Read(m_R.SP+0);
	m_R.IY |= static_cast<uint16_t>(m_pMemSys->Read(m_R.SP+1)) < 8;;
	m_pMemSys->Write(m_R.SP+0, iyl);
	m_pMemSys->Write(m_R.SP+1, iyh);
	return;
}
void CZ80MsxDos::op_PUSH_IY()
{
	m_pMemSys->Write(--m_R.SP, (m_R.IY>>8)&0xFF);
	m_pMemSys->Write(--m_R.SP, (m_R.IY>>0)&0xFF);
	return;
}
void CZ80MsxDos::op_JP_memIY()
{
	m_R.PC = m_R.IY;	// IYの値そのものがアドレス値である
	return;
}
void CZ80MsxDos::op_LD_SP_IY()
{
	m_R.SP = m_R.IY;
	return;
}

// extended4 for IY - 2
void CZ80MsxDos::op_RLC_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rlc8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RRC_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rrc8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RL_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rl8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RR_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Rr8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SLA_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sla8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SRA_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Sra8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SRL_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.Srl8(&v);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_BIT_0_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = (v & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_1_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>1) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_2_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>2) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_3_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>3) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_4_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>4) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_5_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>5) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_6_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>6) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_BIT_7_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	m_R.F.Z = ((v>>7) & 0x01) ^ 0x01;
	m_R.F.N = 0, m_R.F.H = 1;
	return;
}
void CZ80MsxDos::op_RES_0_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 0) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_1_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 1) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_2_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 2) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_3_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 3) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_4_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 4) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_5_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 5) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_6_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 6) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_RES_7_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v &= ((0x01 << 7) ^ 0xFF);
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_0_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 0;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_1_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 1;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_2_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 2;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_3_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 3;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_4_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 4;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_5_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 5;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_6_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 6;
	m_pMemSys->Write(ad, v);
	return;
}
void CZ80MsxDos::op_SET_7_memVpIY()
{
	z80memaddr_t ad = m_R.IY + m_pMemSys->Read(m_R.PC++);
	uint8_t v = m_pMemSys->Read(ad);
	v |= 0x01 << 7;
	m_pMemSys->Write(ad, v);
	return;
}


