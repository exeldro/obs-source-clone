#include "source-clone.h"
#include "audio-wrapper.h"
#include <obs-module.h>
#include <obs-frontend-api.h>

const char *source_clone_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("SourceClone");
}

static void *source_clone_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct source_clone *context = bzalloc(sizeof(struct source_clone));
	context->source = source;
	pthread_mutex_init(&context->audio_mutex, NULL);
	context->cx = 1;
	context->cy = 1;
	obs_source_update(source, NULL);
	return context;
}

void source_clone_audio_callback(void *data, obs_source_t *source,
				 const struct audio_data *audio_data,
				 bool muted)
{
	UNUSED_PARAMETER(muted);
	UNUSED_PARAMETER(source);
	struct source_clone *context = data;
	pthread_mutex_lock(&context->audio_mutex);
	size_t size = audio_data->frames * sizeof(float);
	for (size_t i = 0; i < context->num_channels; i++) {
		circlebuf_push_back(&context->audio_data[i],
				    audio_data->data[i], size);
	}
	circlebuf_push_back(&context->audio_frames, &audio_data->frames,
			    sizeof(audio_data->frames));
	circlebuf_push_back(&context->audio_timestamps, &audio_data->timestamp,
			    sizeof(audio_data->timestamp));
	pthread_mutex_unlock(&context->audio_mutex);
}

void source_clone_audio_activate(void *data, calldata_t *calldata)
{
	struct source_clone *context = data;
	obs_source_t *source = calldata_ptr(calldata, "source");
	if (context->audio_enabled && context->clone &&
	    obs_weak_source_references_source(context->clone, source)) {
		obs_source_set_audio_active(context->source, true);
	}
}

void source_clone_audio_deactivate(void *data, calldata_t *calldata)
{
	struct source_clone *context = data;
	obs_source_t *source = calldata_ptr(calldata, "source");
	if (context->clone &&
	    obs_weak_source_references_source(context->clone, source)) {
		obs_source_set_audio_active(context->source, false);
	}
}

static void source_clone_destroy(void *data)
{
	struct source_clone *context = data;
	if (context->audio_wrapper) {
		audio_wrapper_remove(context->audio_wrapper, context);
		context->audio_wrapper = NULL;
	}
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (source) {
		signal_handler_t *sh = obs_source_get_signal_handler(source);
		signal_handler_disconnect(sh, "audio_activate",
					  source_clone_audio_activate, data);
		signal_handler_disconnect(sh, "audio_deactivate",
					  source_clone_audio_deactivate, data);
		obs_source_remove_audio_capture_callback(
			source, source_clone_audio_callback, data);
		if (obs_source_showing(context->source))
			obs_source_dec_showing(source);
		if (context->active_clone && obs_source_active(context->source))
			obs_source_dec_active(source);
		obs_source_release(source);
	}
	obs_weak_source_release(context->clone);
	obs_weak_source_release(context->current_scene);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		circlebuf_free(&context->audio_data[i]);
	}
	circlebuf_free(&context->audio_frames);
	circlebuf_free(&context->audio_timestamps);
	if (context->render) {
		obs_enter_graphics();
		gs_texrender_destroy(context->render);
		obs_leave_graphics();
	}
	pthread_mutex_destroy(&context->audio_mutex);
	bfree(context);
}

