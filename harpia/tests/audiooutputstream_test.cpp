#include "editor/AudioOutputStream.hpp"

#include <QCoreApplication>
#include <QByteArray>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using harpia::AudioOutputStream;
namespace {
int failures = 0;
void check(bool condition, const char *description)
{
	std::printf("  %s %s\n", condition ? "PASS" : "FAIL", description);
	failures += !condition;
}
QAudioFormat format(int rate, int channels, QAudioFormat::SampleFormat sample)
{
	QAudioFormat out;
	out.setSampleRate(rate);
	out.setChannelCount(channels);
	out.setSampleFormat(sample);
	return out;
}
QByteArray readOddChunks(AudioOutputStream &stream, int bytes)
{
	QByteArray out(bytes, Qt::Uninitialized);
	int at = 0;
	const int sizes[] = {1, 3, 13, 511, 16381};
	for (int i = 0; at < bytes; ++i) {
		const int count = std::min(sizes[i % 5], bytes - at);
		if (stream.read(out.data() + at, count) != count)
			return {};
		at += count;
	}
	return out;
}
template<typename T> T sampleAt(const QByteArray &bytes, int index)
{
	T out{};
	std::memcpy(&out, bytes.constData() + index * sizeof(T), sizeof(T));
	return out;
}
bool allSilence(const QByteArray &bytes, unsigned char silence = 0)
{
	return std::all_of(bytes.cbegin(), bytes.cend(), [silence](char byte) { return unsigned(uchar(byte)) == silence; });
}
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	const QAudioFormat original = AudioOutputStream::mixFormat();
	const QAudioFormat portable = format(44100, 1, QAudioFormat::Int16);
	check(AudioOutputStream::chooseFormat(portable, [](const QAudioFormat &) { return true; }) == original,
		"a device that accepts the timeline format keeps the original byte-exact path");
	check(AudioOutputStream::chooseFormat(portable, [portable](const QAudioFormat &candidate) { return candidate == portable; }) == portable,
		"a supported preferred device format is selected when the original is unavailable");
	const QAudioFormat pcm16 = format(48000, 2, QAudioFormat::Int16);
	check(AudioOutputStream::chooseFormat({}, [pcm16](const QAudioFormat &candidate) { return candidate == pcm16; }) == pcm16,
		"common supported PCM can be selected even if a backend reports no preferred format");
	check(!AudioOutputStream::chooseFormat(portable, [](const QAudioFormat &) { return false; }).isValid(),
		"an incompatible output is explicitly rejected");

