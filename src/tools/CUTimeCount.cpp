
#include "stdafx.h"
#include "CUTimeCount.h"

CUTimeCount::CUTimeCount()
{
#ifdef _WIN32
	::QueryPerformanceFrequency(&m_QPF);
#endif
	m_Begin = getCount();
	return;
}

CUTimeCount::~CUTimeCount()
{
	// do nothing
	return;
}

void CUTimeCount::ResetBegin()
{
	m_Begin = getCount();
	return;
}

uint64_t CUTimeCount::GetTime()
{
	return getCount() - m_Begin;
}

uint64_t CUTimeCount::getCount()	// マイクロ秒単位
{
#ifdef _WIN32
	LARGE_INTEGER step;
	::QueryPerformanceCounter(&step);
	uint64_t usec = static_cast<uint64_t>(
		(static_cast<double>(step.QuadPart) / m_QPF.QuadPart) * 1000000UL);
	return usec;
#endif
#ifdef	__linux
	struct timespec tmp;
	clock_gettime(CLOCK_MONOTONIC, &tmp);
	uint64_t usec =
		static_cast<uint64_t>(tmp.tv_sec) * 1000000ULL
		+ static_cast<uint64_t>(tmp.tv_nsec / 1000L);	// ナノ単位からマイクロ単位に。
	return usec;
#endif

}
