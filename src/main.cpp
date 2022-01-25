#include "stdafx.h"
#include "tools.h"
#include "msxdef.h"
#include "CHopStepZ.h"
#include "playercom.h"

static bool g_bRequestStop = false;
#ifdef __linux
#include <signal.h>
static void ctrlc_handler(int signo)
{
	if( signo == SIGINT ){
		g_bRequestStop = true;
	}
	return;
}
#endif

#ifdef _WIN32
int _tmain(int argc, _TCHAR *argv[])
#endif
#ifdef __linux
int main(int argc, char *argv[])
#endif
{
	setlocale(LC_ALL, "");
	std::wcout << _T("\n") << _T("HopStepZ version 1.00 by @harumakkin. 2021\n");
	if( argc != 3 ){
		std::wcout << _T(" USAGE: hopstepz \"mgsdrv.com\" \"file.MGS\"\n\n");
		return EXIT_FAILURE;
	}

#ifdef _WIN32
	timeBeginPeriod(1);
	tstring argv1(argv[1]);
	tstring argv2(argv[2]);
#endif
#ifdef __linux
	tstring argv1;
	tstring argv2;
	t_ToWiden(argv[1], &argv1);
	t_ToWiden(argv[2], &argv2);

	struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_handler = ctrlc_handler;
    sigaction(SIGINT, &act, NULL);
#endif

	auto *pComFile = GCC_NEW std::vector<uint8_t>();
	if( !t_ReadFile(argv1, &pComFile) ) {
		std::wcout << _T("Not found ") << argv1 << _T("\n");
		return EXIT_FAILURE;
	}
	auto *pMgsFile = GCC_NEW std::vector<uint8_t>();
	if( !t_ReadFile(argv2, &pMgsFile) ) {
		std::wcout << _T("Not found ") << argv2 << _T("\n");
		return EXIT_FAILURE;
	}
	auto *pPlayerFile = GCC_NEW std::vector<uint8_t>();
	if( t_ReadFile(tstring(_T("PLAYER.COM.HSZ")), &pPlayerFile) ) {
		std::wcout << _T("Use \"") << _T("PLAYER.COM.HSZ") << _T("\"\n");
	}
	else{
		GetBinaryPlayerCom(pPlayerFile);
	}

	CHopStepZ *pMsx = GCC_NEW CHopStepZ();
	pMsx->Setup();

	// MGSDRV.COMを実行して常駐させる
	pMsx->MemoryWrite(0x0100, *pComFile);
	pMsx->Run(0x0100, 0xD400, &g_bRequestStop);

#ifdef __linux
	tstring title;
	auto *pMs = reinterpret_cast<char*>(pMgsFile->data());
	size_t ssz = strlen(pMs);
	t_FromSjis(&title, pMs, ssz);
	::wprintf(_T("PLAY:\n%ls\n"), title.c_str());
#endif

	// 演奏データとプレイヤープログラムをロードして再生開始
	pMsx->MemoryWrite(0x8000, *pMgsFile);
	pMsx->MemoryWrite(0x0100, *pPlayerFile);
	pMsx->Run(0x0100, 0xD400, &g_bRequestStop);
	::wprintf(_T("\nSTOP\n"));


	NULL_DELETE(pMgsFile);
	NULL_DELETE(pComFile);
	NULL_DELETE(pMsx);

#ifdef _WIN32
	timeEndPeriod(1);
#endif
	return EXIT_SUCCESS;
}