	const std::vector<float> exact = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f, -1.0f, 0.125f, -0.125f};
	AudioOutputStream direct(exact, 0, original);
	const QByteArray directBytes = readOddChunks(direct, int(exact.size() * sizeof(float)) + 19);
	check(direct.isValid() && directBytes.size() == int(exact.size() * sizeof(float)) + 19 &&
		std::memcmp(directBytes.constData(), exact.data(), exact.size() * sizeof(float)) == 0,
		"float48k stereo reads preserve every sample despite unaligned byte requests");
	check(allSilence(directBytes.mid(int(exact.size() * sizeof(float)))) && direct.atEndOfMix(),
		"the direct path supplies endless silence after its mix ends");
	AudioOutputStream seek(exact, 2, original);
	const QByteArray sought = readOddChunks(seek, 16);
	check(sought.size() == 16 && std::memcmp(sought.constData(), exact.data() + 4, 16) == 0,
		"seeking starts at the requested stereo input frame");
	AudioOutputStream negative(exact, -100, original);
	check(readOddChunks(negative, 8) == directBytes.left(8), "a negative seek is clamped to the first input frame");
	AudioOutputStream past(exact, 10000, original);
	check(allSilence(readOddChunks(past, 33)), "seeking beyond the mix produces silence without reading past its storage");
	check(direct.bufferedBytes() == 0, "the original format needs no converted copy or scratch buffer");

	AudioOutputStream to16(exact, 0, pcm16);
	const QByteArray int16 = readOddChunks(to16, 16);
	check(int16.size() == 16 && sampleAt<qint16>(int16, 0) == -32768 && sampleAt<qint16>(int16, 1) == -16384 &&
		sampleAt<qint16>(int16, 2) == 0 && sampleAt<qint16>(int16, 3) == 16384 && sampleAt<qint16>(int16, 4) == 32767,
		"16-bit PCM preserves signed amplitudes and clips full-scale input safely");
	check(allSilence(readOddChunks(to16, 41)), "16-bit output continues with zero-valued silence");
	AudioOutputStream to32(exact, 0, format(48000, 2, QAudioFormat::Int32));
	const QByteArray int32 = readOddChunks(to32, 32);
	check(int32.size() == 32 && sampleAt<qint32>(int32, 1) == -1073741824 && sampleAt<qint32>(int32, 3) == 1073741824,
		"32-bit integer output is scaled rather than reinterpreted as float bits");
	AudioOutputStream to8(exact, 0, format(48000, 2, QAudioFormat::UInt8));
	const QByteArray uint8 = readOddChunks(to8, 8);
	check(uint8.size() == 8 && uchar(uint8[0]) == 0 && uchar(uint8[2]) == 128 && uchar(uint8[4]) == 255,
		"unsigned 8-bit output uses its proper midpoint and amplitude range");
	check(allSilence(readOddChunks(to8, 11), 128), "unsigned output silence is 128 instead of a loud zero-valued signal");

	std::vector<float> opposing(400);
	for (int i = 0; i < 200; ++i) {
		opposing[2 * i] = 0.5f;
		opposing[2 * i + 1] = -0.5f;
	}
	AudioOutputStream mono(opposing, 0, format(48000, 1, QAudioFormat::Float));
	check(allSilence(readOddChunks(mono, 800)), "mono output combines both source channels instead of dropping one");

	std::vector<float> tone(48000 * 2);
	for (int i = 0; i < 48000; ++i)
		tone[2 * i] = tone[2 * i + 1] = float(0.25 * std::sin(double(i) * 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0));
	AudioOutputStream resampled(tone, 0, format(44100, 2, QAudioFormat::Int16));
	const QByteArray samples = readOddChunks(resampled, 44100 * 2 * 2);
	int crossings = 0;
	double energy = 0.0;
	bool stereo = samples.size() == 44100 * 4;
	for (int i = 1; stereo && i < 44100; ++i) {
		const qint16 now = sampleAt<qint16>(samples, i * 2);
		const qint16 before = sampleAt<qint16>(samples, (i - 1) * 2);
		crossings += before <= 0 && now > 0;
		energy += std::pow(double(now) / 32768.0, 2.0);
		stereo = now == sampleAt<qint16>(samples, i * 2 + 1);
	}
	check(samples.size() == 44100 * 4 && crossings >= 999 && crossings <= 1001,
		"resampling to 44.1k preserves a one-second tone's pitch and duration");
	check(stereo && std::abs(std::sqrt(energy / 44099.0) - 0.25 / std::sqrt(2.0)) < 0.002,
		"resampling preserves the stereo tone's level and channel correspondence");
	check(allSilence(readOddChunks(resampled, 127)) && resampled.atEndOfMix(),
		"resampler delay is flushed before silence begins");
	AudioOutputStream resampledSeek(tone, 24000, format(44100, 2, QAudioFormat::Int16));
	const QByteArray half = readOddChunks(resampledSeek, 22050 * 4);
	check(half.size() == 22050 * 4 && !allSilence(half) && allSilence(readOddChunks(resampledSeek, 51)),
		"a seek before resampling uses the requested source position and remaining duration");

	QAudioFormat surround = format(48000, 6, QAudioFormat::Float);
	surround.setChannelConfig(QAudioFormat::ChannelConfigSurround5Dot1);
	AudioOutputStream toSurround(exact, 0, surround);
	const QByteArray six = readOddChunks(toSurround, 24);
	check(six.size() == 24 && sampleAt<float>(six, 0) == -1.0f && sampleAt<float>(six, 1) == -0.5f &&
		sampleAt<float>(six, 2) == 0 && sampleAt<float>(six, 3) == 0 && sampleAt<float>(six, 4) == 0 && sampleAt<float>(six, 5) == 0,
		"a surround output keeps front stereo routing and fills unused speakers with silence");
	AudioOutputStream bounded(tone, 0, format(44100, 2, QAudioFormat::Int16));
	char first = 0;
	check(bounded.read(&first, 1) == 1 && bounded.bufferedBytes() > 0 && bounded.bufferedBytes() <= 128 * 1024 &&
		bounded.bufferedBytes() < qsizetype(tone.size() * sizeof(float)),
		"active conversion scratch stays bounded instead of copying the complete timeline");

	const std::vector<float> empty;
	AudioOutputStream emptyMix(empty, 0, portable);
	check(emptyMix.isValid() && allSilence(readOddChunks(emptyMix, 97)), "an empty converted mix is safely silent");
	AudioOutputStream invalid(exact, 0, {});
	char one = 0;
	check(!invalid.isValid() && !invalid.error().isEmpty() && invalid.read(&one, 1) == -1,
		"an invalid format reports an error instead of supplying incorrectly encoded data");
	AudioOutputStream invalidBuffer(exact, 0, original);
	check(invalidBuffer.read(nullptr, 1) == -1 && !invalidBuffer.error().isEmpty(),
		"an invalid output buffer fails explicitly");
	AudioOutputStream zero(exact, 0, original);
	check(zero.read(&one, 0) == 0 && zero.read(&one, -1) == 0 && zero.isValid(),
		"empty read requests do not consume input or corrupt the stream");

	std::printf("\n%d failures\n", failures);
	return failures ? 1 : 0;
}
