#include "MacSystemAudioCapture.hpp"
#include "MacAudioPcm.hpp"
#include "MacAudioPermissions.hpp"

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace harpia::mac_audio {
namespace {
constexpr qint64 rate = 48000;
constexpr qint64 frameBytes = 4;

QString nativeError(NSError *error, const QString &fallback)
{
	return error && error.localizedDescription.length
		? QString::fromUtf8(error.localizedDescription.UTF8String) : fallback;
}

struct State {
	std::atomic<bool> accepting{false};
	std::atomic<bool> running{false};
	std::atomic<bool> abandoned{false};
	std::atomic<bool> routingPending{false};
	std::atomic<bool> waitingForApp{false};
	std::atomic<bool> streamStopped{false};
	std::atomic<uint64_t> routingRevision{0};
	bool routingBusy = false; // only accessed on routingQueue
	QString bundleId;
	NSArray *appObservers = nil;
	dispatch_queue_t routingQueue = nil;
	std::mutex mutex;
	std::condition_variable changed;
	QByteArray pending;
	QString error;
	qint64 written = 0;
	qint64 finishedFrames = -1;
	bool clockReady = false;
	CMTime startTime = kCMTimeInvalid;
	std::chrono::steady_clock::time_point startClock;
	bool contentDone = false;
	bool startDone = false;
	bool stopDone = false;
	bool startOk = false;
	SCShareableContent *content = nil;
	SCStream *stream = nil;
	id output = nil;
	dispatch_queue_t queue = nil;

	void fail(const QString &why)
	{
		SCStream *toStop = nil;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (error.isEmpty())
				error = why;
			if (finishedFrames < 0)
				finishedFrames = dueFrames();
			running.store(false);
			accepting.store(false);
			toStop = stream;
		}
		if (@available(macOS 13.0, *))
			if (toStop)
				[toStop stopCaptureWithCompletionHandler:^(NSError *) {}];
	}
	qint64 dueFrames() const
	{
		if (finishedFrames >= 0)
			return finishedFrames;
		if (!clockReady)
			return 0;
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - startClock).count();
		return std::max<qint64>(0, elapsed * rate / 1000000);
	}
	void append(const QByteArray &pcm, qint64 at)
	{
		std::unique_lock<std::mutex> lock(mutex);
		if (!accepting.load())
			return;
		qint64 frames = pcm.size() / frameBytes;
		if (finishedFrames >= 0) {
			if (at >= finishedFrames)
				return;
			frames = std::min(frames, finishedFrames - at);
		}
		qint64 skip = std::max<qint64>(0, written - at);
		if (skip >= frames)
			return; // an overlapping/late packet cannot double elapsed silence
		if (at > written) {
			// Reject a broken timestamp rather than allocate an enormous gap.
			if (at - written > rate * 60) {
				lock.unlock();
				fail(QStringLiteral("macOS audio timestamps jumped unexpectedly. Start a new take."));
				return;
			}
			pending.append(qsizetype((at - written) * frameBytes), '\0');
			written = at;
		}
		pending.append(pcm.constData() + skip * frameBytes, qsizetype((frames - skip) * frameBytes));
		written += frames - skip;
	}
};

API_AVAILABLE(macos(13.0))
void scheduleApplicationRefresh(const std::shared_ptr<State> &state);
} // namespace
} // namespace harpia::mac_audio

// The delegate and completion blocks retain shared state. A timed-out start or
// a late native callback can therefore finish safely after Capture was stopped.
API_AVAILABLE(macos(13.0))
@interface HarpiaSystemAudioOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
	std::shared_ptr<harpia::mac_audio::State> state;
}
@end

