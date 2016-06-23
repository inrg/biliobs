#include <stdlib.h>
#include <util/dstr.h>
#include "dc-capture.h"
#include "window-helpers.h"

#define TEXT_WINDOW_CAPTURE obs_module_text("WindowCapture")
#define TEXT_WINDOW         obs_module_text("WindowCapture.Window")
#define TEXT_MATCH_PRIORITY obs_module_text("WindowCapture.Priority")
#define TEXT_MATCH_TITLE    obs_module_text("WindowCapture.Priority.Title")
#define TEXT_MATCH_CLASS    obs_module_text("WindowCapture.Priority.Class")
#define TEXT_MATCH_EXE      obs_module_text("WindowCapture.Priority.Exe")
#define TEXT_CAPTURE_CURSOR obs_module_text("CaptureCursor")
#define TEXT_COMPATIBILITY  obs_module_text("Compatibility")

struct window_capture {
	obs_source_t         *source;

	char                 *title;
	char                 *class;
	char                 *executable;
	enum window_priority priority;
	bool                 cursor;
	bool                 compatibility;
	bool                 use_wildcards; /* TODO */

	struct dc_capture    capture;

	float                resize_timer;

	HWND                 window;
	RECT                 last_rect;
	bool                 window_exists;

	bool                 is_limit_rect;
	RECT                 limit_rect;
};

static void update_settings(struct window_capture *wc, obs_data_t *s)
{
	const char *window     = obs_data_get_string(s, "window");
	int        priority    = (int)obs_data_get_int(s, "priority");

	bfree(wc->title);
	bfree(wc->class);
	bfree(wc->executable);

	build_window_strings(window, &wc->class, &wc->title, &wc->executable);

	wc->priority      = (enum window_priority)priority;
	wc->cursor        = obs_data_get_bool(s, "cursor");
	wc->use_wildcards = obs_data_get_bool(s, "use_wildcards");
	wc->compatibility = obs_data_get_bool(s, "compatibility");

	wc->is_limit_rect = obs_data_get_bool(s, "is_limit_rect");
	wc->limit_rect.left = obs_data_get_int(s, "limit_x");
	wc->limit_rect.right = wc->limit_rect.left + obs_data_get_int(s, "limit_cx");
	wc->limit_rect.top = obs_data_get_int(s, "limit_y");
	wc->limit_rect.bottom = wc->limit_rect.top + obs_data_get_int(s, "limit_cy");

	if (obs_data_get_bool(s, "is_screen_rect") == true)
	{
		POINT relativePos = { 0, 0 };
		HWND hWin = find_window(EXCLUDE_MINIMIZED, wc->priority,
			wc->class, wc->title, wc->executable);
		if (hWin != NULL)
		{
			ClientToScreen(hWin, &relativePos);
			wc->limit_rect.left -= relativePos.x;
			wc->limit_rect.top -= relativePos.y;
			wc->limit_rect.right -= relativePos.x;
			wc->limit_rect.bottom -= relativePos.y;

			obs_data_set_int(s, "limit_x", wc->limit_rect.left);
			obs_data_set_int(s, "limit_y", wc->limit_rect.top);
		}
		else
		{
			wc->is_limit_rect = false;
			obs_data_set_bool(s, "is_limit_rect", false);
		}

		obs_data_set_bool(s, "is_screen_rect", false);
	}
}

/* ------------------------------------------------------------------------- */

static const char *wc_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return TEXT_WINDOW_CAPTURE;
}

static void *wc_create(obs_data_t *settings, obs_source_t *source)
{
	struct window_capture *wc = bzalloc(sizeof(struct window_capture));
	wc->source = source;

	update_settings(wc, settings);
	return wc;
}

static void wc_destroy(void *data)
{
	struct window_capture *wc = data;

	if (wc) {
		obs_enter_graphics();
		dc_capture_free(&wc->capture);
		obs_leave_graphics();

		bfree(wc->title);
		bfree(wc->class);
		bfree(wc->executable);

		bfree(wc);
	}
}

static void wc_update(void *data, obs_data_t *settings)
{
	struct window_capture *wc = data;
	update_settings(wc, settings);

	/* forces a reset */
	wc->window = NULL;
}

