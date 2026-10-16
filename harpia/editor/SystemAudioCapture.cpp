#include "SystemAudioCapture.hpp"

#include <QFileInfo>
#include <QOperatingSystemVersion>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#endif

namespace harpia {

namespace {
const QString kPrefix = QStringLiteral("harpia:system");
const QString kAppPrefix = QStringLiteral("harpia:system:app:");
} // namespace

QString system_audio::sourceId(const QString &appExe)
{
	return appExe.isEmpty() ? kPrefix : kAppPrefix + appExe;
}

bool system_audio::isSourceId(const QString &id, QString *appExe)
{
	if (id == kPrefix) {
		if (appExe)
			appExe->clear();
		return true;
	}
	if (id.startsWith(kAppPrefix) && id.size() > kAppPrefix.size()) {
		if (appExe)
			*appExe = id.mid(kAppPrefix.size());
		return true;
	}
	return false;
}

struct SystemAudioCapture::Impl {
	std::thread thread;
	std::atomic<bool> quit{false};
	bool stopped = false;

	// The start handshake: the thread reports once the stream runs or fails.
	std::mutex startMutex;
	std::condition_variable startCv;
	bool startDone = false;
	bool startOk = false;
	QString startError;

	int rate = 48000;
	bool excludesSelf = false;

	mutable std::mutex bufMutex;
	QByteArray buf;
	QString failure;

	void reportStart(bool ok, const QString &err)
	{
		std::lock_guard<std::mutex> lock(startMutex);
		startDone = true;
		startOk = ok;
		startError = err;
		startCv.notify_all();
	}
	// 16-bit stereo, straight into the buffer: the lock covers only the copy.
	void append(const char *pcm, qsizetype bytes)
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		buf.append(pcm, bytes);
	}
	void appendSilence(qint64 frames)
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		buf.append(qsizetype(frames) * 4, '\0');
	}
	void fail(const QString &why)
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		if (failure.isEmpty())
			failure = why;
	}

#if defined(_WIN32)
	void run(DWORD pid, bool appOnly);
#endif
};

#if defined(_WIN32)

namespace {

// ---- process loopback, declared here rather than taken from the SDK --------
// audioclientactivationparams.h only ships with SDK 10.0.20348 and newer; the
// layout is fixed ABI, so a copy keeps the build working on an older SDK.
enum HarpiaActivationType { kActivationDefault = 0, kActivationProcessLoopback = 1 };
enum HarpiaLoopbackMode { kIncludeTargetTree = 0, kExcludeTargetTree = 1 };
struct HarpiaLoopbackParams {
	DWORD targetProcessId;
	HarpiaLoopbackMode mode;
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

IAudioClient *activateProcessLoopback(DWORD pid, HarpiaLoopbackMode mode, HRESULT *hr)
{
	static const ActivateAsyncFn activate = []() -> ActivateAsyncFn {
		HMODULE mm = LoadLibraryW(L"Mmdevapi.dll");
		return mm ? reinterpret_cast<ActivateAsyncFn>(
				    reinterpret_cast<void *>(GetProcAddress(mm, "ActivateAudioInterfaceAsync")))
			  : nullptr;
	}();
	if (!activate) {
		*hr = E_NOTIMPL;
		return nullptr;
	}
	HarpiaActivationParams params = {};
	params.type = kActivationProcessLoopback;
	params.loopback.targetProcessId = pid;
	params.loopback.mode = mode;
	PROPVARIANT pv;
	PropVariantInit(&pv);
	pv.vt = VT_BLOB;
	pv.blob.cbSize = ULONG(sizeof(params));
	pv.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

	auto *handler = new ActivationHandler;
	IActivateAudioInterfaceAsyncOperation *op = nullptr;
	*hr = activate(kProcessLoopbackDevice, __uuidof(IAudioClient), &pv, handler, &op);
	IAudioClient *client = nullptr;
	if (SUCCEEDED(*hr))
		client = handler->wait(5000, hr);
	if (op)
		op->Release();
	handler->Release();
	return client;
}

QString exePathOf(DWORD pid)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h)
		return QString();
	wchar_t buf[MAX_PATH * 2];
	DWORD n = DWORD(std::size(buf));
	QString out;
	if (QueryFullProcessImageNameW(h, 0, buf, &n))
		out = QString::fromWCharArray(buf, int(n));
	CloseHandle(h);
	return out;
}