@implementation HarpiaSystemAudioOutput
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
	(void)stream;
	const auto current = state;
	if (!current || current->abandoned.load())
		return; // an intentional stop must not surface a late native error
	if (current->accepting.load() && !current->bundleId.isEmpty() &&
	    error.code == SCStreamErrorNoCaptureSource) {
		// An app closing is a quiet interval, not permission to capture the
		// entire desktop. A launch notification will rebuild this app filter.
		current->routingPending.store(true);
		current->waitingForApp.store(true);
		current->streamStopped.store(true);
		current->running.store(false);
		harpia::mac_audio::scheduleApplicationRefresh(current);
		return;
	}
	if (current && current->accepting.load())
		current->fail(harpia::mac_audio::nativeError(error,
			QStringLiteral("macOS stopped system audio capture. Check Screen & System Audio Recording permission.")));
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
	(void)stream;
	using namespace harpia::mac_audio;
	const auto current = state;
	if (type != SCStreamOutputTypeAudio || !current || !current->accepting.load() ||
	    current->routingPending.load() ||
	    !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample))
		return;
	const CMItemCount frameCount = CMSampleBufferGetNumSamples(sample);
	if (frameCount <= 0)
		return;
	const AudioStreamBasicDescription *format = CMAudioFormatDescriptionGetStreamBasicDescription(
		CMSampleBufferGetFormatDescription(sample));
	if (!format || format->mFormatID != kAudioFormatLinearPCM ||
	    std::abs(format->mSampleRate - double(rate)) > 0.5 ||
	    format->mChannelsPerFrame < 1 || format->mChannelsPerFrame > 2 ||
	    (format->mFormatFlags & kAudioFormatFlagIsBigEndian)) {
		current->fail(QStringLiteral("macOS returned an unsupported system-audio format (expected 48 kHz mono/stereo PCM)."));
		return;
	}
	PcmSample sampleKind;
	const bool floating = (format->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
	if (floating && format->mBitsPerChannel == 32)
		sampleKind = PcmSample::Float32;
	else if (floating && format->mBitsPerChannel == 64)
		sampleKind = PcmSample::Float64;
	else if (!floating && (format->mFormatFlags & kAudioFormatFlagIsSignedInteger) && format->mBitsPerChannel == 16)
		sampleKind = PcmSample::Signed16;
	else if (!floating && (format->mFormatFlags & kAudioFormatFlagIsSignedInteger) && format->mBitsPerChannel == 32)
		sampleKind = PcmSample::Signed32;
	else {
		current->fail(QStringLiteral("macOS returned unsupported PCM sample packing for system audio."));
		return;
	}
	const bool planar = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
	const size_t stride = bytesPerSample(sampleKind) * (planar ? 1 : format->mChannelsPerFrame);
	if (format->mBytesPerFrame != stride || frameCount > rate * 5) {
		current->fail(QStringLiteral("macOS returned an invalid system-audio packet."));
		return;
	}

	size_t needed = 0;
	CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &needed, nullptr, 0,
		kCFAllocatorDefault, kCFAllocatorDefault, 0, nullptr);
	if (needed < sizeof(AudioBufferList) || needed > 65536) {
		current->fail(QStringLiteral("macOS returned an invalid system-audio buffer list."));
		return;
	}
	std::vector<std::max_align_t> storage((needed + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
	auto *list = reinterpret_cast<AudioBufferList *>(storage.data());
	CMBlockBufferRef block = nullptr;
	const OSStatus result = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr,
		list, needed, kCFAllocatorDefault, kCFAllocatorDefault,
		kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
	if (result != noErr) {
		if (block)
			CFRelease(block);
		current->fail(QStringLiteral("macOS could not read a system-audio sample buffer (%1).").arg(result));
		return;
	}
	if (list->mNumberBuffers < (planar ? format->mChannelsPerFrame : 1) || list->mNumberBuffers > 2) {
		if (block)
			CFRelease(block);
		current->fail(QStringLiteral("macOS returned an invalid system-audio channel layout."));
		return;
	}
	std::vector<PcmPlane> planes;
	planes.reserve(list->mNumberBuffers);
	for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
		planes.push_back({list->mBuffers[i].mData, list->mBuffers[i].mDataByteSize});
	QByteArray packed(qsizetype(frameCount * frameBytes), Qt::Uninitialized);
	const bool converted = packStereo16(planes.data(), planes.size(), size_t(frameCount),
		format->mChannelsPerFrame, planar, sampleKind,
		reinterpret_cast<int16_t *>(packed.data()), size_t(frameCount) * 2);
	if (block)
		CFRelease(block);
	if (!converted) {
		current->fail(QStringLiteral("macOS returned incomplete system-audio channel data."));
		return;
	}
	CMTime pts = CMSampleBufferGetOutputPresentationTimeStamp(sample);
	if (!CMTIME_IS_NUMERIC(pts))
		pts = CMSampleBufferGetPresentationTimeStamp(sample);
	qint64 at = 0;
	{
		std::unique_lock<std::mutex> lock(current->mutex);
		if (CMTIME_IS_NUMERIC(pts) && CMTIME_IS_NUMERIC(current->startTime)) {
			const double seconds = CMTimeGetSeconds(CMTimeSubtract(pts, current->startTime));
			if (!std::isfinite(seconds) || std::abs(seconds) > 86400.0) {
				lock.unlock();
				current->fail(QStringLiteral("macOS returned an invalid system-audio timestamp."));
				return;
			}
			at = qint64(std::llround(seconds * double(rate)));
		} else {
			at = current->written;
		}
	}
	current->append(packed, at);
}
@end

namespace harpia::mac_audio {

bool available()
{
	if (@available(macOS 13.0, *))
		return true;
	return false;
}

bool permissionGranted()
{
	return available() && CGPreflightScreenCaptureAccess();
}

QVector<SystemAudioApp> runningApps()
{
	QVector<SystemAudioApp> apps;
	@autoreleasepool {
		const pid_t ownPid = NSProcessInfo.processInfo.processIdentifier;
		for (NSRunningApplication *application in NSWorkspace.sharedWorkspace.runningApplications) {
			if (application.processIdentifier == ownPid || application.terminated ||
			    application.activationPolicy != NSApplicationActivationPolicyRegular ||
			    application.bundleIdentifier.length == 0)
				continue;
			const QString bundle = QString::fromUtf8(application.bundleIdentifier.UTF8String);
			bool seen = false;
			for (const auto &app : apps)
				seen = seen || app.exe.compare(bundle, Qt::CaseInsensitive) == 0;
			if (!seen) {
				const QString name = QString::fromUtf8(application.localizedName.UTF8String);
				apps.append({bundle, name.isEmpty() ? bundle : name, false, false});
			}
		}
	}
	std::sort(apps.begin(), apps.end(), [](const auto &a, const auto &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return apps;
}

namespace {
bool currentRouting(const std::shared_ptr<State> &state, uint64_t revision)
{
	return !state->abandoned.load() && state->accepting.load() &&
	       state->routingRevision.load() == revision;
}

API_AVAILABLE(macos(13.0))
void refreshApplicationFilter(const std::shared_ptr<State> &state, uint64_t revision);

API_AVAILABLE(macos(13.0))
void finishRouting(const std::shared_ptr<State> &state, uint64_t revision)
{
	state->routingBusy = false;
	if (!state->abandoned.load() && state->accepting.load() && state->routingRevision.load() != revision)
		refreshApplicationFilter(state, state->routingRevision.load());
}

// This queue serializes the complete native operation, including its async
// completion. A newer launch cannot race an older filter into the stream.
API_AVAILABLE(macos(13.0))
void refreshApplicationFilter(const std::shared_ptr<State> &state, uint64_t revision)
{
	if (!currentRouting(state, revision) || state->routingBusy)
		return;
	state->routingBusy = true;
	[SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO
		completionHandler:^(SCShareableContent *content, NSError *error) {
			dispatch_async(state->routingQueue, ^{
				if (!currentRouting(state, revision)) {
					finishRouting(state, revision);
					return;
				}
				if (error) {
					state->fail(nativeError(error, QStringLiteral("macOS could not refresh the application's audio filter.")));
					finishRouting(state, revision);
					return;
				}
				NSMutableArray<SCRunningApplication *> *targets = [[NSMutableArray alloc] init];
				NSString *wanted = [NSString stringWithUTF8String:state->bundleId.toUtf8().constData()];
				for (SCRunningApplication *application in content.applications)
					if (application.bundleIdentifier.length &&
					    [application.bundleIdentifier caseInsensitiveCompare:wanted] == NSOrderedSame)
						[targets addObject:application];
				if (targets.count == 0) {
					bool running = false;
					for (NSRunningApplication *application in NSWorkspace.sharedWorkspace.runningApplications)
						if (!application.terminated && application.bundleIdentifier.length &&
						    [application.bundleIdentifier caseInsensitiveCompare:wanted] == NSOrderedSame)
							running = true;
					state->waitingForApp.store(!running);
					finishRouting(state, revision);
					// Workspace can announce a launch before SCK publishes its
					// capture identity. Retry briefly without ever unmuting.
					if (running)
						dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 5), state->routingQueue, ^{
							if (currentRouting(state, revision))
								refreshApplicationFilter(state, revision);
						});
					return;
				}
				state->waitingForApp.store(false);
				SCDisplay *display = nil;
				for (SCDisplay *candidate in content.displays)
					if (candidate.displayID == CGMainDisplayID())
						display = candidate;
				if (!display)
					display = content.displays.firstObject;
				if (!display) {
					state->fail(QStringLiteral("macOS did not provide a display while refreshing application audio."));
					finishRouting(state, revision);
					return;
				}
				SCStream *stream = nil;
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					if (!state->abandoned.load())
						stream = state->stream;
				}
				if (!stream) {
					finishRouting(state, revision);
					return;
				}
				SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display
					includingApplications:targets exceptingWindows:@[]];
				[stream updateContentFilter:filter completionHandler:^(NSError *updateError) {
					dispatch_async(state->routingQueue, ^{
						if (!currentRouting(state, revision)) {
							finishRouting(state, revision);
							return;
						}
						if (updateError) {
							state->fail(nativeError(updateError, QStringLiteral("macOS could not update the application's audio filter.")));
							finishRouting(state, revision);
							return;
						}
						if (state->streamStopped.load()) {
							[stream startCaptureWithCompletionHandler:^(NSError *startError) {
								dispatch_async(state->routingQueue, ^{
									if (!currentRouting(state, revision)) {
										if (state->abandoned.load() && !startError)
											[stream stopCaptureWithCompletionHandler:^(NSError *) {}];
									} else if (startError) {
										state->fail(nativeError(startError, QStringLiteral("macOS could not resume application audio.")));
									} else {
										state->streamStopped.store(false);
										state->running.store(true);
										state->routingPending.store(false);
									}
									finishRouting(state, revision);
								});
							}];
						} else {
							state->running.store(true);
							state->routingPending.store(false);
							finishRouting(state, revision);
						}
					});
				}];
			});
		}];
}

API_AVAILABLE(macos(13.0))
void scheduleApplicationRefresh(const std::shared_ptr<State> &state)
{
	if (state->abandoned.load() || !state->accepting.load())
		return;
	if (!state->routingQueue) {
		state->fail(QStringLiteral("The selected app stopped before system audio could start. Start the app and try again."));
		return;
	}
	state->routingPending.store(true);
	state->waitingForApp.store(false);
	const uint64_t revision = state->routingRevision.fetch_add(1) + 1;
	dispatch_async(state->routingQueue, ^{ refreshApplicationFilter(state, revision); });
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), state->routingQueue, ^{
		if (currentRouting(state, revision) && state->routingPending.load() && !state->waitingForApp.load())
			state->fail(QStringLiteral("macOS took too long to refresh application audio. Start a new take."));
	});
}

