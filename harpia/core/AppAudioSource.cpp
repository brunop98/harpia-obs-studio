#include "AppAudioSource.hpp"

#include "PcAudioApps.hpp"

#include <obs.h>

#if defined(_WIN32)

#include "platform/ProcessLoopback.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <audioclient.h>

#include <util/base.h>
#include <util/platform.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace harpia {

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out: ksmedia.h only defines it
// under INITGUID.
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr REFERENCE_TIME kBuffer100ns = 5 * 10000000; // five seconds, as OBS's own
constexpr uint64_t kLookEveryNs = 1000000000ull;      // how often the app is looked for

std::wstring widen(const std::string &utf8)
{
	if (utf8.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
	std::wstring out(size_t(n > 0 ? n : 0), L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), &out[0], n);
	return out;
}

// One process-loopback stream, running.
struct Stream {
	IAudioClient *client = nullptr;
	IAudioCaptureClient *capture = nullptr;

	Stream() = default;
	Stream(const Stream &) = delete;
	Stream &operator=(const Stream &) = delete;
	~Stream() { close(); }

	void swap(Stream &o)
	{
		std::swap(client, o.client);
		std::swap(capture, o.capture);
	}
	void close()
	{
		if (client)
			client->Stop();
		if (capture)
			capture->Release();
		if (client)
			client->Release();
		capture = nullptr;
		client = nullptr;
	}

	// Float stereo at the mixer's rate: what libobs mixes in, so nothing is
	// converted twice. The same format OBS's own process capture asks for.
	bool open(DWORD pid, process_loopback::Mode mode, uint32_t rate, HANDLE ready, HRESULT *hr)
	{
		close();
		long activated = S_OK;
		client = process_loopback::activate(pid, mode, &activated);
		*hr = HRESULT(activated);
		if (!client)
			return false;
		WAVEFORMATEXTENSIBLE wfx = {};
		wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wfx.Format.nChannels = 2;
		wfx.Format.nSamplesPerSec = rate;
		wfx.Format.wBitsPerSample = 32;
		wfx.Format.nBlockAlign = 8;
		wfx.Format.nAvgBytesPerSec = rate * 8;
		wfx.Format.cbSize = WORD(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
		wfx.Samples.wValidBitsPerSample = 32;
		wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
		wfx.SubFormat = kSubtypeFloat;
		*hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
					 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBuffer100ns, 0,
					 &wfx.Format, nullptr);
		if (SUCCEEDED(*hr))
			*hr = client->SetEventHandle(ready);
		if (SUCCEEDED(*hr))
			*hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
		if (SUCCEEDED(*hr))
			*hr = client->Start();
		if (FAILED(*hr)) {
			close();
			return false;
		}
		return true;
	}
};

struct AppAudio {
	obs_source_t *source = nullptr;
	std::thread thread;
	HANDLE wake = nullptr; // the settings changed, or it is time to stop
	std::atomic<bool> quit{false};

	std::mutex mutex; // guards the three below
	std::wstring exe;
	std::string exeUtf8; // for the log
	bool exclude = false;
	bool changed = true;

	void update(obs_data_t *settings)
	{
		const std::string name = normalizeAppExe(obs_data_get_string(settings, "exe"));
		{
			std::lock_guard<std::mutex> lock(mutex);
			exe = widen(name);
			exeUtf8 = name;
			exclude = obs_data_get_bool(settings, "exclude");
			changed = true;
		}
		SetEvent(wake);
	}

	void run();
};

