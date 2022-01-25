#pragma once

class RmmChipMuse 
{
private:
	static int s_AlphaCount;
public:
	enum TARGETCHIP	{OPLL, PSG, SCC };
private:
	TARGETCHIP m_TergetChip;
public:
	explicit RmmChipMuse(const TARGETCHIP target);
	virtual ~RmmChipMuse();

// 上位処理向け
public:
	bool Init();
	bool ResetChip();
	bool SetRegister(const uint32_t addr, const uint32_t data);
	bool SetRegisterAddr(const uint32_t addr);
	bool SetRegisterData(const uint32_t data);


// 内部処理
private:
	void setPSG(const uint32_t addr, const uint32_t data);
	void setOPLL(const uint32_t addr, const uint32_t data);
	void setSCC(const uint32_t addr, const uint32_t data);
	void setRegAddress(const uint32_t addr);
	void setRegData(const uint32_t data);
	void sendPinD(const uint8_t data);
	void setRegAddressEX(const uint32_t addr);

	void resetDevice();
	void initRegs();
	void initClockfoHraSCC();
};
