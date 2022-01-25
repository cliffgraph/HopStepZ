
#pragma once
#include "stdafx.h"

class CUTimeCount
{
private:
#ifdef _WIN32
	LARGE_INTEGER m_QPF;
#endif
	uint64_t m_Begin;

private:
	uint64_t getCount();
public:
	CUTimeCount();
	virtual ~CUTimeCount();
public:
	void ResetBegin();
	uint64_t GetTime();
};