// "Google Chrome" from chrome.exe's version resource; the file name without
// ".exe" when it has none.
QString friendlyName(const QString &exePath)
{
	const QString fallback = QFileInfo(exePath).completeBaseName();
	const std::wstring path = exePath.toStdWString();
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (size == 0)
		return fallback;
	QByteArray data(int(size), Qt::Uninitialized);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
		return fallback;
	struct LangCp {
		WORD lang;
		WORD cp;
	} *tr = nullptr;
	UINT trLen = 0;
	if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&tr), &trLen) ||
	    trLen < sizeof(LangCp))
		return fallback;
	wchar_t key[64];
	swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
	wchar_t *desc = nullptr;
	UINT descLen = 0;
	if (!VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&desc), &descLen) || !desc || descLen <= 1)
		return fallback;
	const QString name = QString::fromWCharArray(desc).trimmed();
	return name.isEmpty() ? fallback : name;
}

struct ProcEntry {
	DWORD pid;
	DWORD parent;
	QString exe; // lower case
};

QVector<ProcEntry> processes()
{
	QVector<ProcEntry> out;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return out;
	PROCESSENTRY32W e;
	e.dwSize = DWORD(sizeof(e));
	if (Process32FirstW(snap, &e)) {
		do {
			out.append({e.th32ProcessID, e.th32ParentProcessID, QString::fromWCharArray(e.szExeFile).toLower()});
		} while (Process32NextW(snap, &e));
	}
	CloseHandle(snap);
	return out;
}

// The top of `exe`'s own process tree: a browser's main process, whose tree
// holds the audio service that actually plays the sound. 0 when not running.
DWORD rootProcessOf(const QString &exe)
{
	const QString want = exe.toLower();
	const QVector<ProcEntry> all = processes();
	const auto find = [&all](DWORD pid) -> const ProcEntry * {
		for (const ProcEntry &p : all)
			if (p.pid == pid)
				return &p;
		return nullptr;
	};
	for (const ProcEntry &p : all) {
		if (p.exe != want)
			continue;
		const ProcEntry *at = &p;
		for (int guard = 0; guard < 64; ++guard) {
			const ProcEntry *up = find(at->parent);
			if (!up || up->exe != want || up->pid == at->pid)
				break;
			at = up;
		}
		return at->pid;
	}
	return 0;
}

// COM for the calling thread, released only if this call was the one that
// started it (the GUI thread already has it, single-threaded).
struct ComScope {
	HRESULT hr;
	explicit ComScope(DWORD model) : hr(CoInitializeEx(nullptr, model)) {}
	~ComScope()
	{
		if (SUCCEEDED(hr))
			CoUninitialize();
	}
};

