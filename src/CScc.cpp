#include "stdafx.h"
#include "msxdef.h"
#include "CScc.h"
#include <memory.h>
#include "RmmChipMuse.h"

CScc::CScc()
{
	m_pScc = GCC_NEW RmmChipMuse(RmmChipMuse::SCC);
	m_pScc->Init();
	m_M9000 = 0;
	return;
}
CScc::~CScc()
{
	m_pScc->SetRegister(0x988F, 0);
	NULL_DELETE(m_pScc);
	return;
}

void CScc::SetupHardware()
{
	return;
}

bool CScc::WriteMem(const z80memaddr_t addr, const uint8_t b)
{
	bool bRetc = false;
	if( addr == 0x9000 ){
		m_M9000 = b;
		m_pScc->SetRegister(addr, b);
		bRetc = true;
	}
	else if(m_M9000 == 0x3f && ADDR_START <= addr && addr <= ADDR_END ){
		m_M9800[addr-ADDR_START] = b;
		m_pScc->SetRegister(addr, b);
		bRetc = true;
	}
	return bRetc;
}

uint8_t CScc::ReadMem(const z80memaddr_t addr) const
{
	uint8_t b = 0xff;
	if( addr == 0x9000 ){
		b = m_M9000;
	}
	else if(m_M9000 == 0x3f && ADDR_START <= addr && addr <= ADDR_END ){
		b = m_M9800[addr-ADDR_START];
	}
	return b;
}
