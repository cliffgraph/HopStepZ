#pragma once
#include "msxdef.h"
#include "msxdef.h"

class CRam256k : public IZ80MemoryDevice, public IZ80IoDevice
{
private:
	static const int NUM_SEGMENTS = 16;			// 16 * 16[KBytes] = 256[KBytes]
	int m_AssignedSegmentToPage[SLOTNO_NUM];	// 各ページに割り付けているセグメントの番号０～
	uint8_t *m_pPage[MEMPAGENO_NUM];

private:
	static const int TOTAL_SIZE = (Z80_PAGE_SIZE*NUM_SEGMENTS);
	uint8_t	m_Memory[TOTAL_SIZE];

public:
	CRam256k();
	explicit CRam256k(uint8_t v);
	virtual ~CRam256k();

private:
	void init(uint8_t v);

public:
	void Clear(uint8_t v);

public:
/*IZ80MemoryDevice*/
	bool WriteMem(const z80memaddr_t addr, const uint8_t b);
	uint8_t ReadMem(const z80memaddr_t addr) const;
/*IZ80IoDevice*/
	bool OutPort(const z80ioaddr_t addr, const uint8_t b);
	bool InPort(uint8_t *pB, const z80ioaddr_t addr);
};


