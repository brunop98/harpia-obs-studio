// ScreenCaptureKit hands back floating-point planar audio on common devices.
// Verify actual packet packing without requiring an Apple SDK or sound device.
#include "platform/mac/MacAudioPcm.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace harpia::mac_audio;
static int failures = 0;
static void check(bool value, const char *message)
{
	std::printf("  %s %s\n", value ? "PASS" : "FAIL", message);
	failures += !value;
}
int main()
{
	const float left[] = {-1.f, 0.5f, 1.f, std::numeric_limits<float>::quiet_NaN(), 0.99999994f};
	const float right[] = {1.f, -0.5f, -2.f, std::numeric_limits<float>::infinity(), -0.99999994f};
	const PcmPlane planar[] = {{left, sizeof(left)}, {right, sizeof(right)}};
	int16_t out[10] = {};
	check(packStereo16(planar, 2, 5, 2, true, PcmSample::Float32, out, 10) &&
	      out[0] == -32768 && out[1] == 32767 && out[2] == 16384 && out[3] == -16384,
	      "planar float channels become interleaved PCM with correct sign and amplitude");
	check(out[4] == 32767 && out[5] == -32768 && out[6] == 0 && out[7] == 0,
	      "overload clips and non-finite values cannot become loud noise");
	check(out[8] == 32767 && out[9] == -32768,
	      "rounding immediately below full scale cannot wrap positive samples negative");

	const int16_t original[] = {-32768, 32767, 12345, -23456};
	const PcmPlane interleaved[] = {{original, sizeof(original)}};
	check(packStereo16(interleaved, 1, 2, 2, false, PcmSample::Signed16, out, 10) &&
	      std::memcmp(original, out, sizeof(original)) == 0,
	      "native signed 16-bit stereo preserves every sample exactly");
	const int32_t wide[] = {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()};
	const PcmPlane widePlane[] = {{wide, sizeof(wide)}};
	check(packStereo16(widePlane, 1, 1, 2, false, PcmSample::Signed32, out, 10) &&
	      out[0] == -32768 && out[1] == 32767,
	      "signed 32-bit capture reaches both output rails without overflow");

	const double mono[] = {0.25, -0.25, std::numeric_limits<double>::quiet_NaN()};
	const PcmPlane monoPlane[] = {{mono, sizeof(mono)}};
	check(packStereo16(monoPlane, 1, 3, 1, true, PcmSample::Float64, out, 10) &&
	      out[0] == 8192 && out[1] == 8192 && out[2] == -8192 && out[3] == -8192 &&
	      out[4] == 0 && out[5] == 0,
	      "mono double-precision capture is duplicated into stereo safely");

	std::vector<unsigned char> unaligned(sizeof(original) + 1);
	std::memcpy(unaligned.data() + 1, original, sizeof(original));
	const PcmPlane unalignedPlane[] = {{unaligned.data() + 1, sizeof(original)}};
	check(packStereo16(unalignedPlane, 1, 2, 2, false, PcmSample::Signed16, out, 10) &&
	      std::memcmp(original, out, sizeof(original)) == 0,
	      "unaligned native buffers are read without undefined typed loads");

	std::fill(std::begin(out), std::end(out), int16_t(999));
	const PcmPlane shortPlane[] = {{original, sizeof(original) - 1}};
	check(!packStereo16(shortPlane, 1, 2, 2, false, PcmSample::Signed16, out, 10) && out[0] == 999,
	      "truncated channel buffers fail before modifying output");
	check(!packStereo16(planar, 2, 5, 2, true, PcmSample::Float32, out, 9) && out[0] == 999,
	      "an undersized destination cannot be overrun");
	check(!packStereo16(planar, 1, 5, 2, true, PcmSample::Float32, out, 10) &&
	      !packStereo16(planar, 2, 5, 3, true, PcmSample::Float32, out, 10),
	      "missing planar channels and unexpected surround layouts are rejected");
	check(!packStereo16(planar, 2, std::numeric_limits<size_t>::max(), 2, true,
		PcmSample::Float32, out, 10) &&
	      !packStereo16(nullptr, 0, 1, 2, true, PcmSample::Float32, out, 10),
	      "invalid lengths and absent buffers cannot overflow size calculations");
	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