API_AVAILABLE(macos(13.0))
void watchApplication(const std::shared_ptr<State> &state)
{
	if (state->bundleId.isEmpty())
		return;
	NSString *wanted = [NSString stringWithUTF8String:state->bundleId.toUtf8().constData()];
	void (^changed)(NSNotification *) = ^(NSNotification *notification) {
		NSRunningApplication *application = notification.userInfo[NSWorkspaceApplicationKey];
		if (state->abandoned.load() || !state->accepting.load() || !application.bundleIdentifier.length ||
		    [application.bundleIdentifier caseInsensitiveCompare:wanted] != NSOrderedSame)
			return;
		scheduleApplicationRefresh(state);
	};
	NSNotificationCenter *center = NSWorkspace.sharedWorkspace.notificationCenter;
	id launched = [center addObserverForName:NSWorkspaceDidLaunchApplicationNotification object:nil queue:nil usingBlock:changed];
	id terminated = [center addObserverForName:NSWorkspaceDidTerminateApplicationNotification object:nil queue:nil usingBlock:changed];
	state->appObservers = @[launched, terminated];
}
} // namespace

struct Capture::Impl {
	std::shared_ptr<State> state;
};

Capture::Capture() : d_(std::make_unique<Impl>()) {}
Capture::~Capture() { stop(); }

