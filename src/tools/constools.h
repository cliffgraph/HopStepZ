
#pragma once
#include "stdafx.h"
#include <vector>

bool t_ConsoleCursor(const bool bOn);
void t_OC(const uint16_t attr, const TCHAR *pFormat, ...);
void t_OC(const TCHAR *pFormat, ...);
void t_IV(const TCHAR *pItem, const TCHAR *pFormat, ...);
