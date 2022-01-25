#include "stdafx.h"
#include "tools.h"
#ifdef _WIN32 
#include <conio.h>
#endif
#ifdef __linux
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <iconv.h>
#endif

/** SJIS をtstringに変換する
 @param pDest 格納先領域へのポインタ
 @param pSrc 変換元文字列(SJIS)へのポインタ
 @param srcSize 変換元サイズ
 @return none
*/
void t_FromSjis(tstring *pDest, const void *pSrc, const size_t srcAreaSize)
{
#ifdef _WIN32
	const UINT codePage = CP_ACP;
	const DWORD flags = 0;
	size_t resultSize = MultiByteToWideChar(
		codePage,
		flags,
		reinterpret_cast<LPCSTR>(pSrc),
		static_cast<int>(srcAreaSize),
		nullptr,
		0);
	std::unique_ptr<TCHAR[]> pDestBuff(GCC_NEW TCHAR[resultSize+1]());
	MultiByteToWideChar(
		codePage,
		flags,
		reinterpret_cast<LPCSTR>(pSrc),
		static_cast<int>(srcAreaSize),
		pDestBuff.get(),
		static_cast<int>(resultSize+1));
	*pDest = tstring(pDestBuff.get());
#endif
#ifdef __linux
	iconv_t fd = iconv_open("UTF32LE", "SJIS");
	if( fd !=  (iconv_t)-1 )
	{
		size_t srcSize = srcAreaSize;
		char *pS = reinterpret_cast<char*>(const_cast<void*>(pSrc));
		const size_t CANDI_DEST_SIZE = sizeof(wchar_t)*(srcAreaSize+1);
		size_t destSize = CANDI_DEST_SIZE;
		std::unique_ptr<wchar_t> pDestChars( GCC_NEW wchar_t[destSize+1]() );
		char *pD = reinterpret_cast<char*>(pDestChars.get());
		iconv(fd, &pS, &srcSize, &pD, &destSize );
		*pDest = tstring(pDestChars.get());
	}
	iconv_close(fd);
#endif
}

/** マルチバイト文字列からワイド文字列（ロケール依存）
*/
size_t t_ToWiden(const std::string &src, std::wstring *pDest)
{
#ifdef _WIN32
	size_t count = src.length() + 1;
	wchar_t *pWcs = GCC_NEW wchar_t[count]();
	size_t retlen;
	size_t result = mbstowcs_s(&retlen, pWcs, count, src.c_str(), _TRUNCATE);
#endif
#ifdef __linux
	size_t count = mbstowcs(nullptr, src.c_str(), 0) + 1;
	wchar_t *pWcs = GCC_NEW wchar_t[count]();
	size_t result = mbstowcs(pWcs, src.c_str(), count);
#endif
	*pDest = std::wstring(pWcs);
	delete[] pWcs;
	return result;
}

/** target 文字列を sep で区切って pDest に格納する
 */
const std::vector<tstring> &t_CreateStringArray(
	const tstring &sep, const tstring &target, std::vector<tstring> *pDest)
{
	assert( pDest != nullptr );
	pDest->clear();

	if( target.empty() )
		return *pDest;

	tstring::size_type end = tstring::npos;
	tstring str = target;
	for(;;)
	{
		end = str.find(sep, 0);
		tstring token = str.substr(0, end);
		pDest->push_back(token);
		if( end == tstring::npos )
			break;
		str = str.substr(end+sep.size());
	}
	return *pDest;
}

/** v が minv以下ならminvを返し、minx以上ならminxを返す。それ以外はvをそのまま返す
*/
int t_Trimer(const int v, const int minv, const int maxv)
{
	if( v <= minv )
		return minv;
	if( maxv <= v )
		return maxv;
	return v;
}


void t_MemSetZero(void *const p, const size_t size)
{
	memset(p, 0, size);
	return;
}


/** ワイド文字列からマルチバイト文字列（ロケール依存）
* @param src ワイド文字列への参照
* @param pDest 変換後のマルチバイト文字列を受け取る string オブジェクトへのポインタ
*/
void t_ToNarrow(const std::wstring &src, std::string *pDest)
{
	size_t count = src.length() * MB_CUR_MAX + 1;
	char *aMbs = GCC_NEW char[count]();
#ifdef _WIN32
	size_t retlen;
	wcstombs_s(&retlen, aMbs, count, src.c_str(), _TRUNCATE );
#endif
#ifdef __linux
	wcstombs(aMbs, src.c_str(), count);
#endif
	*pDest = aMbs;
	delete[] aMbs;
}

