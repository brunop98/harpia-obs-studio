// Optional native regression: a source/scene is created while the canvas is a
// full monitor, then reused for Custom Region takes of different sizes. Read
// the real OBS item transforms and pixels after each reset, including a second
// monitor capture. Requires built libobs, its renderer and obs-filters.
// Usage: regioncapture_test <obs-filters module> <obs-filters data>
//        [--legacy-relative]
// The final option is a negative control: it reproduces the old narrow strip
// and black frame by restoring libobs' default relative scene coordinates.
#include "core/CaptureManager.hpp"
#include "core/ObsContext.hpp"

#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <util/platform.h>

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace harpia;
namespace {
int failures = 0;

void check(bool condition, const char *message)
{
	std::printf("%s %s\n", condition ? "PASS" : "FAIL", message);
	failures += !condition;
}

struct Pattern {
	gs_texture_t *texture = nullptr;
	int monitor = 0;
};

void registerDisplay()
{
	obs_source_info info = {};
	info.id = CaptureManager::platformCaptureId();
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) { return "Region regression display"; };
	info.get_properties = [](void *) {
		obs_properties_t *properties = obs_properties_create();
		obs_property_t *monitors = obs_properties_add_list(properties, "screen", "Display",
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(monitors, "Main", 0);
		obs_property_list_add_int(monitors, "Second", 1);
		return properties;
	};
	info.create = [](obs_data_t *settings, obs_source_t *) -> void * {
		auto *pattern = new Pattern;
		pattern->monitor = int(obs_data_get_int(settings, "screen"));
		std::vector<uint8_t> pixels(1920 * 1080 * 4);
		for (int y = 0; y < 1080; ++y)
			for (int x = 0; x < 1920; ++x) {
				const size_t at = size_t(y * 1920 + x) * 4;
				pixels[at] = uint8_t((x / 64 % 2) ? 220 : 20);
				pixels[at + 1] = uint8_t((y / 64 % 2) ? 220 : 20);
				pixels[at + 2] = uint8_t(pattern->monitor ? 200 : 40);
				pixels[at + 3] = 255;
			}
		const uint8_t *data = pixels.data();
		obs_enter_graphics();
		pattern->texture = gs_texture_create(1920, 1080, GS_RGBA, 1, &data, 0);
		obs_leave_graphics();
		return pattern;
	};
	info.destroy = [](void *data) {
		auto *pattern = static_cast<Pattern *>(data);
		obs_enter_graphics();
		gs_texture_destroy(pattern->texture);
		obs_leave_graphics();
		delete pattern;
	};
	info.get_width = [](void *) -> uint32_t { return 1920; };
	info.get_height = [](void *) -> uint32_t { return 1080; };
	info.video_render = [](void *data, gs_effect_t *) {
		const auto *pattern = static_cast<Pattern *>(data);
		obs_source_draw(pattern->texture, 0, 0, 1920, 1080, false);
	};
	obs_register_source(&info);
}

struct Items {
	obs_sceneitem_t *main = nullptr;
	obs_sceneitem_t *extra = nullptr;
	obs_source_t *source = nullptr;
};

Items sceneItems(obs_source_t *sceneSource, obs_source_t *mainSource)
{
	Items items;
	items.source = mainSource;
	obs_scene_enum_items(obs_scene_from_source(sceneSource), [](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
		auto *result = static_cast<Items *>(opaque);
		if (obs_sceneitem_get_source(item) == result->source)
			result->main = item;
		else
			result->extra = item;
		return true;
	}, &items);
	return items;
}

bool pixelTransform(obs_sceneitem_t *item)
{
	if (!item)
		return false;
	vec2 scale = {}, position = {};
	obs_sceneitem_get_scale(item, &scale);
	obs_sceneitem_get_pos(item, &position);
	std::printf("  item scale %.6f,%.6f at %.2f,%.2f\n", scale.x, scale.y, position.x, position.y);
	return std::abs(scale.x - 1.0f) < 0.0001f && std::abs(scale.y - 1.0f) < 0.0001f &&
	       std::abs(position.x) < 0.01f && std::abs(position.y) < 0.01f;
}

bool correctPixels(obs_source_t *scene, const CaptureRegion &region, int monitor)
{
	// Allow the real crop filter's video tick to refresh its natural size.
	os_sleep_ms(100);
	obs_enter_graphics();
	gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(region.width, region.height, GS_RGBA);
	bool ok = render && stage && gs_texrender_begin(render, region.width, region.height);
	if (ok) {
		vec4 black = {0, 0, 0, 1};
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_ortho(0, float(region.width), 0, float(region.height), -100, 100);
		gs_matrix_identity();
		obs_source_video_render(scene);
		gs_texrender_end(render);
		gs_stage_texture(stage, gs_texrender_get_texture(render));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		ok = gs_stagesurface_map(stage, &data, &linesize);
		if (ok) {
			for (int y : {8, region.height / 2, region.height - 9})
				for (int x : {8, region.width / 2, region.width - 9}) {
					const uint8_t *pixel = data + size_t(y) * linesize + x * 4;
					const int red = ((region.x + x) / 64 % 2) ? 220 : 20;
					const int green = ((region.y + y) / 64 % 2) ? 220 : 20;
					const int blue = monitor ? 200 : 40;
					if (std::abs(int(pixel[0]) - red) > 15 ||
					    std::abs(int(pixel[1]) - green) > 15 ||
					    std::abs(int(pixel[2]) - blue) > 15 || pixel[3] < 240) {
						std::printf("  at %d,%d: got %d,%d,%d,%d expected %d,%d,%d,255\n", x, y,
							pixel[0], pixel[1], pixel[2], pixel[3], red, green, blue);
						ok = false;
					}
				}
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(render);
	obs_leave_graphics();
	return ok;
}
} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 3) {
		std::fprintf(stderr, "Need obs-filters module and its data folder, optional --legacy-relative\n");
		return 2;
	}
	QTemporaryDir settings;
	qputenv("XDG_CONFIG_HOME", settings.filePath(QStringLiteral("config")).toUtf8());
	QCoreApplication app(argc, argv);
	ObsContext backend;
	check(backend.startup(), "the recorder initializes its real libobs backend");
	if (!backend.initialized())
		return 2;
	if (argc > 3 && std::strcmp(argv[3], "--legacy-relative") == 0) {
		obs_data_t *legacy = obs_data_create();
		obs_data_set_bool(legacy, "AbsoluteCoordinates", false);
		obs_apply_private_data(legacy);
		obs_data_release(legacy);
	}
	check(backend.resetVideo(1920, 1080, 30) == OBS_VIDEO_SUCCESS, "a full-monitor canvas initializes");
	if (!obs_get_video())
		return 2;
	obs_module_t *filters = nullptr;
	check(obs_open_module(&filters, argv[1], argv[2]) == MODULE_SUCCESS && obs_init_module(filters),
	      "the real crop filter plugin initializes");
	registerDisplay();
	{
		CaptureManager capture;
		check(capture.startCapture(), "a patterned monitor source is captured through the real scene wrapper");
		obs_source_t *scene = obs_get_output_source(0);
		obs_source_t *source = capture.source();
		if (!scene || !source)
			return 2;
		check(pixelTransform(sceneItems(scene, source).main), "the initial full-monitor scene starts at one-to-one pixels");
		CaptureRegion region{true, 150, 120, 640, 400};
		capture.setRegion(region); // chosen on desktop before the recording starts
		check(backend.resetVideo(640, 400, 30) == OBS_VIDEO_SUCCESS, "the canvas switches to the chosen region");
		check(capture.startCapture() && capture.source() == source, "recording reuses the already live capture source");
		capture.setRegion(region); // the ordinary same-region fast path
		check(pixelTransform(sceneItems(scene, source).main), "the reused crop remains unscaled at the canvas origin");
		check(correctPixels(scene, region, 0), "the selected region fills the frame with the correct source pixels and no black strip");
		region = {true, 320, 80, 400, 600};
		capture.setRegion(region);
		check(backend.resetVideo(400, 600, 30) == OBS_VIDEO_SUCCESS, "a later take can use a different, portrait region");
		check(pixelTransform(sceneItems(scene, source).main), "a different region aspect ratio preserves native pixel placement");
		check(correctPixels(scene, region, 0), "portrait-region pixels also fill the entire recorded frame");
		check(capture.addExtraMonitor(1), "a second monitor is added for multiple-area recording");
		capture.setExtraRegion(1, region);
		capture.showMonitor(1);
		check(correctPixels(scene, region, 1), "the second monitor's cropped area fills the region canvas");
		region = {true, 500, 200, 800, 500};
		capture.setExtraRegion(1, region);
		check(backend.resetVideo(800, 500, 30) == OBS_VIDEO_SUCCESS, "a reused multi-area scene survives another canvas size");
		check(pixelTransform(sceneItems(scene, source).extra), "secondary capture items also retain one-to-one pixel coordinates");
		check(correctPixels(scene, region, 1), "the second monitor stays correctly framed after resizing the canvas");
		capture.clearExtraMonitors();
		capture.setRegion(CaptureRegion{});
		check(backend.resetVideo(1920, 1080, 30) == OBS_VIDEO_SUCCESS, "recording returns to the full-monitor canvas");
		check(pixelTransform(sceneItems(scene, source).main), "returning to full-monitor capture preserves the original transform");
		check(correctPixels(scene, CaptureRegion{true, 0, 0, 1920, 1080}, 0), "full-monitor pixels remain intact after several region recordings");
		obs_source_release(scene);
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
