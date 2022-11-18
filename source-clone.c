#include "source-clone.h"
#include <obs-module.h>

#include <util/threading.h>
#include <util/circlebuf.h>

struct source_clone {
	obs_source_t *source;
	obs_weak_source_t *clone;
	struct circlebuf audio_data[MAX_AUDIO_CHANNELS];
	struct circlebuf audio_frames;
	struct circlebuf audio_timestamps;
	size_t num_channels;
	pthread_mutex_t audio_mutex;
};

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

static void source_clone_destroy(void *data)
{
	struct source_clone *context = data;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (source) {
		obs_source_remove_audio_capture_callback(
			source, source_clone_audio_callback, data);
		if(obs_source_showing(context->source))
			obs_source_dec_showing(source);
		obs_source_release(source);
	}
	obs_weak_source_release(context->clone);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		circlebuf_free(&context->audio_data[i]);
	}
	circlebuf_free(&context->audio_frames);
	circlebuf_free(&context->audio_timestamps);
	pthread_mutex_destroy(&context->audio_mutex);
	bfree(context);
}

void source_clone_update(void *data, obs_data_t *settings)
{
	struct source_clone *context = data;

	const char *source_name = obs_data_get_string(settings, "clone");
	obs_source_t *source = obs_get_source_by_name(source_name);
	if (source == context->source) {
		obs_source_release(source);
		source = NULL;
	}
	if (source) {
		if (!obs_weak_source_references_source(context->clone,
						       source)) {
			obs_source_t *prev_source =
				obs_weak_source_get_source(context->clone);
			if (prev_source) {
				obs_source_remove_audio_capture_callback(
					prev_source,
					source_clone_audio_callback, data);
				if (obs_source_showing(context->source))
					obs_source_dec_showing(prev_source);
				obs_source_release(prev_source);
			}
			obs_weak_source_release(context->clone);
			context->clone = obs_source_get_weak_source(source);
			obs_source_add_audio_capture_callback(
				source, source_clone_audio_callback, data);
			if (obs_source_showing(context->source))
				obs_source_inc_showing(source);
		}
		obs_source_release(source);
	}
	context->num_channels = audio_output_get_channels(obs_get_audio());
}

void source_clone_defaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
}

bool source_clone_list_add_source(void *data, obs_source_t *source)
{
	obs_property_t *prop = data;

	const char * name = obs_source_get_name(source);
	size_t count = obs_property_list_item_count(prop);
	size_t idx = 0;
	while(idx < count && strcmp(name, obs_property_list_item_string(prop, idx)) > 0)
		idx++;
	obs_property_list_insert_string(prop, idx, name, name);
	return true;
}

obs_properties_t *source_clone_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "clone", "Clone",
						    OBS_COMBO_TYPE_EDITABLE,
						    OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(source_clone_list_add_source, p);
	return props;
}

void source_clone_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct source_clone *context = data;
	if (!context->clone)
		return;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return;
	obs_source_video_render(source);
	obs_source_release(source);
}

uint32_t source_clone_get_width(void *data)
{
	struct source_clone *context = data;
	if (!context->clone)
		return 1;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return 1;
	const uint32_t width = obs_source_get_width(source);
	obs_source_release(source);
	return width;
}

uint32_t source_clone_get_height(void *data)
{
	struct source_clone *context = data;
	if (!context->clone)
		return 1;
	obs_source_t *source = obs_weak_source_get_source(context->clone);
	if (!source)
		return 1;
	const uint32_t height = obs_source_get_height(source);
	obs_source_release(source);
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

void source_clone_save(void *data, obs_data_t *settings)
{
	struct source_clone *context = data;
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
	return true;
}
