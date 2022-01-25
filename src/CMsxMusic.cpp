#include "stdafx.h"
#include "msxdef.h"
#include "CMsxMusic.h"
#include <memory.h>
#include "RmmChipMuse.h"

CMsxMusic::CMsxMusic()
{
	m_pOpll = GCC_NEW RmmChipMuse(RmmChipMuse::OPLL);
	m_pPsg = GCC_NEW RmmChipMuse(RmmChipMuse::PSG);
	m_pOpll->Init();
	m_pPsg->Init();
	return;
}
CMsxMusic::~CMsxMusic()
{
	m_pOpll->SetRegister(0x30, 0x0F);
	m_pOpll->SetRegister(0x31, 0x0F);
	m_pOpll->SetRegister(0x32, 0x0F);
	m_pOpll->SetRegister(0x33, 0x0F);
	m_pOpll->SetRegister(0x34, 0x0F);
	m_pOpll->SetRegister(0x35, 0x0F);
	m_pOpll->SetRegister(0x36, 0x0F);
	m_pOpll->SetRegister(0x37, 0x0F);
	m_pOpll->SetRegister(0x38, 0x0F);
	m_pPsg->SetRegister(0x08, 0x00);
	m_pPsg->SetRegister(0x09, 0x00);
	m_pPsg->SetRegister(0x0A, 0x00);
	NULL_DELETE(m_pOpll);
	NULL_DELETE(m_pPsg);
	return;
}

bool CMsxMusic::WriteMem(const z80memaddr_t addr, const uint8_t b)
{
	// do nothing
	return true;
}

uint8_t CMsxMusic::ReadMem(const z80memaddr_t addr) const
{
	const static uint8_t sign[] ={ "APRLOPLL" };
	if( 0x4018 <= addr && addr <= 0x401F ) {
		return sign[addr-0x4018];
	}
	return 0xff;
}

bool CMsxMusic::OutPort(const z80ioaddr_t addr, const uint8_t b)
{
	bool bRetc = true;
	//bool b2 = false;
	switch(addr)
	{
	// OPLL- REGISTAR ADDRESS RATCH
	case 0x7C:
		m_pOpll->SetRegisterAddr(b);
		break;
	// OPLL- REGISTAR DATA
	case 0x7D:
		m_pOpll->SetRegisterData(b);
		break;
	// PSG - REGISTAR ADDRESS RATCH
	case 0xA0:
		m_pPsg->SetRegisterAddr(b);
		break;
	// PSG - REGISTAR DATA
	case 0xA1:
		m_pPsg->SetRegisterData(b);
		break;
	default:
		bRetc = false;
		break;
	}
	return bRetc;
}

bool CMsxMusic::InPort(uint8_t *pB, const z80ioaddr_t addr)
{
	// do nohing
	return false;
}
