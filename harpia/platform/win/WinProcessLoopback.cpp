#include "platform/ProcessLoopback.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>

#include <util/windows/win-version.h>

#include <unordered_map>
#include <vector>

namespace harpia {

namespace {

// ---- declared here rather than taken from the SDK ----------------------------
// audioclientactivationparams.h only ships with SDK 10.0.20348 and newer; the
// layout is fixed ABI, so a copy keeps the build working on an older SDK.
enum HarpiaActivationType { kActivationDefault = 0, kActivationProcessLoopback = 1 };
struct HarpiaLoopbackParams {
	DWORD targetProcessId;
	DWORD mode; // process_loopback::Mode
};
struct HarpiaActivationParams {
	HarpiaActivationType type;
	HarpiaLoopbackParams loopback;
};
const wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback";
// IAgileObject: the completion handler must be callable from any thread.
const IID kIidAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

using ActivateAsyncFn = HRESULT(WINAPI *)(LPCWSTR, REFIID, PROPVARIANT *,
					  IActivateAudioInterfaceCompletionHandler *,
					  IActivateAudioInterfaceAsyncOperation **);

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
	ActivationHandler() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

	ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG n = ULONG(InterlockedDecrement(&refs_));
		if (n == 0)
			delete this;
		return n;
	}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (!out)
			return E_POINTER;
		if (IsEqualIID(riid, __uuidof(IUnknown)) ||
		    IsEqualIID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler)) ||
		    IsEqualIID(riid, kIidAgileObject)) {
			*out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
			AddRef();
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override
	{
		HRESULT activated = E_FAIL;
		IUnknown *unk = nullptr;
		const HRESULT r = op->GetActivateResult(&activated, &unk);
		if (SUCCEEDED(r) && SUCCEEDED(activated) && unk)
			result_ = unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client_));
		else
			result_ = FAILED(r) ? r : activated;
		if (unk)
			unk->Release();
		SetEvent(done_);
		return S_OK;
	}

	// The activated client (the caller owns the reference), or null.
	IAudioClient *wait(DWORD ms, HRESULT *hr)
	{
		if (WaitForSingleObject(done_, ms) != WAIT_OBJECT_0) {
			*hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
			return nullptr;
		}
		*hr = result_;
		IAudioClient *c = client_;
		client_ = nullptr;
		return c;
	}

private:
	~ActivationHandler()
	{
		if (client_)
			client_->Release();
		if (done_)
			CloseHandle(done_);
	}
	LONG refs_ = 1;
	HANDLE done_ = nullptr;
	HRESULT result_ = E_FAIL;
	IAudioClient *client_ = nullptr;
};

struct ProcEntry {
	DWORD pid;
	DWORD parent;
	bool match; // runs the program asked about
};

bool sameName(const wchar_t *a, const std::wstring &b)
{
	return CompareStringOrdinal(a, -1, b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

bool process_loopback::available()
{
	static const bool has = []() {
		win_version_info ver = {};
		get_win_ver(&ver);
		win_version_info minimum = {};
		minimum.major = 10;
		minimum.minor = 0;
		minimum.build = 19041;
		return win_version_compare(&ver, &minimum) >= 0;
	}();
	return has;
}

IAudioClient *process_loopback::activate(unsigned long pid, Mode mode, long *hr)
{
	static const ActivateAsyncFn activateAsync = []() -> ActivateAsyncFn {
		HMODULE mm = LoadLibraryW(L"Mmdevapi.dll");
		return mm ? reinterpret_cast<ActivateAsyncFn>(
				    reinterpret_cast<void *>(GetProcAddress(mm, "ActivateAudioInterfaceAsync")))
			  : nullptr;
	}();
	HRESULT result = E_NOTIMPL;
	IAudioClient *client = nullptr;
	if (activateAsync) {
		HarpiaActivationParams params = {};
		params.type = kActivationProcessLoopback;
		params.loopback.targetProcessId = DWORD(pid);
		params.loopback.mode = DWORD(mode);
		PROPVARIANT pv;
		PropVariantInit(&pv);
		pv.vt = VT_BLOB;
		pv.blob.cbSize = ULONG(sizeof(params));
		pv.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

		auto *handler = new ActivationHandler;
		IActivateAudioInterfaceAsyncOperation *op = nullptr;
		result = activateAsync(kProcessLoopbackDevice, __uuidof(IAudioClient), &pv, handler, &op);
		if (SUCCEEDED(result))
			client = handler->wait(5000, &result);
		if (op)
			op->Release();
		handler->Release();
	}
	if (hr)
		*hr = result;
	return client;
}

unsigned long process_loopback::rootProcessOf(const std::wstring &exe)
{
	if (exe.empty())
		return 0;
	std::vector<ProcEntry> all;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return 0;
	PROCESSENTRY32W e;
	e.dwSize = DWORD(sizeof(e));
	if (Process32FirstW(snap, &e)) {
		do {
			all.push_back({e.th32ProcessID, e.th32ParentProcessID, sameName(e.szExeFile, exe)});
		} while (Process32NextW(snap, &e));
	}
	CloseHandle(snap);

	// Climb from the first of its processes while the parent is the same
	// program. By pid, so the climb is a lookup per step, not a scan.
	std::unordered_map<DWORD, size_t> byPid;
	byPid.reserve(all.size());
	for (size_t i = 0; i < all.size(); ++i)
		byPid.emplace(all[i].pid, i);
	for (const ProcEntry &p : all) {
		if (!p.match)
			continue;
		const ProcEntry *at = &p;
		for (int guard = 0; guard < 64; ++guard) {
			const auto up = byPid.find(at->parent);
			if (up == byPid.end() || !all[up->second].match || all[up->second].pid == at->pid)
				break;
			at = &all[up->second];
		}
		return at->pid;
	}
	return 0;
}

} // namespace harpia
