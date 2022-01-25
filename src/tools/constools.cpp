#include "stdafx.h"
#include "constools.h"

/** コンソールのカーソル表示をON/OFFする
*/
bool t_ConsoleCursor(const bool bOn)
{
#ifdef _WIN32
	CONSOLE_CURSOR_INFO cur;
	::GetConsoleCursorInfo( ::GetStdHandle(STD_OUTPUT_HANDLE), &cur);
	const bool bOld = (cur.bVisible==FALSE) ? false : true;
	cur.bVisible = (bOn)?TRUE:FALSE;
	::SetConsoleCursorInfo( ::GetStdHandle(STD_OUTPUT_HANDLE), &cur);
	return bOld;
#endif
#ifdef __linux
	// 方法不明
#endif
	return true;
}

void t_OC(const uint16_t attr, const TCHAR *pFormat, ...)
{
#ifdef _WIN32
	const HANDLE hConsole = ::GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
    if(!::GetConsoleScreenBufferInfo(hConsole, &consoleInfo))
		return;
	::SetConsoleTextAttribute(hConsole, attr);
#endif

	const int LENGTH = 1024*2;
	std::unique_ptr<TCHAR[]> pMess(GCC_NEW TCHAR[LENGTH+1]);
    va_list list;
    va_start(list, pFormat);
	#if defined(_WIN32)
		_vstprintf_s(pMess.get(), LENGTH+1, pFormat, list);
	#endif
	#ifdef __linux
		#ifdef _UNICODE
			vswprintf(pMess.get(), LENGTH+1, pFormat, list);
		#else
			vsprintf(pMess.get(), pFormat, list);
		#endif
	#endif
	va_end(list);
	#ifdef _UNICODE
		#ifdef _WIN32
		_tprintf(_T("%ls"), pMess.get());
		#endif
		#ifdef __linux
		std::wcout << tstring(pMess.get());
		#endif
	#else
		std::cout << pMess;
	#endif

#if defined(_WIN32) 
    ::SetConsoleTextAttribute(hConsole, consoleInfo.wAttributes);
#endif
	return;
}

void t_OC(const TCHAR *pFormat, ...)
{
#ifdef _WIN32
	const uint16_t attr = FOREGROUND_INTENSITY|FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE;
	const HANDLE hConsole = ::GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
    if(!::GetConsoleScreenBufferInfo(hConsole, &consoleInfo))
		return;
	::SetConsoleTextAttribute(hConsole, attr);
#endif

	const int LENGTH = 1024*2;
	std::unique_ptr<TCHAR[]> pMess(GCC_NEW TCHAR[LENGTH+1]);
    va_list list;
    va_start(list, pFormat);
	#if defined(_WIN32)
		_vstprintf_s(pMess.get(), LENGTH+1, pFormat, list);
	#endif
	#ifdef __linux
		#ifdef _UNICODE
			vswprintf(pMess.get(), LENGTH+1, pFormat, list);
		#else
			vsprintf(pMess.get(), pFormat, list);
		#endif
	#endif
	va_end(list);
	#ifdef _UNICODE
		#ifdef _WIN32
		_tprintf(_T("%ls"), pMess.get());
		#endif
		#ifdef __linux
		wprintf(pMess.get());
//		std::wcout << tstring(pMess.get());
		#endif
	#else
		std::cout << pMess;
	#endif

#if defined(_WIN32) 
    ::SetConsoleTextAttribute(hConsole, consoleInfo.wAttributes);
#endif
	return;
}

void t_IV(const TCHAR *pItem, const TCHAR *pFormat, ...)
{
	t_OC(FOREGROUND_WHITE, pItem);

#ifdef _WIN32
	const uint16_t attr = FOREGROUND_GREEN|FOREGROUND_BLUE;
	const HANDLE hConsole = ::GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO consoleInfo;
    if(!::GetConsoleScreenBufferInfo(hConsole, &consoleInfo))
		return;
	::SetConsoleTextAttribute(hConsole, attr);
#endif

	const int LENGTH = 1024*2;
	std::unique_ptr<TCHAR[]> pMess(GCC_NEW TCHAR[LENGTH+1]);
    va_list list;
    va_start(list, pFormat);
	#if defined(_WIN32)
		_vstprintf_s(pMess.get(), LENGTH+1, pFormat, list);
	#endif
	#ifdef __linux
		#ifdef _UNICODE
			vswprintf(pMess.get(), LENGTH+1, pFormat, list);
		#else
			vsprintf(pMess.get(), pFormat, list);
		#endif
	#endif
	va_end(list);
	#ifdef _UNICODE
		#ifdef _WIN32
		_tprintf(_T("%ls"), pMess.get());
		#endif
		#ifdef __linux
		wprintf(pMess.get());
//		std::wcout << tstring(pMess.get());
		#endif
	#else
		std::cout << pMess;
	#endif

#if defined(_WIN32) 
    ::SetConsoleTextAttribute(hConsole, consoleInfo.wAttributes);
#endif
	return;
}

