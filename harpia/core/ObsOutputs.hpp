#pragma once

// Stopping an output before letting go of it.
//
// obs_output_stop() only ASKS: the muxer finishes the file, the encoders
// detach and the output deactivates later, on libobs's own threads. Released
// in the meantime, the output is force-stopped (the file loses its trailer --
// an MP4 without its moov atom does not play) and, worse, whatever it was fed
// from can be torn down under it: the webcam recorder destroyed its private
// video mix right after, with the encoder still attached to it.
//
// So: ask, wait (bounded -- a stuck muxer must not hang a quit), and only if
// it never finishes, force it. Returns at once for an output that is not
// running, which is every normal path: they stop and reap on their own.

#include <obs.h>

#include <chrono>
#include <thread>

namespace harpia {

inline void stopOutputAndWait(obs_output_t *output, int timeoutMs = 5000)
{
	if (!output || !obs_output_active(output))
		return;
	obs_output_stop(output);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (obs_output_active(output) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	if (obs_output_active(output)) {
		blog(LOG_WARNING, "[harpia] output did not stop within %d ms; forcing it", timeoutMs);
		obs_output_force_stop(output);
	}
}

} // namespace harpia
