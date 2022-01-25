#pragma once
#include "stdafx.h"

class CMsxMemSlotSystem;
class CMsxIoSystem;
class CZ80MsxDos;
class CRam256k;
class CMsxMusic;
class CScc;

class CHopStepZ
{
private:
	CMsxMemSlotSystem	*m_pSlot;
	CMsxIoSystem 		*m_pIo;
	CZ80MsxDos			*m_pCpu;
	CRam256k			*m_pRam256;
	CMsxMusic			*m_pFm;
	CScc				*m_pScc;

public:
	CHopStepZ();
	virtual ~CHopStepZ();
public:
	void Setup();
	void Run(const z80memaddr_t startAddr, const z80memaddr_t stackAddr, bool *pStop);

public:
	void MemoryWrite(const z80memaddr_t addr, const uint8_t b);
	void MemoryWrite(const z80memaddr_t addr, const std::vector<uint8_t> &block);

};

