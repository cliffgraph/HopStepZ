#include "stdafx.h"
#include "CZ80MsxDos.h"
#include "CMsxMemSlotSystem.h"
#include "CMsxIoSystem.h"
#include "CRam256k.h"
#include "CMsxMusic.h"
#include "CScc.h"
#include "CHopStepZ.h"

CHopStepZ::CHopStepZ()
{
	m_pSlot = nullptr;
	m_pIo = nullptr;
	m_pCpu = nullptr;
	m_pRam256 = nullptr;
	m_pFm = nullptr;
	m_pScc = nullptr;
	return;
}
CHopStepZ::~CHopStepZ()
{
	NULL_DELETE(m_pCpu);
	NULL_DELETE(m_pRam256);
	NULL_DELETE(m_pFm);
	NULL_DELETE(m_pScc);
	NULL_DELETE(m_pSlot);
	NULL_DELETE(m_pIo);
	return;
}

void CHopStepZ::Setup()
{
	m_pSlot = GCC_NEW CMsxMemSlotSystem();
	m_pIo = GCC_NEW CMsxIoSystem();
	m_pIo->JoinObject(m_pIo);
	m_pIo->JoinObject(m_pSlot);
	// device - 256k ram #3-0
	m_pRam256 = GCC_NEW CRam256k(0xc9);
	m_pSlot->JoinObject(SLOTNO_3, SLOTNO_0, m_pRam256);
	m_pIo->JoinObject(m_pRam256);
	// device - fm-bios #0-2
	m_pFm = GCC_NEW CMsxMusic();
	m_pSlot->JoinObject(SLOTNO_0, SLOTNO_2, m_pFm);
	m_pIo->JoinObject(m_pFm);
	// device - scc #1-0
	m_pScc = GCC_NEW CScc();
	m_pSlot->JoinObject(SLOTNO_1, SLOTNO_0, m_pScc);
	// CPU
	m_pCpu = GCC_NEW CZ80MsxDos();
	m_pCpu->SetSubSystem(m_pSlot, m_pIo);	

	// メモリセットアップ
	for( int t = 0; t < 0xf0; ++t)
		m_pSlot->Write(t, 0x00);
	// ページnのRAMのスロットアドレス
	m_pSlot->Write(0xF341, 0x83);	// Page.0
	m_pSlot->Write(0xF342, 0x83);	// Page.1
	m_pSlot->Write(0xF343, 0x83);	// Page.2
	m_pSlot->Write(0xF344, 0x83);	// Page.3
	// 拡張スロットの状態(拡張の有無を管理するのが面倒なので、すべて拡張しているとする）
	m_pSlot->Write(0xFCC1, 0x80);	// SLOT.0 + MAIN-ROMのスロットアドレス
	m_pSlot->Write(0xFCC2, 0x80);	// SLOT.1
	m_pSlot->Write(0xFCC3, 0x80);	// SLOT.2
	m_pSlot->Write(0xFCC4, 0x80);	// SLOT.3
	m_pSlot->Write(0xFFF7, 0x80);	// 0xFCC1と同一の内容を書く
	// FDCの作業領域らしいがMGSDRVがここ読み込んで何か書き換えを行っていたので、
	// DOS起動直後の値を再現しておく
	m_pSlot->Write(0xF349, 0xbb);
	m_pSlot->Write(0xF34A, 0xe7);
	//
	m_pSlot->Write(0xFF3E, 0xc9);	// H.NEWS
	m_pSlot->Write(0xFD9F, 0xc9);	// H.TIMI
	return;
}

void CHopStepZ::MemoryWrite(const z80memaddr_t addr, const uint8_t b)
{
	m_pSlot->Write(addr, b);
	return;
}

void CHopStepZ::MemoryWrite(const z80memaddr_t addr, const std::vector<uint8_t> &block)
{
	m_pSlot->BinaryTo(addr, block);
	return;
}

void CHopStepZ::Run(const z80memaddr_t startAddr, const z80memaddr_t stackAddr, bool *pStop)
{
	m_pSlot->Write(0x0006, (stackAddr>>0)&0xff);
	m_pSlot->Write(0x0007, (stackAddr>>8)&0xff);
	m_pCpu->Push16(0x0000);
	m_pCpu->ResetCpu(startAddr, stackAddr);

	while( m_pCpu->GetPC() != 0 && (pStop==nullptr||!*pStop)) {
	 	m_pCpu->Execution();
	}
	return;
}