static uint32_t wc_width(void *data)
{
	struct window_capture *wc = data;
	return wc->capture.width;
}

static uint32_t wc_height(void *data)
{
	struct window_capture *wc = data;
	return wc->capture.height;
}

static void wc_defaults(obs_data_t *defaults)
{
	obs_data_set_default_bool(defaults, "cursor", true);
	obs_data_set_default_bool(defaults, "compatibility", false);
}

static obs_properties_t *wc_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(ppts, "window", TEXT_WINDOW,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_window_list(p, EXCLUDE_MINIMIZED);

	p = obs_properties_add_list(ppts, "priority", TEXT_MATCH_PRIORITY,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, TEXT_MATCH_TITLE, WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(p, TEXT_MATCH_CLASS, WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(p, TEXT_MATCH_EXE,   WINDOW_PRIORITY_EXE);

	obs_properties_add_bool(ppts, "cursor", TEXT_CAPTURE_CURSOR);

	obs_properties_add_bool(ppts, "compatibility", TEXT_COMPATIBILITY);

	return ppts;
}

#define RESIZE_CHECK_TIME 0.2f

static void wc_tick(void *data, float seconds)
{
	struct window_capture *wc = data;
	RECT rect;
	RECT abRect;
	bool reset_capture = false;

	if (!obs_source_showing(wc->source))
		return;

	if (!wc->window || !IsWindow(wc->window)) {
		wc->window_exists = false;
		if (!wc->title && !wc->class)
			return;

		wc->window = find_window(EXCLUDE_MINIMIZED, wc->priority,
				wc->class, wc->title, wc->executable);
		if (!wc->window)
			return;

		reset_capture = true;

	} else if (IsIconic(wc->window)) {
		return;
	}

	obs_enter_graphics();

	GetClientRect(wc->window, &rect);
	GetWindowRect(wc->window, &abRect);

	if (!reset_capture) {
		wc->resize_timer += seconds;

		if (wc->resize_timer >= RESIZE_CHECK_TIME) {
			if (rect.bottom != wc->last_rect.bottom ||
			    rect.right  != wc->last_rect.right)
				reset_capture = true;

			wc->resize_timer = 0.0f;
		}
	}

	if (reset_capture) {
		RECT rectToCapture = rect;
		if (wc->is_limit_rect)
		{
			//����rectToCapture��limit_rect���ཻ����
			int left = max(wc->limit_rect.left, rectToCapture.left);
			int right = min(wc->limit_rect.right, rectToCapture.right);
			int top = max(wc->limit_rect.top, rectToCapture.top);
			int bottom = min(wc->limit_rect.bottom, rectToCapture.bottom);

			if (right > left && top < bottom)
			{
				rectToCapture.left = left;
				rectToCapture.right = right;
				rectToCapture.top = top;
				rectToCapture.bottom = bottom;
			}
			else
			{
				rectToCapture.left = 0;
				rectToCapture.right = 0;
				rectToCapture.top = 0;
				rectToCapture.bottom = 0;
			}
		}

		wc->resize_timer = 0.0f;
		wc->last_rect = rect;
		dc_capture_free(&wc->capture);
		dc_capture_init(&wc->capture, rectToCapture.left, rectToCapture.top, rectToCapture.right - rectToCapture.left, rectToCapture.bottom - rectToCapture.top,
				wc->cursor, wc->compatibility);
		wc->window_exists = true;
	}

	dc_capture_capture(&wc->capture, wc->window);
	obs_leave_graphics();
}

static void wc_render(void *data, gs_effect_t *effect)
{
	struct window_capture *wc = data;

	if (wc->window_exists == false)
		return;

	dc_capture_render(&wc->capture, obs_get_opaque_effect());

	UNUSED_PARAMETER(effect);
}

struct obs_source_info window_capture_info = {
	.id             = "window_capture",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name       = wc_getname,
	.create         = wc_create,
	.destroy        = wc_destroy,
	.update         = wc_update,
	.video_render   = wc_render,
	.video_tick     = wc_tick,
	.get_width      = wc_width,
	.get_height     = wc_height,
	.get_defaults   = wc_defaults,
	.get_properties = wc_properties
};