template <class T> void releaseCom(T *&p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

// What the captured samples look like.
struct InFormat {
	int channels = 2;
	int bits = 16;
	bool isFloat = false;
	int blockAlign = 4;
	int rate = 48000;

	// Already what the take holds: copied as it comes.
	bool isStereo16() const { return !isFloat && bits == 16 && channels == 2 && blockAlign == 4; }
	// The usual mix format.
	bool isStereoFloat() const { return isFloat && bits == 32 && channels == 2 && blockAlign == 8; }
};

InFormat formatOf(const WAVEFORMATEX *f)
{
	InFormat in;
	in.channels = std::max<int>(1, f->nChannels);
	in.bits = f->wBitsPerSample;
	in.blockAlign = f->nBlockAlign;
	in.rate = int(f->nSamplesPerSec);
	WORD tag = f->wFormatTag;
	if (tag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
		const auto *x = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
		// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT share one base GUID; the
		// first field is the plain format tag.
		tag = WORD(x->SubFormat.Data1);
	}
	in.isFloat = tag == WAVE_FORMAT_IEEE_FLOAT;
	return in;
}

double sampleAt(const BYTE *frame, const InFormat &f, int ch)
{
	if (f.isFloat && f.bits == 32)
		return double(reinterpret_cast<const float *>(frame)[ch]);
	if (f.isFloat && f.bits == 64)
		return reinterpret_cast<const double *>(frame)[ch];
	if (f.bits == 16)
		return reinterpret_cast<const qint16 *>(frame)[ch] / 32768.0;
	if (f.bits == 32)
		return reinterpret_cast<const qint32 *>(frame)[ch] / 2147483648.0;
	if (f.bits == 24) {
		const BYTE *p = frame + ch * 3;
		const qint32 v = qint32((quint32(p[0]) << 8) | (quint32(p[1]) << 16) | (quint32(p[2]) << 24)) >> 8;
		return v / 8388608.0;
	}
	if (f.bits == 8)
		return (int(frame[ch]) - 128) / 128.0;
	return 0.0;
}

qint16 toInt16(double v)
{
	return qint16(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
}

// Any capture format -> 16-bit stereo, `frames` * 2 samples into `dst`.
// Surround is folded down the usual way (centre and the rear pair into both
// sides, the LFE left out).
void toStereo16(const BYTE *data, UINT32 frames, const InFormat &f, qint16 *dst)
{
	if (f.isStereoFloat()) {
		// The usual mix format: one loop, no format checks per sample.
		const auto *src = reinterpret_cast<const float *>(data);
		const size_t n = size_t(frames) * 2;
		for (size_t i = 0; i < n; ++i)
			dst[i] = toInt16(double(src[i]));
		return;
	}
	for (UINT32 i = 0; i < frames; ++i) {
		const BYTE *fr = data + size_t(i) * size_t(f.blockAlign);
		double l = sampleAt(fr, f, 0);
		double r = f.channels >= 2 ? sampleAt(fr, f, 1) : l;
		if (f.channels >= 3) {
			const double c = sampleAt(fr, f, 2) * 0.7071;
			l += c;
			r += c;
		}
		if (f.channels >= 6) {
			l += sampleAt(fr, f, 4) * 0.5;
			r += sampleAt(fr, f, 5) * 0.5;
		}
		dst[2 * i] = toInt16(l);
		dst[2 * i + 1] = toInt16(r);
	}
}

QString hrText(HRESULT hr)
{
	return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

} // namespace

void SystemAudioCapture::Impl::run(DWORD pid, bool appOnly)
{
	ComScope com(COINIT_MULTITHREADED);
	IAudioClient *client = nullptr;
	IAudioCaptureClient *capture = nullptr;
	HANDLE ready = nullptr;
	InFormat fmt;
	HRESULT hr = S_OK;

	// Process loopback first: one app, or everything but Harpia.
	if (appOnly || system_audio::perAppAvailable()) {
		client = activateProcessLoopback(appOnly ? pid : GetCurrentProcessId(),
						 appOnly ? kIncludeTargetTree : kExcludeTargetTree, &hr);
		if (client) {
			WAVEFORMATEX want = {};
			want.wFormatTag = WAVE_FORMAT_PCM;
			want.nChannels = 2;
			want.nSamplesPerSec = 48000;
			want.wBitsPerSample = 16;
			want.nBlockAlign = 4;
			want.nAvgBytesPerSec = 48000 * 4;
			ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
						AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
							AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
						2000000, 0, &want, nullptr);
			if (SUCCEEDED(hr))
				hr = client->SetEventHandle(ready);
			if (SUCCEEDED(hr)) {
				fmt = formatOf(&want);
				excludesSelf = true;
			} else {
				releaseCom(client);
				CloseHandle(ready);
				ready = nullptr;
			}
		}
		if (!client && appOnly) {
			reportStart(false, QStringLiteral("Windows would not record that app on its own (%1).").arg(hrText(hr)));
			return;
		}
	}

	// The whole output mix of the default device.
	if (!client) {
		IMMDeviceEnumerator *en = nullptr;
		IMMDevice *dev = nullptr;
		WAVEFORMATEX *mix = nullptr;
		hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				      reinterpret_cast<void **>(&en));
		if (SUCCEEDED(hr))
			hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
		if (SUCCEEDED(hr))
			hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
		if (SUCCEEDED(hr))
			hr = client->GetMixFormat(&mix);
		if (SUCCEEDED(hr))
			hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix,
						nullptr);
		if (SUCCEEDED(hr))
			fmt = formatOf(mix);
		if (mix)
			CoTaskMemFree(mix);
		releaseCom(dev);
		releaseCom(en);
		if (FAILED(hr)) {
			releaseCom(client);
			reportStart(false, hr == HRESULT(0x80070490) // E_NOTFOUND: no output device
						   ? QStringLiteral("There is no sound output device to record from.")
						   : QStringLiteral("Windows would not record its sound (%1).").arg(hrText(hr)));
			return;
		}
		excludesSelf = false;
	}

	hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
	if (SUCCEEDED(hr))
		hr = client->Start();
	if (FAILED(hr)) {
		releaseCom(capture);
		releaseCom(client);
		if (ready)
			CloseHandle(ready);
		reportStart(false, QStringLiteral("Windows would not start recording its sound (%1).").arg(hrText(hr)));
		return;
	}
	rate = fmt.rate;
	reportStart(true, QString());

	using clock = std::chrono::steady_clock;
	const clock::time_point t0 = clock::now();
	qint64 written = 0; // frames handed over, real or padded
	// Converted packets land here first: one buffer for the whole take, grown
	// to the largest packet, rather than a new one for every packet.
	QByteArray scratch;
	while (!quit.load()) {
		if (ready)
			WaitForSingleObject(ready, 40);
		else
			Sleep(10);
		UINT32 packet = 0;
		while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
			if (FAILED(hr))
				break;
			if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
				appendSilence(frames);
			} else if (fmt.isStereo16()) {
				append(reinterpret_cast<const char *>(data), qsizetype(frames) * 4);
			} else {
				scratch.resize(qsizetype(frames) * 4);
				toStereo16(data, frames, fmt, reinterpret_cast<qint16 *>(scratch.data()));
				append(scratch.constData(), scratch.size());
			}
			written += frames;
			capture->ReleaseBuffer(frames);
		}
		if (FAILED(hr)) {
			fail(hr == AUDCLNT_E_DEVICE_INVALIDATED
				     ? QStringLiteral("The sound output changed (unplugged or switched), so the recording "
						      "stopped there.")
				     : QStringLiteral("Recording Windows audio stopped (%1).").arg(hrText(hr)));
			break;
		}
		// Nothing playing sends nothing at all; the take still has to be as
		// long as it took, so the clock fills the gap with silence. Only past
		// a clear gap, so an ordinary late packet is never doubled.
		const qint64 due = qint64(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count()) *
				   fmt.rate / 1000000;
		if (due - written > fmt.rate * 15 / 100) {
			const qint64 pad = due - written - fmt.rate * 3 / 100;
			appendSilence(pad);
			written += pad;
		}
	}
	client->Stop();
	releaseCom(capture);
	releaseCom(client);
	if (ready)
		CloseHandle(ready);
}

