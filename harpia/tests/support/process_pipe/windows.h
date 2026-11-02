#pragma once
// Controlled Win32 pipe responses, compiling the production C implementation
// on Linux. A non-ready ReadFile is deliberately observable.
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
typedef struct { DWORD cb, dwFlags; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOW;
#define STARTF_USESTDHANDLES 1
#define STARTF_FORCEOFFFEEDBACK 128
#define CREATE_NO_WINDOW 0x08000000
#define HANDLE_FLAG_INHERIT 1
#define INFINITE UINT32_MAX
#define MAXDWORD UINT32_MAX
extern DWORD fake_available, fake_chunk, fake_error;
extern int fake_reads, fake_writes, fake_failure_at, fake_zero_at, fake_logs;
extern DWORD fake_first_request;
static inline BOOL CreatePipe(HANDLE *a, HANDLE *b, SECURITY_ATTRIBUTES *s, DWORD n) { (void)a;(void)b;(void)s;(void)n;return 0; }
static inline BOOL CreateProcessW(const wchar_t *a, wchar_t *b, void *c, void *d, BOOL e, DWORD f, void *g, const wchar_t *h, STARTUPINFOW *i, PROCESS_INFORMATION *j)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;return 0; }
static inline BOOL CloseHandle(HANDLE h) { (void)h;return 1; }
static inline BOOL SetHandleInformation(HANDLE h, DWORD a, DWORD b) { (void)h;(void)a;(void)b;return 1; }
static inline DWORD WaitForSingleObject(HANDLE h, DWORD d) { (void)h;(void)d;return 0; }
static inline BOOL GetExitCodeProcess(HANDLE h, DWORD *c) { (void)h;*c=0;return 1; }
static inline DWORD GetLastError(void) { return fake_error; }
static inline BOOL PeekNamedPipe(HANDLE h, void *d, DWORD n, DWORD *r, DWORD *available, DWORD *left)
{ (void)h;(void)d;(void)n;(void)r;(void)left;*available=fake_available;return 1; }
static inline BOOL ReadFile(HANDLE h, void *d, DWORD n, DWORD *read, void *o)
{ (void)h;(void)d;(void)o;++fake_reads;*read=n < fake_available ? n : fake_available;fake_available-=*read;return 1; }
static inline BOOL WriteFile(HANDLE h, const void *d, DWORD n, DWORD *written, void *o)
{
	(void)h;(void)d;(void)o;++fake_writes;
	if (fake_writes == 1) fake_first_request=n;
	if (fake_writes == fake_failure_at) return 0;
	*written = fake_writes == fake_zero_at ? 0 : (n < fake_chunk ? n : fake_chunk);
	return 1;
}
