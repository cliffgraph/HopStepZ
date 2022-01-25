#pragma once
#include "msxdef.h"
#include "msxdef.h"
#include "CMsxVoidMemory.h"
#include <vector>

class CMsxMemSlotSystem : public IZ80IoDevice
{
private:
	struct PAGEBIND {
		SLOTNO BaseNo, ExtNo;
		PAGEBIND() : BaseNo(SLOTNO_0), ExtNo(SLOTNO_0){return;}
		PAGEBIND(SLOTNO b, SLOTNO e) : BaseNo(b), ExtNo(e){return;}
	};

private:
	//
	CMsxVoidMemory m_VoidMem;
	// 各ページに対応するメモリ装置オブジェクトへのポインタを保持する
	IZ80MemoryDevice* m_MemObjs[SLOTNO_NUM][SLOTNO_NUM];	// 基本スロット：拡張スロット
	// CPUメモリ空間を構成する各ページの、現在のスロット番号を保持する
	PAGEBIND m_SlotNoToPage[MEMPAGENO_NUM];

public:
	CMsxMemSlotSystem();
	virtual ~CMsxMemSlotSystem();

public:
	void JoinObject(const SLOTNO baseSlotNo, const SLOTNO extSlotNo, IZ80MemoryDevice *pObj);
	void ChangeSlot(const MEMPAGENO pageNo, const SLOTNO baseSlotNo, const SLOTNO extSlotNo);
	void GetSlot(SLOTNO *pBaseSlotNo, SLOTNO *pExtSlotNo, const MEMPAGENO pageNo);
	void BinaryTo(const z80memaddr_t dest, const std::vector<uint8_t> &block);

private:
	void writeByte(const z80memaddr_t addr, const uint8_t b);
	uint8_t readByte(const z80memaddr_t addr) const;

public:
	void Write(const z80memaddr_t addr, const uint8_t b);
	uint8_t Read(const z80memaddr_t addr) const;
	int8_t ReadInt8(const z80memaddr_t addr) const;
	void Push16(const uint16_t w);
	void ReadString(std::string *pStr, z80memaddr_t srcAddr);
	uint16_t ReadWord(const z80memaddr_t addr) const;
	void WriteWord(const z80memaddr_t addr, uint16_t v);

/*IZ80IoDevice*/
public:
	bool OutPort(const z80ioaddr_t addr, const uint8_t b);
	bool InPort(uint8_t *pB, const z80ioaddr_t addr);
};