void source_clone_switch_source(struct source_clone *context,
				obs_source_t *source)
{
	if (context->audio_wrapper) {
		audio_wrapper_remove(context->audio_wrapper, context);
		context->audio_wrapper = NULL;
	}
	obs_source_t *prev_source = obs_weak_source_get_source(context->clone);
	if (prev_source) {
		signal_handler_t *sh =
			obs_source_get_signal_handler(prev_source);
		signal_handler_disconnect(sh, "audio_activate",
					  source_clone_audio_activate, context);
		signal_handler_disconnect(sh, "audio_deactivate",
					  source_clone_audio_deactivate,
					  context);
		obs_source_remove_audio_capture_callback(
			prev_source, source_clone_audio_callback, context);
		if (obs_source_showing(context->source))
			obs_source_dec_showing(prev_source);
		if (context->active_clone && obs_source_active(context->source))
			obs_source_dec_active(source);
		obs_source_release(prev_source);
	}
	obs_weak_source_release(context->clone);
	context->clone = obs_source_get_weak_source(source);
	if (context->audio_enabled) {
		uint32_t flags = obs_source_get_output_flags(source);
		if ((flags & OBS_SOURCE_AUDIO) != 0) {
			obs_source_add_audio_capture_callback(
				source, source_clone_audio_callback, context);

			obs_source_set_audio_active(
				context->source,
				obs_source_audio_active(source));
			signal_handler_t *sh =
				obs_source_get_signal_handler(source);
			signal_handler_connect(sh, "audio_activate",
					       source_clone_audio_activate,
					       context);
			signal_handler_connect(sh, "audio_deactivate",
					       source_clone_audio_deactivate,
					       context);
		} else if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
			context->audio_wrapper = audio_wrapper_get(true);
			audio_wrapper_add(context->audio_wrapper, context);
			obs_source_set_audio_active(context->source, true);
		} else {
			obs_source_set_audio_active(context->source, false);
		}
	} else {
		obs_source_set_audio_active(context->source, false);
	}
	if (obs_source_showing(context->source))
		obs_source_inc_showing(source);
	if (context->active_clone && obs_source_active(context->source))
		obs_source_inc_active(source);
}

void source_clone_update(void *data, obs_data_t *settings)
{
	struct source_clone *context = data;
	bool audio_enabled = obs_data_get_bool(settings, "audio");
	bool active_clone = obs_data_get_bool(settings, "active_clone");
	context->clone_type = obs_data_get_int(settings, "clone_type");
	if (context->clone_type == CLONE_SOURCE) {
		const char *source_name =
			obs_data_get_string(settings, "clone");
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (source == context->source) {
			obs_source_release(source);
			source = NULL;
		}
		if (source) {
			if (!obs_weak_source_references_source(context->clone,
							       source) ||
			    context->audio_enabled != audio_enabled ||
			    context->active_clone != active_clone) {
				context->audio_enabled = audio_enabled;
				context->active_clone = active_clone;
				source_clone_switch_source(context, source);
			}
			obs_source_release(source);
		}
	}
	context->audio_enabled = audio_enabled;
	context->active_clone = active_clone;
	context->num_channels = audio_output_get_channels(obs_get_audio());
	context->buffer_frame =
		(uint8_t)obs_data_get_int(settings, "buffer_frame");
}

void source_clone_defaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	obs_data_set_default_bool(settings, "audio", true);
}

bool source_clone_list_add_source(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;

	const char *name = obs_source_get_name(source);
	size_t count = obs_property_list_item_count(prop);
	size_t idx = 0;
	while (idx < count &&
	       strcmp(name, obs_property_list_item_string(prop, idx)) > 0)
		idx++;
	obs_property_list_insert_string(prop, idx, name, name);
	return true;
}

bool source_clone_type_changed(void *priv, obs_properties_t *props,
			       obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(property);
	obs_property_t *clone = obs_properties_get(props, "clone");
	obs_property_set_visible(clone,
				 obs_data_get_int(settings, "clone_type") ==
					 CLONE_SOURCE);
	return true;
}

