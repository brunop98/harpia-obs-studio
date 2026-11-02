// Exercise production shutdown methods against an output that keeps draining.
#include "core/AudioOnlyRecorder.hpp"
#include "core/AutoSplitSettings.hpp"
#include "model/Preset.hpp"
#include <memory>
#include <string>
#define private public
#include "core/RecordingController.hpp"
#undef private
#include <obs.h>
#include <cstdio>

static int stops, forced, pauses, failures;
static bool paused;
extern "C" bool obs_output_active(const obs_output_t *) { return true; }
extern "C" void obs_output_stop(obs_output_t *) { ++stops; }
extern "C" void obs_output_force_stop(obs_output_t *) { ++forced; }
extern "C" bool obs_output_pause(obs_output_t *, bool on) { ++pauses; paused = on; return true; }
extern "C" bool obs_output_paused(const obs_output_t *) { return paused; }
extern "C" uint32_t obs_output_get_flags(const obs_output_t *) { return OBS_OUTPUT_CAN_PAUSE; }
// The tap is not used in this output-path test.
namespace harpia {
AudioOnlyRecorder::~AudioOnlyRecorder() = default;
void AudioOnlyRecorder::requestStop() {}
void AudioOnlyRecorder::setPaused(bool) {}
bool AudioOnlyRecorder::paused() const { return false; }
}
static void ok(bool pass, const char *why) { std::printf("%s %s\n", pass ? "PASS" : "FAIL", why); failures += !pass; }
int main()
{
	harpia::RecordingController c;
	c.output_ = reinterpret_cast<obs_output_t *>(1);
	c.pause(true); ok(pauses == 1 && paused, "pause works during an active take");
	c.stop(); c.stop(); c.stop(); ok(stops == 1, "ordinary stop is idempotent while output drains");
	c.pause(false); ok(pauses == 1 && paused, "pause/resume cannot interrupt draining");
	c.forceStop(); ok(forced == 1, "watchdog force-stop still executes after ordinary stop");
	c.stop(); ok(stops == 1, "ordinary stop stays idempotent after force-stop");
	c.stopRequested_.store(false); paused = false; c.pause(true); c.stop();
	ok(stops == 2 && pauses == 2, "reset stop flag allows another recording lifecycle");
	c.output_ = nullptr;
	calldata_t data; calldata_init(&data); calldata_set_int(&data, "code", 0);
	int finished = 0; c.onFinished = [&](const std::string &) { ++finished; };
	c.stopRequested_.store(false); harpia::RecordingController::onStopSignal(&c, &data);
	ok(c.stopRequested_.load() && finished == 1, "final stop callback marks shutdown before notifying UI");
	calldata_free(&data);
	return failures ? 1 : 0;
}
