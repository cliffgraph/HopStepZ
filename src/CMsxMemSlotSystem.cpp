#include "stdafx.h"
#include "msxdef.h"
#include <vector>
#include <assert.h>
#include "CMsxMemSlotSystem.h"

CMsxMemSlotSystem::CMsxMemSlotSystem()
{
	//
	for( int x = 0; x < SLOTNO_NUM; ++x){
		for( int y = 0; y < SLOTNO_NUM; ++y){
			m_MemObjs[x][y] = &m_VoidMem;
		}
	}

	// Z80面り空間の全ページはスロット3-0にしておく
	for( int t = 0; t < MEMPAGENO_NUM; ++t)
		m_SlotNoToPage[t] = PAGEBIND(SLOTNO_3, SLOTNO_0);
	return;
}

CMsxMemSlotSystem::~CMsxMemSlotSystem()
{
	// do nothing;
	return;
}

void CMsxMemSlotSystem::JoinObject(
	const SLOTNO baseSlotNo, const SLOTNO extSlotNo, IZ80MemoryDevice *pObj)
{
	assert(SLOTNO_0 <= baseSlotNo && baseSlotNo <= SLOTNO_3);
	assert(SLOTNO_0 <= extSlotNo && extSlotNo <= SLOTNO_3);
	assert(pObj != nullptr);
	m_MemObjs[baseSlotNo][extSlotNo] = pObj;
	return;
}

/** 指定ページへのスロット割付を、指定スロットに切り替える 
 */
void CMsxMemSlotSystem::ChangeSlot(
	const MEMPAGENO pageNo, const SLOTNO baseSlotNo, const SLOTNO extSlotNo)
{
	assert(MEMPAGE_0 <= pageNo && pageNo <= MEMPAGE_3);
	assert(SLOTNO_0 <= baseSlotNo && baseSlotNo <= SLOTNO_3);
	assert(SLOTNO_0 <= extSlotNo && extSlotNo <= SLOTNO_3);
	m_SlotNoToPage[pageNo] = PAGEBIND(baseSlotNo, extSlotNo);
	return;
}

/** 指定したページに割り付けられているスロットの番号を返す
 */
void CMsxMemSlotSystem::GetSlot(
	SLOTNO *pBaseSlotNo, SLOTNO *pExtSlotNo, const MEMPAGENO pageNo)
{
	assert(MEMPAGE_0 <= pageNo && pageNo <= MEMPAGE_3);
	auto &pb = m_SlotNoToPage[pageNo];
	*pBaseSlotNo = pb.BaseNo;
	*pExtSlotNo = pb.ExtNo;
	return;
}

/** 指定メモリにバイナリデータを書き込む
 */
void CMsxMemSlotSystem::BinaryTo(
	const z80memaddr_t dest, const std::vector<uint8_t> &block)
{
	z80memaddr_t p = dest;
	for(auto b : block){
		writeByte(p++, b);
	}
	return;
}

void CMsxMemSlotSystem::writeByte(const z80memaddr_t addr, const uint8_t b)
{
	auto pageNo = addr / Z80_PAGE_SIZE;
	auto slot = m_SlotNoToPage[pageNo];
	auto *p = m_MemObjs[slot.BaseNo][slot.ExtNo];
	p->WriteMem(addr, b);
	return;
}

uint8_t CMsxMemSlotSystem::readByte(const z80memaddr_t addr) const
{
	auto pageNo = addr / Z80_PAGE_SIZE;
	auto slot = m_SlotNoToPage[pageNo];
	auto *p = m_MemObjs[slot.BaseNo][slot.ExtNo];
	uint8_t b = p->ReadMem(addr);
	return b;
}

void CMsxMemSlotSystem::Write(const z80memaddr_t addr, const uint8_t b)
{
	if( addr == 0xffff ) {
		// FFFFHへのアクセスに従い、拡張スロットを切り替える
		m_SlotNoToPage[MEMPAGE_0].ExtNo = static_cast<SLOTNO>((b >> 0) & 0x3);
		m_SlotNoToPage[MEMPAGE_1].ExtNo = static_cast<SLOTNO>((b >> 2) & 0x3);
		m_SlotNoToPage[MEMPAGE_2].ExtNo = static_cast<SLOTNO>((b >> 4) & 0x3);
		m_SlotNoToPage[MEMPAGE_3].ExtNo = static_cast<SLOTNO>((b >> 6) & 0x3);
	}
	else {
		writeByte(addr, b);
	}
	return;
}

uint8_t CMsxMemSlotSystem::Read(const z80memaddr_t addr) const
{
	uint8_t v;
	if( addr == 0xffff ) {
		v = ((m_SlotNoToPage[MEMPAGE_0].ExtNo & 0x03) << 0) |
			((m_SlotNoToPage[MEMPAGE_1].ExtNo & 0x03) << 2) | 
			((m_SlotNoToPage[MEMPAGE_2].ExtNo & 0x03) << 4) | 
			((m_SlotNoToPage[MEMPAGE_3].ExtNo & 0x03) << 6); 
		v ^= 0xff;	
	}
	else{
		v = readByte(addr);
	}
	return v;
}

int8_t CMsxMemSlotSystem::ReadInt8(const z80memaddr_t addr) const
{
	return static_cast<int8_t>(readByte(addr));
}

uint16_t CMsxMemSlotSystem::ReadWord(const z80memaddr_t addr) const
{
	uint16_t v;
	v = Read(0xFC9E + 0);
	v |= Read(0xFC9E + 1) << 8;
	return v;
}

void CMsxMemSlotSystem::WriteWord(const z80memaddr_t addr, uint16_t v)
{
	Write(addr + 0, (v>>0) & 0xff);
	Write(addr + 1, (v>>8) & 0xff);
	return;
}

void CMsxMemSlotSystem::ReadString(std::string *pStr, z80memaddr_t srcAddr)
{
	for(;;){
		uint8_t v = readByte(srcAddr++);
		if( v == '\0' )
			break;
		*pStr += static_cast<char>(v);
	}
	return;
}
bool CMsxMemSlotSystem::OutPort(const z80ioaddr_t addr, const uint8_t b)
{
	if( addr == 0xa8 ) {
		// A8Hへのアクセスに従い、基本スロットを切り替える
		m_SlotNoToPage[MEMPAGE_0].BaseNo = static_cast<SLOTNO>((b >> 0) & 0x3);
		m_SlotNoToPage[MEMPAGE_1].BaseNo = static_cast<SLOTNO>((b >> 2) & 0x3);
		m_SlotNoToPage[MEMPAGE_2].BaseNo = static_cast<SLOTNO>((b >> 4) & 0x3);
		m_SlotNoToPage[MEMPAGE_3].BaseNo = static_cast<SLOTNO>((b >> 6) & 0x3);
		return true;
	}
	return false;
}
bool CMsxMemSlotSystem::InPort(uint8_t *pB, const z80ioaddr_t addr)
{
	if( addr == 0xa8 ) {
		*pB =
			((m_SlotNoToPage[MEMPAGE_0].BaseNo & 0x03) << 0) |
			((m_SlotNoToPage[MEMPAGE_1].BaseNo & 0x03) << 2) | 
			((m_SlotNoToPage[MEMPAGE_2].BaseNo & 0x03) << 4) | 
			((m_SlotNoToPage[MEMPAGE_3].BaseNo & 0x03) << 6); 
		return true;
	}
	return false;
}