void AppAudio::run()
{
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	obs_audio_info oai = {};
	const uint32_t rate = obs_get_audio_info(&oai) && oai.samples_per_sec > 0 ? oai.samples_per_sec : 48000;
	const DWORD self = GetCurrentProcessId();

	Stream stream;
	DWORD target = 0; // the process the stream is about; 0 = none
	std::wstring app;
	std::string appUtf8;
	bool leaveOut = false;
	uint64_t nextLook = 0;
	bool warned = false;
	std::vector<float> silence;

	while (!quit.load()) {
		bool changedNow = false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (changed) {
				app = exe;
				appUtf8 = exeUtf8;
				leaveOut = exclude;
				changed = false;
				changedNow = true;
			}
		}
		const uint64_t now = os_gettime_ns();
		if (changedNow || now >= nextLook) {
			nextLook = now + kLookEveryNs;
			DWORD want = app.empty() ? 0 : DWORD(process_loopback::rootProcessOf(app));
			// An app to leave out that is not running: leave out Harpia instead,
			// which plays nothing of its own while it records -- everything.
			if (leaveOut && want == 0)
				want = self;
			if (changedNow || want != target) {
				if (want == 0) {
					stream.close();
					target = 0;
				} else {
					// The new stream first, then the old one goes: switching
					// from "everything" to "everything but the player that just
					// started" leaves no hole in the sound.
					Stream next;
					HRESULT hr = S_OK;
					if (next.open(want, leaveOut ? process_loopback::Mode::ExcludeTree
								     : process_loopback::Mode::IncludeTree,
						      rate, ready, &hr)) {
						stream.swap(next);
						target = want;
						warned = false;
					} else {
						stream.close();
						target = 0;
						if (!warned)
							blog(LOG_WARNING, "[harpia] app audio: could not %s %s (0x%08lX)",
							     leaveOut ? "leave out" : "record", appUtf8.c_str(),
							     (unsigned long)hr);
						warned = true; // tried again at the next look, quietly
					}
				}
			}
		}

		if (!stream.capture) {
			WaitForSingleObject(wake, 250);
			continue;
		}
		const HANDLE waits[2] = {ready, wake};
		WaitForMultipleObjects(2, waits, FALSE, 100);

		HRESULT hr = S_OK;
		UINT32 packet = 0;
		while (SUCCEEDED(hr = stream.capture->GetNextPacketSize(&packet)) && packet > 0) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			UINT64 qpc = 0;
			hr = stream.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
			if (FAILED(hr))
				break;
			if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
				if (silence.size() < size_t(frames) * 2)
					silence.resize(size_t(frames) * 2, 0.0f);
				data = reinterpret_cast<BYTE *>(silence.data());
			}
			obs_source_audio audio = {};
			audio.data[0] = data;
			audio.frames = frames;
			audio.speakers = SPEAKERS_STEREO;
			audio.format = AUDIO_FORMAT_FLOAT;
			audio.samples_per_sec = rate;
			// The packet's own capture time, in the clock libobs keeps (QPC,
			// in 100 ns units); the arrival time when Windows had none.
			audio.timestamp = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 && qpc != 0
						  ? qpc * 100
						  : os_gettime_ns() - util_mul_div64(frames, 1000000000ull, rate);
			obs_source_output_audio(source, &audio);
			stream.capture->ReleaseBuffer(frames);
		}
		if (FAILED(hr)) {
			// The output device changed, or the app's audio went away: start
			// over at the next look.
			blog(LOG_INFO, "[harpia] app audio: the stream for %s ended (0x%08lX); reconnecting",
			     appUtf8.c_str(), (unsigned long)hr);
			stream.close();
			target = 0;
		}
	}
	stream.close();
	if (ready)
		CloseHandle(ready);
	if (SUCCEEDED(com))
		CoUninitialize();
}

} // namespace

bool app_audio::registerSource()
{
	static bool registered = false;
	if (registered)
		return true;
	if (!process_loopback::available())
		return false;
	obs_source_info info = {};
	info.id = kAppAudioSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT;
	info.get_name = [](void *) -> const char * { return "Harpia app audio"; };
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_string(settings, "exe", "");
		obs_data_set_default_bool(settings, "exclude", false);
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *a = new AppAudio;
		a->source = source;
		a->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		a->update(settings);
		a->thread = std::thread([a]() { a->run(); });
		return a;
	};
	info.destroy = [](void *data) {
		auto *a = static_cast<AppAudio *>(data);
		a->quit.store(true);
		SetEvent(a->wake);
		if (a->thread.joinable())
			a->thread.join();
		CloseHandle(a->wake);
		delete a;
	};
	info.update = [](void *data, obs_data_t *settings) { static_cast<AppAudio *>(data)->update(settings); };
	obs_register_source(&info);
	registered = true;
	return true;
}

} // namespace harpia

#else // not Windows

namespace harpia {

bool app_audio::registerSource()
{
	return false;
}

} // namespace harpia

#endif