bool Capture::start(const QString &bundleId, QString *error)
{
	stop();
	d_->state.reset();
	const auto fail = [error](const QString &why) {
		if (error)
			*error = why;
		return false;
	};
	if (!available())
		return fail(QStringLiteral("Recording system or application audio requires macOS 13 or newer."));
	if (!CGPreflightScreenCaptureAccess())
		return fail(QStringLiteral("Allow Harpia in System Settings > Privacy & Security > Screen & System Audio Recording, then start a new take."));
	if (@available(macOS 13.0, *)) {
		const auto state = std::make_shared<State>();
		state->bundleId = bundleId;
		if (!bundleId.isEmpty())
			state->routingQueue = dispatch_queue_create("com.harpia.editor.audio-routing", DISPATCH_QUEUE_SERIAL);
		d_->state = state;
		[SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO
			completionHandler:^(SCShareableContent *content, NSError *nativeFailure) {
				std::lock_guard<std::mutex> lock(state->mutex);
				if (!state->abandoned.load()) {
					state->content = content;
					if (nativeFailure)
						state->error = nativeError(nativeFailure, QStringLiteral("macOS could not list capture sources."));
					state->contentDone = true;
					state->changed.notify_all();
				}
			}];
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			if (!state->changed.wait_for(lock, std::chrono::seconds(8), [&] { return state->contentDone; })) {
				state->abandoned.store(true);
				return fail(QStringLiteral("macOS took too long to list system-audio sources. Check recording permission and try again."));
			}
			if (!state->error.isEmpty())
				return fail(state->error);
		}
		SCDisplay *display = nil;
		for (SCDisplay *candidate in state->content.displays)
			if (candidate.displayID == CGMainDisplayID())
				display = candidate;
		if (!display)
			display = state->content.displays.firstObject;
		if (!display)
			return fail(QStringLiteral("macOS did not provide a display for system audio. Check Screen & System Audio Recording permission."));
		SCContentFilter *filter = nil;
		if (bundleId.isEmpty()) {
			filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
		} else {
			NSMutableArray<SCRunningApplication *> *targets = [[NSMutableArray alloc] init];
			NSString *wanted = [NSString stringWithUTF8String:bundleId.toUtf8().constData()];
			for (SCRunningApplication *application in state->content.applications)
				if (application.bundleIdentifier.length &&
				    [application.bundleIdentifier caseInsensitiveCompare:wanted] == NSOrderedSame)
					[targets addObject:application];
			if (targets.count == 0)
				return fail(QStringLiteral("%1 is not available for capture. Start that app and try again.").arg(bundleId));
			filter = [[SCContentFilter alloc] initWithDisplay:display includingApplications:targets exceptingWindows:@[]];
		}
		SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
		configuration.capturesAudio = YES;
		configuration.excludesCurrentProcessAudio = YES;
		configuration.sampleRate = int(rate);
		configuration.channelCount = 2;
		configuration.showsCursor = NO;
		configuration.width = 2;
		configuration.height = 2;
		configuration.minimumFrameInterval = CMTimeMake(1, 1);
		configuration.queueDepth = 3;
		state->queue = dispatch_queue_create("com.harpia.editor.system-audio", DISPATCH_QUEUE_SERIAL);
		HarpiaSystemAudioOutput *output = [[HarpiaSystemAudioOutput alloc] init];
		output->state = state;
		state->output = output;
		state->stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:output];
		NSError *nativeFailure = nil;
		if (![state->stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:state->queue error:&nativeFailure] ||
		    ![state->stream addStreamOutput:output type:SCStreamOutputTypeAudio sampleHandlerQueue:state->queue error:&nativeFailure]) {
			const QString why = nativeError(nativeFailure, QStringLiteral("macOS could not configure system-audio capture."));
			state->stream = nil;
			state->output = nil;
			return fail(why);
		}
		SCStream *startingStream = state->stream;
		[startingStream startCaptureWithCompletionHandler:^(NSError *nativeFailure) {
			if (state->abandoned.load()) {
				if (!nativeFailure)
					[startingStream stopCaptureWithCompletionHandler:^(NSError *) {}];
				return;
			}
			std::lock_guard<std::mutex> lock(state->mutex);
			state->startOk = nativeFailure == nil && state->error.isEmpty();
			if (state->startOk) {
				// The editor starts playback after start() returns. Discard
				// native initialization pre-roll and anchor to this handshake,
				// so a slow SCK startup cannot shift narration on the timeline.
				state->startClock = std::chrono::steady_clock::now();
				state->startTime = CMClockGetTime(CMClockGetHostTimeClock());
				state->clockReady = true;
				state->accepting.store(true);
			}
			if (nativeFailure)
				state->error = nativeError(nativeFailure, QStringLiteral("macOS could not start system-audio capture."));
			state->startDone = true;
			state->running.store(state->startOk);
			state->changed.notify_all();
		}];
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			if (!state->changed.wait_for(lock, std::chrono::seconds(8), [&] { return state->startDone; })) {
				state->abandoned.store(true);
				lock.unlock();
				stop();
				return fail(QStringLiteral("macOS took too long to start system audio. Check recording permission and try again."));
			}
			if (!state->startOk) {
				const QString why = state->error;
				lock.unlock();
				stop();
				return fail(why);
			}
		}
		watchApplication(state);
		if (error)
			error->clear();
		return true;
	}
	return false;
}

