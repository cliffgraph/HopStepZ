#pragma once
#ifdef __linux

#include <arpa/inet.h>			// for socket()
#include <assert.h>
#include <cstdlib>
#include <endian.h>
#include <errno.h>
#include <fstream>
#include <iomanip>				// for マニュピレーター
#include <iostream>
#include <malloc.h>
#include <math.h>
#include <memory.h>
#include <memory>				// for std::unique_ptr, "memory.h"ではないことに注意
#include <netinet/in.h>
#include <netinet/in.h>
#include <sstream>
#include <stdarg.h>				// for va_start, va_end
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>				// for close()
#include <wchar.h>
#include <float.h>
#include <stdint.h>				// for int8_t 等のサイズが保障されているプリミティブ型
#include <netdb.h>

#ifdef _UNICODE
	#define	TCHAR	wchar_t
	#define	_T(x)	L##x
#else /*_UNICODE */
	#define	TCHAR	char
	#define	_T(x)	x
#endif /*_UNICODE */

typedef struct timeval TIMEVAL;
typedef struct timeval *PTIMEVAL;
typedef struct timeval /*FAR*/ *LPTIMEVAL;

typedef int SOCKET;
#define SOCKET_ERROR (-1)

#define FOREGROUND_BLUE      0x0001 // text color contains blue.
#define FOREGROUND_GREEN     0x0002 // text color contains green.
#define FOREGROUND_RED       0x0004 // text color contains red.
#define FOREGROUND_INTENSITY 0x0008 // text color is intensified.
#define BACKGROUND_BLUE      0x0010 // background color contains blue.
#define BACKGROUND_GREEN     0x0020 // background color contains green.
#define BACKGROUND_RED       0x0040 // background color contains red.
#define BACKGROUND_INTENSITY 0x0080 // background color is intensified.

#ifndef _tstoi
#ifdef _UNICODE
	#define	_tstoi(x)	((int)wcstol((x),nullptr,10))
#else
	#define	_tstoi(x)	atoi((x))
#endif
#endif

#ifndef _tstol
#ifdef _UNICODE
	#define	_tstol(x)	((int)wcstol((x),nullptr,10))
#else
	#define	_tstol	atol
#endif
#endif

#ifndef _tstof
#ifdef _UNICODE
	#define	_tstof(x)	wcstof((x),nullptr)
#else
	#define	_tstof(x)	atof((x))
#endif
#endif

#ifndef _tcstod
#ifdef _UNICODE
	#define	_tcstod(x,y)	wcstod((x),(y))
#else
	#define	_tcstod(x,y)	strtod((x),(y))
#endif
#endif

#endif // __linux