/** _kbhit()
*/
bool t_kbhit(void)
{
#ifdef _WIN32 
	return (_kbhit()==0)?false:true;
#endif
#ifdef __linux
	// Linux上での、kbhit()の再現
	// 情報源：https://hotnews8.net/programming/tricky-code/c-code03
    struct termios oldt, newt;
    int ch;
    int oldf;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    if (ch != EOF) {
        ungetc(ch, stdin);
        return true;
    }
    return false;
#endif
}

/** getch()
*/
char t_getch(void)
{
#ifdef _WIN32
	return _getch();
#endif
#ifdef __linux
	// 情報源：https://www.it-swarm-ja.tech/ja/c/linux%E3%81%AEgetch%EF%BC%88%EF%BC%89%E3%81%8A%E3%82%88%E3%81%B3getche%EF%BC%88%EF%BC%89%E3%81%A8%E5%90%8C%E7%AD%89%E3%81%AE%E3%82%82%E3%81%AE%E3%81%AF%E4%BD%95%E3%81%A7%E3%81%99%E3%81%8B%EF%BC%9F/940072155/
    char buf = 0;
    struct termios old = {0};
    fflush(stdout);
    if(tcgetattr(0, &old) < 0)
        perror("tcsetattr()");
    old.c_lflag &= ~ICANON;
    old.c_lflag &= ~ECHO;
    old.c_cc[VMIN] = 1;
    old.c_cc[VTIME] = 0;
    if(tcsetattr(0, TCSANOW, &old) < 0)
        perror("tcsetattr ICANON");
    if(read(0, &buf, 1) < 0)
        perror("read()");
    old.c_lflag |= ICANON;
    old.c_lflag |= ECHO;
    if(tcsetattr(0, TCSADRAIN, &old) < 0)
        perror("tcsetattr ~ICANON");
    return buf;
#endif
}

 void t_sleep(unsigned int msec)
{
#ifdef _WIN32
	Sleep(msec);
#endif
#ifdef __linux
	usleep(msec*1000);
#endif
}

/** ファイルサイズを得る
 @return サイズ
*/
uint64_t t_GetFileSize(const tstring &targetPath)
{
#ifdef _WIN32
	HANDLE hFile = CreateFile(targetPath.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ,
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			NULL);

	if( hFile == INVALID_HANDLE_VALUE )
		return 0;

	DWORD sizeLo, sizeHi;
	sizeLo = GetFileSize(hFile, &sizeHi);
	CloseHandle(hFile);

	if( sizeLo == 0xFFFFFFFF && GetLastError() != NO_ERROR )
		return 0;

	uint64_t size = static_cast<uint64_t>(sizeHi) << 32 | static_cast<uint64_t>(sizeLo);
	return size;
#endif /* _WIN32 */

#ifdef __linux
	std::string fpath;
	t_ToNarrow(targetPath, &fpath);

	struct stat fileInfo;
	if( stat(fpath.c_str(), &fileInfo) != 0 )
		return 0;

	uint64_t size = static_cast<uint64_t>(fileInfo.st_size);
	return size;
#endif /* __linux */
}

bool t_ReadFile(const tstring &targetPath, std::vector<uint8_t> **pp)
{
	assert( !targetPath.empty() );
	size_t fsize = static_cast<size_t>(t_GetFileSize(targetPath));
	if( fsize == 0 )
		return false;
	*pp = GCC_NEW std::vector<uint8_t>(fsize);

#ifdef __linux
	std::string fpath;
	t_ToNarrow(targetPath, &fpath);
	FILE *pFile = ::fopen(fpath.c_str(), "rb");
	if( pFile == nullptr )
		return false;
#else
	FILE *pFile = ::_tfsopen(targetPath.c_str(), _T("rb"), _SH_DENYNO );
	if( pFile == nullptr )
		return false;
#endif

	fread((*pp)->data(), fsize, 1, pFile);
	fclose(pFile);

	return true;
}