obs_properties_t *source_clone_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(
		props, "clone_type", obs_module_text("CloneType"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Source"), CLONE_SOURCE);
	obs_property_list_add_int(p, obs_module_text("CurrentScene"),
				  CLONE_CURRENT_SCENE);
	obs_property_list_add_int(p, obs_module_text("PreviousScene"),
				  CLONE_PREVIOUS_SCENE);

	obs_property_set_modified_callback2(p, source_clone_type_changed, data);

	p = obs_properties_add_list(props, "clone", obs_module_text("Clone"),
				    OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(source_clone_list_add_source, p);
	obs_enum_scenes(source_clone_list_add_source, p);
	obs_property_list_insert_string(p, 0, "", "");
	//add global audio sources
	for (uint32_t i = 1; i < 7; i++) {
		obs_source_t *s = obs_get_output_source(i);
		if (!s)
			continue;
		source_clone_list_add_source(p, s);
		obs_source_release(s);
	}
	obs_properties_add_bool(props, "audio", obs_module_text("Audio"));
	p = obs_properties_add_list(props, "buffer_frame",
				    obs_module_text("VideoBuffer"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("None"), 0);
	obs_property_list_add_int(p, obs_module_text("Full"), 1);
	obs_property_list_add_int(p, obs_module_text("Half"), 2);
	obs_property_list_add_int(p, obs_module_text("Third"), 3);
	obs_property_list_add_int(p, obs_module_text("Quarter"), 4);

	obs_properties_add_bool(props, "active_clone",
				obs_module_text("ActiveClone"));

	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/source-clone.1632/\">Source Clone</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return props;
}

static const char *
get_tech_name_and_multiplier(enum gs_color_space current_space,
			     enum gs_color_space source_space,
			     float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		switch (current_space) {
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		default:;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		default:;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
		default:;
		}
	}

	return tech_name;
}

static void source_clone_draw_frame(struct source_clone *context)
{

	const enum gs_color_space current_space = gs_get_color_space();
	float multiplier;
	const char *technique = get_tech_name_and_multiplier(
		current_space, context->space, &multiplier);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(context->render);
	if (!tex)
		return;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"),
				   tex);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"),
			    multiplier);

	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(tex, 0, context->cx, context->cy);

	gs_enable_framebuffer_srgb(previous);
}

void source_clone_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct source_clone *context = data;
	if (context->clone_type == CLONE_SOURCE && !context->clone)
		return;

	if (context->buffer_frame > 0 && context->processed_frame) {
		source_clone_draw_frame(context);
		return;
	}
	if (context->rendering)
		return;
	context->rendering = true;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source) {
		context->rendering = false;
		return;
	}
	if (context->buffer_frame == 0) {
		obs_source_video_render(source);
		obs_source_release(source);
		context->rendering = false;
		return;
	}

	if (!context->source_cx || !context->source_cy) {
		obs_source_release(source);
		context->rendering = false;
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space space = obs_source_get_color_space(
		source, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);
	if (!context->render ||
	    gs_texrender_get_format(context->render) != format) {
		gs_texrender_destroy(context->render);
		context->render = gs_texrender_create(format, GS_ZS_NONE);
	} else {
		gs_texrender_reset(context->render);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(context->render, context->cx,
						context->cy, space)) {

		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		if (context->source_cx && context->source_cy) {
			gs_ortho(0.0f, (float)context->source_cx, 0.0f,
				 (float)context->source_cy, -100.0f, 100.0f);
			obs_source_video_render(source);
		}
		gs_texrender_end(context->render);

		context->space = space;
	}

	gs_blend_state_pop();

	context->processed_frame = true;
	obs_source_release(source);
	context->rendering = false;
	source_clone_draw_frame(context);
}

uint32_t source_clone_get_width(void *data)
{
	struct source_clone *context = data;
	if (!context->clone)
		return 1;
	if (context->buffer_frame > 0)
		return context->cx;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return 1;
	uint32_t width = obs_source_get_width(source);
	obs_source_release(source);
	if (context->buffer_frame > 1)
		width /= context->buffer_frame;
	return width;
}

uint32_t source_clone_get_height(void *data)
{
	struct source_clone *context = data;
	if (!context->clone)
		return 1;
	if (context->buffer_frame > 0)
		return context->cy;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return 1;
	uint32_t height = obs_source_get_height(source);
	obs_source_release(source);
	if (context->buffer_frame > 1)
		height /= context->buffer_frame;
	return height;
}

void source_clone_show(void *data)
{
	struct source_clone *context = data;
	if (!context->clone)
		return;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return;
	obs_source_inc_showing(source);
	obs_source_release(source);
}

void source_clone_hide(void *data)
{
	struct source_clone *context = data;
	if (!context->clone)
		return;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return;
	obs_source_dec_showing(source);
	obs_source_release(source);
}

void source_clone_activate(void *data)
{
	struct source_clone *context = data;
	if (!context->clone || !context->active_clone)
		return;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return;
	obs_source_inc_active(source);
	obs_source_release(source);
}

void source_clone_deactivate(void *data)
{
	struct source_clone *context = data;
	if (!context->clone || !context->active_clone)
		return;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return;
	obs_source_dec_active(source);
	obs_source_release(source);
}

