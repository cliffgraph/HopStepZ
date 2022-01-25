
#pragma once
#include "stdafx.h"
#include <vector>

void t_FromSjis(tstring *pDest, const void *pSrc, const size_t srcAreaSize);
size_t t_ToWiden(const std::string &src, std::wstring *pDest);
const std::vector<tstring> &t_CreateStringArray(const tstring &sep, const tstring &target, std::vector<tstring> *pDest);
int t_Trimer(const int v, const int minv, const int maxv);
void t_MemSetZero(void *const p, const size_t size);
void t_ToNarrow(const std::wstring &src, std::string *pDest);
bool t_kbhit(void);
char t_getch(void);
void t_sleep(unsigned int msec);
uint64_t t_GetFileSize(const tstring &targetPath);
bool t_ReadFile(const tstring &targetPath, std::vector<uint8_t> **pp);
