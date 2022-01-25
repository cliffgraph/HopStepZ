#include "stdafx.h"
#include "msxdef.h"
#include "CRam256k.h"
#include <memory.h>

CRam256k::CRam256k()
{
	init(0x00);
	return;
}
CRam256k::CRam256k(uint8_t v)
{
	init(v);
	return;
}
CRam256k::~CRam256k()
{
	// do nothing
	return;
}

void CRam256k::init(uint8_t v)
{
	for(int t = 0; t < MEMPAGENO_NUM; ++t ){
		const int segNo = MEMPAGENO_NUM - t - 1;
		m_AssignedSegmentToPage[t] = segNo;
		m_pPage[t] = &m_Memory[segNo*Z80_PAGE_SIZE];
	}
	Clear(v);
	return;
}

void CRam256k::Clear(uint8_t v)
{
	memset(m_Memory, v, TOTAL_SIZE);
	return;
}

bool CRam256k::WriteMem(const z80memaddr_t addr, const uint8_t b)
{
	const int pageNo = addr / Z80_PAGE_SIZE;
	const int offset = addr % Z80_PAGE_SIZE;
	m_pPage[pageNo][offset] = b;
	return true;
}

uint8_t CRam256k::ReadMem(const z80memaddr_t addr) const
{
	const int pageNo = addr / Z80_PAGE_SIZE;
	const int offset = addr % Z80_PAGE_SIZE;
	return m_pPage[pageNo][offset];
}

bool CRam256k::OutPort(const z80ioaddr_t addr, const uint8_t b)
{
// ページ毎にメモリマッパーセグメントを割り付けることができます。
// それをI/Oポート FCh～FFhで行います。
// MSX-Datapackのその対応の記載は間違いらしい。
// 正しくは、
// 	FCH = ページ0
// 	FDH = ページ1
// 	FEH = ページ2
// 	FFH = ページ3
// 情報源：http://map.grauw.nl/resources/msx_io_ports.php

	bool bRet= false;
	if( 0xFC <= addr && addr <= 0xFF ){
		auto pageNo = static_cast<MEMPAGENO>(addr - 0xFC);
		m_AssignedSegmentToPage[pageNo] = b;
		m_pPage[pageNo] = &m_Memory[b*Z80_PAGE_SIZE];
		bRet= true;
	}
	return bRet;
}

bool CRam256k::InPort(uint8_t *pB, const z80ioaddr_t addr)
{
	bool bRet= false;
	if( 0xFC <= addr && addr <= 0xFF ){
		auto pageNo = static_cast<MEMPAGENO>(addr - 0xFC);
		*pB = static_cast<uint8_t>(m_AssignedSegmentToPage[pageNo]);
		bRet= true;
	}
	return bRet;
}