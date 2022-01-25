#pragma once
#include <stdint.h>

typedef uint8_t		z80ioaddr_t;
typedef uint16_t	z80memaddr_t;
typedef uint8_t		dosfuncno_t;
static const int Z80_PAGE_SIZE = 16*1024;
static const int Z80_MEMORY_SIZE = 64*1024;

class IZ80MemoryDevice
{
public:
	virtual ~IZ80MemoryDevice(){return;}
public:
	virtual bool WriteMem(const z80memaddr_t addr, const uint8_t b) = 0;
	virtual uint8_t ReadMem(const z80memaddr_t addr) const = 0;
};

class IZ80IoDevice
{
public:
	virtual ~IZ80IoDevice(){return;}
public:
	virtual bool OutPort(const z80ioaddr_t addr, const uint8_t b) = 0;
	virtual bool InPort(uint8_t *pB, const z80ioaddr_t addr) = 0;
};


enum SLOTNO : int
{
	SLOTNO_0 = 0,
	SLOTNO_1 = 1,
	SLOTNO_2 = 2,
	SLOTNO_3 = 3,
	SLOTNO_NUM = 4,
};
enum MEMPAGENO : int
{
	MEMPAGE_0 = 0,
	MEMPAGE_1 = 1,
	MEMPAGE_2 = 2,
	MEMPAGE_3 = 3,
	MEMPAGENO_NUM = 4,
};

static const z80memaddr_t PAGE0_END = 0x3FFF;
static const z80memaddr_t PAGE1_END = 0x8FFF;
static const z80memaddr_t PAGE2_END = 0xBFFF;
static const z80memaddr_t PAGE3_END = 0xFFFF;