void source_clone_save(void *data, obs_data_t *settings)
{
	struct source_clone *context = data;
	if (context->clone_type != CLONE_SOURCE) {
		obs_data_set_string(settings, "clone", "");
		return;
	}
	if (!context->clone)
		return;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return;
	obs_data_set_string(settings, "clone", obs_source_get_name(source));
	obs_source_release(source);
}

void source_clone_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct source_clone *context = data;
	context->processed_frame = false;

	if (context->clone_type == CLONE_CURRENT_SCENE) {
		obs_source_t *source = obs_frontend_get_current_scene();
		if (!obs_weak_source_references_source(context->clone,
						       source)) {
			source_clone_switch_source(context, source);
		}
		obs_source_release(source);
	} else if (context->clone_type == CLONE_PREVIOUS_SCENE) {
		obs_source_t *current_scene = obs_frontend_get_current_scene();
		if (!obs_weak_source_references_source(context->current_scene,
						       current_scene)) {
			obs_source_t *source = obs_weak_source_get_source(
				context->current_scene);
			source_clone_switch_source(context, source);
			obs_source_release(source);

			obs_weak_source_release(context->current_scene);
			context->current_scene =
				obs_source_get_weak_source(current_scene);
		}
		obs_source_release(current_scene);
	}
	if (context->buffer_frame > 0) {
		uint32_t cx = context->buffer_frame;
		uint32_t cy = context->buffer_frame;
		if (context->clone) {
			obs_source_t *s =
				obs_weak_source_get_source(context->clone);
			if (s) {
				context->source_cx = obs_source_get_width(s);
				context->source_cy = obs_source_get_height(s);
				cx = context->source_cx;
				cy = context->source_cy;
				obs_source_release(s);
			}
		}
		if (context->buffer_frame > 1) {
			cx /= context->buffer_frame;
			cy /= context->buffer_frame;
		}
		if (cx != context->cx || cy != context->cy) {
			context->cx = cx;
			context->cy = cy;
			obs_enter_graphics();
			gs_texrender_destroy(context->render);
			context->render = NULL;
			obs_leave_graphics();
		}
	}
	if (!context->audio_enabled)
		return;

	const audio_t *a = obs_get_audio();
	const struct audio_output_info *aoi = audio_output_get_info(a);

	pthread_mutex_lock(&context->audio_mutex);
	while (context->audio_frames.size > 0) {
		struct obs_source_audio audio;
		audio.format = aoi->format;
		audio.samples_per_sec = aoi->samples_per_sec;
		audio.speakers = aoi->speakers;
		circlebuf_pop_front(&context->audio_frames, &audio.frames,
				    sizeof(audio.frames));
		circlebuf_pop_front(&context->audio_timestamps,
				    &audio.timestamp, sizeof(audio.timestamp));
		for (size_t i = 0; i < context->num_channels; i++) {
			audio.data[i] = (uint8_t *)context->audio_data[i].data +
					context->audio_data[i].start_pos;
		}
		obs_source_output_audio(context->source, &audio);
		for (size_t i = 0; i < context->num_channels; i++) {
			circlebuf_pop_front(&context->audio_data[i], NULL,
					    audio.frames * sizeof(float));
		}
	}
	context->num_channels = audio_output_get_channels(a);
	pthread_mutex_unlock(&context->audio_mutex);
}

struct obs_source_info source_clone_info = {
	.id = "source-clone",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |
			OBS_SOURCE_AUDIO,
	.get_name = source_clone_get_name,
	.create = source_clone_create,
	.destroy = source_clone_destroy,
	.load = source_clone_update,
	.update = source_clone_update,
	.save = source_clone_save,
	.video_render = source_clone_video_render,
	.get_width = source_clone_get_width,
	.get_height = source_clone_get_height,
	.video_tick = source_clone_video_tick,
	.show = source_clone_show,
	.hide = source_clone_hide,
	.activate = source_clone_activate,
	.deactivate = source_clone_deactivate,
	.get_defaults = source_clone_defaults,
	.get_properties = source_clone_properties,
};

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("source-clone", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("SourceClone");
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Source Clone] loaded version %s", PROJECT_VERSION);
	obs_register_source(&source_clone_info);
	obs_register_source(&audio_wrapper_source);
	return true;
}