void Capture::stop()
{
	if (!d_ || !d_->state)
		return;
	const auto state = d_->state;
	state->abandoned.store(true);
	NSNotificationCenter *center = NSWorkspace.sharedWorkspace.notificationCenter;
	for (id observer in state->appObservers)
		[center removeObserver:observer];
	state->appObservers = nil;
	state->running.store(false);
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->finishedFrames < 0)
			state->finishedFrames = state->dueFrames();
	}
	if (@available(macOS 13.0, *)) {
		SCStream *stream = state->stream;
		if (stream) {
			[stream stopCaptureWithCompletionHandler:^(NSError *) {
				std::lock_guard<std::mutex> lock(state->mutex);
				state->stopDone = true;
				state->changed.notify_all();
			}];
			std::unique_lock<std::mutex> lock(state->mutex);
			state->changed.wait_for(lock, std::chrono::seconds(3), [&] { return state->stopDone; });
		}
		// Drain packets already queued before native stop completed. Their
		// timestamps are clipped to the stop time so this preserves the last
		// audible syllable without extending the take by stop-call latency.
		if (state->queue)
			dispatch_sync(state->queue, ^{});
		state->accepting.store(false);
		// A late callback owns shared state and observes accepting=false.
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->stream = nil;
			state->output = nil;
			state->content = nil;
		}
	}
}

bool Capture::running() const
{
	return d_ && d_->state && d_->state->running.load();
}

QByteArray Capture::take()
{
	if (!d_ || !d_->state)
		return {};
	const auto state = d_->state;
	std::lock_guard<std::mutex> lock(state->mutex);
	const qint64 due = state->dueFrames();
	// Leave a latency allowance while live, then finish all elapsed silence
	// after stop. Timestamps discard overlaps if an overdue packet arrives.
	if (state->finishedFrames >= 0 || due - state->written > rate * 15 / 100) {
		const qint64 target = state->finishedFrames >= 0 ? due : std::max<qint64>(0, due - rate * 3 / 100);
		if (target > state->written) {
			state->pending.append(qsizetype((target - state->written) * frameBytes), '\0');
			state->written = target;
		}
	}
	QByteArray out;
	out.swap(state->pending);
	return out;
}

QString Capture::failure() const
{
	if (!d_ || !d_->state)
		return {};
	std::lock_guard<std::mutex> lock(d_->state->mutex);
	return d_->state->error;
}

} // namespace harpia::mac_audio