bool system_audio::available()
{
	return true;
}

bool system_audio::perAppAvailable()
{
	// Process loopback arrived in build 20348 (Windows 11 and Server 2022).
	const QOperatingSystemVersion v = QOperatingSystemVersion::current();
	return v.type() == QOperatingSystemVersion::Windows && v.majorVersion() >= 10 && v.microVersion() >= 20348;
}

QVector<SystemAudioApp> system_audio::apps()
{
	QVector<SystemAudioApp> out;
	ComScope com(COINIT_APARTMENTTHREADED);
	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	IAudioSessionManager2 *mgr = nullptr;
	IAudioSessionEnumerator *sessions = nullptr;
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
				      reinterpret_cast<void **>(&en));
	if (SUCCEEDED(hr))
		hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
	if (SUCCEEDED(hr))
		hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&mgr));
	if (SUCCEEDED(hr))
		hr = mgr->GetSessionEnumerator(&sessions);
	int count = 0;
	if (SUCCEEDED(hr))
		hr = sessions->GetCount(&count);
	const DWORD self = GetCurrentProcessId();
	const QString selfExe = QFileInfo(exePathOf(self)).fileName().toLower();
	for (int i = 0; SUCCEEDED(hr) && i < count; ++i) {
		IAudioSessionControl *ctl = nullptr;
		IAudioSessionControl2 *ctl2 = nullptr;
		if (FAILED(sessions->GetSession(i, &ctl)) || !ctl)
			continue;
		if (SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void **>(&ctl2)))) {
			DWORD pid = 0;
			AudioSessionState state = AudioSessionStateInactive;
			ctl2->GetState(&state);
			if (ctl2->IsSystemSoundsSession() != S_OK && SUCCEEDED(ctl2->GetProcessId(&pid)) && pid != 0 &&
			    pid != self && state != AudioSessionStateExpired) {
				const QString path = exePathOf(pid);
				const QString exe = QFileInfo(path).fileName();
				if (!exe.isEmpty() && exe.toLower() != selfExe) {
					bool merged = false;
					for (SystemAudioApp &a : out)
						if (a.exe.compare(exe, Qt::CaseInsensitive) == 0) {
							a.playing = a.playing || state == AudioSessionStateActive;
							merged = true;
						}
					if (!merged)
						out.append({exe, friendlyName(path), state == AudioSessionStateActive});
				}
			}
			releaseCom(ctl2);
		}
		releaseCom(ctl);
	}
	releaseCom(sessions);
	releaseCom(mgr);
	releaseCom(dev);
	releaseCom(en);
	std::stable_sort(out.begin(), out.end(), [](const SystemAudioApp &a, const SystemAudioApp &b) {
		if (a.playing != b.playing)
			return a.playing;
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return out;
}

#else // not Windows

bool system_audio::available()
{
	return false;
}

bool system_audio::perAppAvailable()
{
	return false;
}

QVector<SystemAudioApp> system_audio::apps()
{
	return {};
}

#endif

SystemAudioCapture::SystemAudioCapture() = default;

SystemAudioCapture::~SystemAudioCapture()
{
	stop();
}

bool SystemAudioCapture::start(const QString &appExe, QString *error)
{
	stop();
	d_.reset();
	QString err;
#if defined(_WIN32)
	DWORD pid = 0;
	if (!appExe.isEmpty()) {
		if (!system_audio::perAppAvailable())
			err = QStringLiteral("Recording one app on its own needs Windows 11. Choose \"Windows audio "
					     "(everything)\" instead.");
		else if ((pid = rootProcessOf(appExe)) == 0)
			err = QStringLiteral("%1 isn't running. Start it (and the video) first.").arg(appExe);
	}
	if (err.isEmpty()) {
		d_ = std::make_unique<Impl>();
		Impl *d = d_.get();
		const bool appOnly = !appExe.isEmpty();
		d->thread = std::thread([d, pid, appOnly]() { d->run(pid, appOnly); });
		{
			std::unique_lock<std::mutex> lock(d->startMutex);
			d->startCv.wait_for(lock, std::chrono::seconds(8), [d]() { return d->startDone; });
			if (!d->startDone)
				err = QStringLiteral("Windows took too long to start recording its sound.");
			else if (!d->startOk)
				err = d->startError;
		}
		if (!err.isEmpty()) {
			stop();
			d_.reset();
		}
	}
#else
	Q_UNUSED(appExe);
	err = QStringLiteral("Recording the computer's sound is only available on Windows.");
#endif
	if (error)
		*error = err;
	return err.isEmpty();
}

void SystemAudioCapture::stop()
{
	// The thread goes; what it captured stays for take() until the next start.
	if (!d_)
		return;
	d_->quit.store(true);
	if (d_->thread.joinable())
		d_->thread.join();
	d_->stopped = true;
}

bool SystemAudioCapture::running() const
{
	return d_ && !d_->stopped;
}

int SystemAudioCapture::sampleRate() const
{
	return d_ ? d_->rate : 48000;
}

bool SystemAudioCapture::excludesSelf() const
{
	return d_ && d_->excludesSelf;
}


QByteArray SystemAudioCapture::take()
{
	if (!d_)
		return QByteArray();
	std::lock_guard<std::mutex> lock(d_->bufMutex);
	QByteArray out;
	out.swap(d_->buf);
	return out;
}

QString SystemAudioCapture::failure() const
{
	if (!d_)
		return QString();
	std::lock_guard<std::mutex> lock(d_->bufMutex);
	return d_->failure;
}

} // namespace harpia
