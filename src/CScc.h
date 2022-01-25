#pragma once
#include "msxdef.h"
class RmmChipMuse;

class CScc : public IZ80MemoryDevice
{
private:
	static const z80memaddr_t ADDR_START = 0x9800;
	static const z80memaddr_t ADDR_END = 0x98FF;
	static const z80memaddr_t MEM_SIZE = (ADDR_END-ADDR_START+1);
	uint8_t	m_M9000;
	uint8_t	m_M9800[MEM_SIZE];
	RmmChipMuse *m_pScc;

public:
	CScc();
	virtual ~CScc();

public:
	void SetupHardware();

/*IZ80MemoryDevice*/
public:
	bool WriteMem(const z80memaddr_t addr, const uint8_t b);
	uint8_t ReadMem(const z80memaddr_t addr) const;

private:
	void  setScc(const uint32_t addr, const uint32_t data);
	void  setupScc();


};

