#include "m3u8_builder.h"
#include "../manifest_utils.h"

// macros
#define M3U8_HEADER_PART1 "#EXTM3U\n#EXT-X-TARGETDURATION:%d\n#EXT-X-ALLOW-CACHE:YES\n"
#define M3U8_HEADER_VOD "#EXT-X-PLAYLIST-TYPE:VOD\n"
#define M3U8_HEADER_PART2 "#EXT-X-VERSION:%d\n#EXT-X-MEDIA-SEQUENCE:%uD\n"

#define M3U8_ALTERNATIVE_AUDIO_PART1 "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",LANGUAGE=\"%s\",NAME=\"%V\","
#define M3U8_ALTERNATIVE_AUDIO_PART2_DEFAULT "AUTOSELECT=YES,DEFAULT=YES,URI=\""
#define M3U8_ALTERNATIVE_AUDIO_PART2_NON_DEFAULT "AUTOSELECT=NO,DEFAULT=NO,URI=\""
#define M3U8_ALTERNATIVE_AUDIO_STREAM_TAG ",AUDIO=\"audio\""

// constants
static const u_char m3u8_header[] = "#EXTM3U\n";
static const u_char m3u8_footer[] = "#EXT-X-ENDLIST\n";
static const char m3u8_stream_inf_video[] = "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%uD,RESOLUTION=%uDx%uD,CODECS=\"%V";
static const char m3u8_stream_inf_audio[] = "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%uD,CODECS=\"%V";
static const u_char m3u8_discontinuity[] = "#EXT-X-DISCONTINUITY\n";
static const char byte_range_tag_format[] = "#EXT-X-BYTERANGE:%uD@%uD\n";
static const u_char m3u8_url_suffix[] = ".m3u8";

static const char encryption_key_tag_method[] = "#EXT-X-KEY:METHOD=";
static const char encryption_key_tag_uri[] = ",URI=\"";
static const char encryption_key_tag_key_format[] = ",KEYFORMAT=\"";
static const char encryption_key_tag_key_format_versions[] = ",KEYFORMATVERSIONS=\"";
static const char encryption_key_extension[] = ".key";
static const char encryption_type_aes_128[] = "AES-128";
static const char encryption_type_sample_aes[] = "SAMPLE-AES";

// typedefs
typedef struct {
	u_char* p;
	vod_str_t tracks_spec;
	vod_str_t* base_url;
	vod_str_t* segment_file_name_prefix;
} write_segment_context_t;

// Notes: 
//	1. not using vod_sprintf in order to avoid the use of floats
//  2. scale must be a power of 10

static u_char* 
m3u8_builder_format_double(u_char* p, uint32_t n, uint32_t scale)
{
	int cur_digit;
	int int_n = n / scale;
	int fraction = n % scale;

	p = vod_sprintf(p, "%d", int_n);

	if (scale == 1)
	{
		return p;
	}

	*p++ = '.';
	for (;;)
	{
		scale /= 10;
		if (scale == 0)
		{
			break;
		}
		cur_digit = fraction / scale;
		*p++ = cur_digit + '0';
		fraction -= cur_digit * scale;
	}
	return p;
}


static u_char*
m3u8_builder_append_segment_name_ex(
                                 u_char* p,
                                 vod_str_t* base_url,
                                 vod_str_t* segment_file_name_prefix,
                                 uint64_t segment_start_time,
                                 uint32_t segment_duration_millis,
                                 uint32_t segment_index,
                                 vod_str_t* tracks_spec)
{
    p = vod_copy(p, base_url->data, base_url->len);
    p = vod_copy(p, segment_file_name_prefix->data, segment_file_name_prefix->len);
    *p++ = '-';
    p = vod_sprintf(p, "%uL-", segment_start_time);
    p = vod_sprintf(p, "%uD-", segment_duration_millis);
    p = vod_sprintf(p, "%uD", segment_index + 1);
    p = vod_copy(p, tracks_spec->data, tracks_spec->len);
    p = vod_copy(p, ".ts\n", sizeof(".ts\n") - 1);
    return p;
}

static u_char*
m3u8_builder_append_segment_name(
	u_char* p, 
	vod_str_t* base_url,
	vod_str_t* segment_file_name_prefix, 
	uint32_t segment_index, 
	vod_str_t* tracks_spec)
{
	p = vod_copy(p, base_url->data, base_url->len);
	p = vod_copy(p, segment_file_name_prefix->data, segment_file_name_prefix->len);
	*p++ = '-';
	p = vod_sprintf(p, "%uD", segment_index + 1);
	p = vod_copy(p, tracks_spec->data, tracks_spec->len);
	p = vod_copy(p, ".ts\n", sizeof(".ts\n") - 1);
	return p;
}

static u_char*
m3u8_builder_append_extinf_tag(u_char* p, uint32_t duration, uint32_t scale)
{
	p = vod_copy(p, "#EXTINF:", sizeof("#EXTINF:") - 1);
	p = m3u8_builder_format_double(p, duration, scale);
	*p++ = ',';
	*p++ = '\n';
	return p;
}

static void
m3u8_builder_append_iframe_string(void* context, uint32_t segment_index, uint32_t frame_duration, uint32_t frame_start, uint32_t frame_size)
{
	write_segment_context_t* ctx = (write_segment_context_t*)context;

	ctx->p = m3u8_builder_append_extinf_tag(ctx->p, frame_duration, 1000);
	ctx->p = vod_sprintf(ctx->p, byte_range_tag_format, frame_size, frame_start);
	ctx->p = m3u8_builder_append_segment_name(
		ctx->p, 
		ctx->base_url,
		ctx->segment_file_name_prefix, 
		segment_index, 
		&ctx->tracks_spec);
}

static uint32_t
m3u8_builder_get_sequences_mask(media_set_t* media_set)
{
	media_sequence_t* cur_sequence;
	uint32_t result;

	if (!media_set->has_multi_sequences)
	{
		return 0xffffffff;
	}

	result = 0;
	for (cur_sequence = media_set->sequences;
		cur_sequence < media_set->sequences_end;
		cur_sequence++)
	{
		result |= (1 << cur_sequence->index);
	}

	return result;
}

vod_status_t
m3u8_builder_build_iframe_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	hls_muxer_conf_t* muxer_conf,
	vod_str_t* base_url,
	request_params_t* request_params,
	media_set_t* media_set,
	vod_str_t* result)
{
	hls_encryption_params_t encryption_params;
	write_segment_context_t ctx;
	segment_durations_t segment_durations;
	segmenter_conf_t* segmenter_conf = media_set->segmenter_conf;
	size_t iframe_length;
	size_t result_size;
	uint64_t duration_millis;
	uint32_t sequences_mask;
	vod_status_t rc; 

	sequences_mask = m3u8_builder_get_sequences_mask(media_set);

	// iframes list is not supported with encryption, since:
	// 1. AES-128 - the IV of each key frame is not known in advance
	// 2. SAMPLE-AES - the layout of the TS files is not known in advance due to emulation prevention
	encryption_params.type = HLS_ENC_NONE;
	encryption_params.key = NULL;
	encryption_params.iv = NULL;

	// build the required tracks string
	rc = manifest_utils_build_request_params_string(
		request_context, 
		media_set->track_count,
		INVALID_SEGMENT_INDEX,
		sequences_mask,
		request_params->sequence_tracks_mask,
		request_params->tracks_mask,
		&ctx.tracks_spec);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get segment durations
	if (segmenter_conf->align_to_key_frames)
	{
		rc = segmenter_get_segment_durations_accurate(
			request_context,
			segmenter_conf,
			media_set,
			NULL,
			MEDIA_TYPE_NONE,
			&segment_durations);
	}
	else
	{
		rc = segmenter_get_segment_durations_estimate(
			request_context,
			segmenter_conf,
			media_set,
			NULL,
			MEDIA_TYPE_NONE,
			&segment_durations);
	}

	if (rc != VOD_OK)
	{
		return rc;
	}

	duration_millis = segment_durations.end_time - segment_durations.start_time;
	iframe_length = sizeof("#EXTINF:.000,\n") - 1 + vod_get_int_print_len(vod_div_ceil(duration_millis, 1000)) +
		sizeof(byte_range_tag_format) + VOD_INT32_LEN + vod_get_int_print_len(MAX_FRAME_SIZE) - (sizeof("%uD%uD") - 1) +
		base_url->len + conf->segment_file_name_prefix.len + 1 + vod_get_int_print_len(segment_durations.segment_count) + ctx.tracks_spec.len + sizeof(".ts\n") - 1;

	result_size =
		conf->iframes_m3u8_header_len +
		iframe_length * media_set->sequences[0].video_key_frame_count +
		sizeof(m3u8_footer);

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_iframe_playlist: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// fill out the buffer
	ctx.p = vod_copy(result->data, conf->iframes_m3u8_header, conf->iframes_m3u8_header_len);

	if (media_set->sequences[0].video_key_frame_count > 0)
	{
		ctx.base_url = base_url;
		ctx.segment_file_name_prefix = &conf->segment_file_name_prefix;
	
		rc = hls_muxer_simulate_get_iframes(
			request_context,
			&segment_durations, 
			muxer_conf,
			&encryption_params,
			media_set, 
			m3u8_builder_append_iframe_string, 
			&ctx);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}

	ctx.p = vod_copy(ctx.p, m3u8_footer, sizeof(m3u8_footer) - 1);
	result->len = ctx.p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_iframe_playlist: result length %uz exceeded allocated length %uz", 
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

vod_status_t
m3u8_builder_build_index_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	vod_str_t* segments_base_url,
	request_params_t* request_params,
	hls_encryption_params_t* encryption_params,
	media_set_t* media_set,
	vod_str_t* result)
{
	segment_durations_t segment_durations;
	segment_duration_item_t* cur_item;
	segment_duration_item_t* last_item;
	segmenter_conf_t* segmenter_conf = media_set->segmenter_conf;
	uint64_t duration_millis;
	uint32_t sequences_mask;
	vod_str_t extinf;
	uint32_t segment_index;
	uint32_t last_segment_index;
	vod_str_t tracks_spec;
	uint32_t scale;
	size_t segment_length;
	size_t result_size;
	vod_status_t rc;
	u_char* p;
    
    uint64_t dtsStart;
    uint32_t segment_duration;
    

	sequences_mask = m3u8_builder_get_sequences_mask(media_set);

	// build the required tracks string
	rc = manifest_utils_build_request_params_string(
		request_context,
		media_set->track_count,
		INVALID_SEGMENT_INDEX,
		sequences_mask,
		request_params->sequence_tracks_mask,
		request_params->tracks_mask,
		&tracks_spec);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the segment durations
	rc = segmenter_conf->get_segment_durations(
		request_context,
		segmenter_conf,
		media_set,
		NULL,
		MEDIA_TYPE_NONE,
		&segment_durations);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the required buffer length
	duration_millis = segment_durations.end_time - segment_durations.start_time;
	last_segment_index = media_set->initial_segment_index + segment_durations.segment_count;
  
    dtsStart = segment_durations.start_time;
    
	segment_length = sizeof("#EXTINF:.000,\n") - 1 + vod_get_int_print_len(vod_div_ceil(duration_millis, 1000)) +
		segments_base_url->len + conf->segment_file_name_prefix.len + 1
    + vod_get_int_print_len(dtsStart) + 2 + vod_get_int_print_len(duration_millis) // additional data: dts_start_time + "-" + segment_duration
    + vod_get_int_print_len(last_segment_index) + tracks_spec.len + sizeof(".ts\n") - 1;

  	result_size =
		sizeof(M3U8_HEADER_PART1) + VOD_INT64_LEN +
		sizeof(M3U8_HEADER_VOD) +
		sizeof(M3U8_HEADER_PART2) + VOD_INT64_LEN + VOD_INT32_LEN +
		segment_length * segment_durations.segment_count +
		segment_durations.discontinuities * (sizeof(m3u8_discontinuity) - 1) +
		sizeof(m3u8_footer);

	if (encryption_params->type != HLS_ENC_NONE)
	{
		result_size +=
			sizeof(encryption_key_tag_method) - 1 +
			sizeof(encryption_type_sample_aes) - 1 +
			sizeof(encryption_key_tag_uri) - 1 + 
			2;			// '"', '\n'

		if (encryption_params->key_uri.len != 0)
		{
			result_size += encryption_params->key_uri.len;
		}
		else
		{
			result_size += base_url->len +
				conf->encryption_key_file_name.len +
				sizeof("-f") - 1 + VOD_INT32_LEN +
				sizeof(encryption_key_extension) - 1;
		}

		if (conf->encryption_key_format.len != 0)
		{
			result_size +=
				sizeof(encryption_key_tag_key_format) +				// '"'
				conf->encryption_key_format.len;
		}

		if (conf->encryption_key_format_versions.len != 0)
		{
			result_size +=
				sizeof(encryption_key_tag_key_format_versions) +	// '"'
				conf->encryption_key_format_versions.len;
		}
	}

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_index_playlist: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}

	// write the header
	p = vod_sprintf(
		result->data,
		M3U8_HEADER_PART1,
		(segmenter_conf->max_segment_duration + 500) / 1000);

	if (media_set->type == MEDIA_SET_VOD)
	{
		p = vod_copy(p, M3U8_HEADER_VOD, sizeof(M3U8_HEADER_VOD) - 1);
	}

	if (encryption_params->type != HLS_ENC_NONE)
	{
		p = vod_copy(p, encryption_key_tag_method, sizeof(encryption_key_tag_method) - 1);
		switch (encryption_params->type)
		{
		case HLS_ENC_SAMPLE_AES:
			p = vod_copy(p, encryption_type_sample_aes, sizeof(encryption_type_sample_aes) - 1);
			break;

		default:		// HLS_ENC_AES_128
			p = vod_copy(p, encryption_type_aes_128, sizeof(encryption_type_aes_128) - 1);
			break;
		}

		// uri
		p = vod_copy(p, encryption_key_tag_uri, sizeof(encryption_key_tag_uri) - 1);
		if (encryption_params->key_uri.len != 0)
		{
			p = vod_copy(p, encryption_params->key_uri.data, encryption_params->key_uri.len);
		}
		else
		{
			p = vod_copy(p, base_url->data, base_url->len);
			p = vod_copy(p, conf->encryption_key_file_name.data, conf->encryption_key_file_name.len);
			if (media_set->has_multi_sequences)
			{
				p = vod_sprintf(p, "-f%uD", media_set->sequences->index + 1);
			}
			p = vod_copy(p, encryption_key_extension, sizeof(encryption_key_extension) - 1);
		}
		*p++ = '"';

		// keyformat
		if (conf->encryption_key_format.len != 0)
		{
			p = vod_copy(p, encryption_key_tag_key_format, sizeof(encryption_key_tag_key_format) - 1);
			p = vod_copy(p, conf->encryption_key_format.data, conf->encryption_key_format.len);
			*p++ = '"';
		}

		// keyformatversions
		if (conf->encryption_key_format_versions.len != 0)
		{
			p = vod_copy(p, encryption_key_tag_key_format_versions, sizeof(encryption_key_tag_key_format_versions) - 1);
			p = vod_copy(p, conf->encryption_key_format_versions.data, conf->encryption_key_format_versions.len);
			*p++ = '"';
		}

		*p++ = '\n';
	}

	p = vod_sprintf(
		p,
		M3U8_HEADER_PART2,
		conf->m3u8_version, 
		media_set->initial_segment_index + 1);

	// write the segments
	scale = conf->m3u8_version >= 3 ? 1000 : 1;
	last_item = segment_durations.items + segment_durations.item_count;

	for (cur_item = segment_durations.items; cur_item < last_item; cur_item++)
	{
		segment_index = cur_item->segment_index;
		last_segment_index = segment_index + cur_item->repeat_count;

		if (cur_item->discontinuity)
		{
			p = vod_copy(p, m3u8_discontinuity, sizeof(m3u8_discontinuity) - 1);
		}

        segment_duration = rescale_time(cur_item->duration, segment_durations.timescale, scale);
		// write the first segment
		extinf.data = p;
		p = m3u8_builder_append_extinf_tag(p, rescale_time(cur_item->duration, segment_durations.timescale, scale), scale);
		extinf.len = p - extinf.data;
		p = m3u8_builder_append_segment_name_ex(p, segments_base_url, &conf->segment_file_name_prefix, dtsStart, segment_duration, segment_index, &tracks_spec);
		segment_index++;
        dtsStart += segment_duration;
	
        // write any additional segments
		for (; segment_index < last_segment_index; segment_index++)
		{
			p = vod_copy(p, extinf.data, extinf.len);
			p = m3u8_builder_append_segment_name_ex(p, segments_base_url, &conf->segment_file_name_prefix, dtsStart, segment_duration, segment_index, &tracks_spec);
            dtsStart += segment_duration;
		}
	}

	// write the footer
	if (media_set->presentation_end)
	{
		p = vod_copy(p, m3u8_footer, sizeof(m3u8_footer) - 1);
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_index_playlist: result length %uz exceeded allocated length %uz", 
			result->len, result_size);
		return VOD_UNEXPECTED;
	}
	
	return VOD_OK;
}

static u_char*
m3u8_builder_append_index_url(
	u_char* p,
	m3u8_config_t* conf,
	media_set_t* media_set,
	media_track_t** tracks,
	vod_str_t* base_url)
{
	media_track_t* main_track;
	media_track_t* sub_track;
	bool_t write_sequence_index;

	// get the main track and sub track
	main_track = tracks[MEDIA_TYPE_VIDEO];
	if (main_track != NULL)
	{
		sub_track = tracks[MEDIA_TYPE_AUDIO];
	}
	else
	{
		main_track = tracks[MEDIA_TYPE_AUDIO];
		sub_track = NULL;
	}

	write_sequence_index = media_set->has_multi_sequences;
	if (base_url->len != 0)
	{
		// absolute url only
		p = vod_copy(p, base_url->data, base_url->len);
		if (main_track->file_info.uri.len != 0 &&
			(sub_track == NULL || vod_str_equals(main_track->file_info.uri, sub_track->file_info.uri)))
		{
			p = vod_copy(p, main_track->file_info.uri.data, main_track->file_info.uri.len);
			write_sequence_index = FALSE;		// no need to pass the sequence index since we have a direct uri
		}
		else
		{
			p = vod_copy(p, media_set->uri.data, media_set->uri.len);
		}
		*p++ = '/';
	}

	p = vod_copy(p, conf->index_file_name_prefix.data, conf->index_file_name_prefix.len);
	p = manifest_utils_append_tracks_spec(p, tracks, write_sequence_index);
	p = vod_copy(p, m3u8_url_suffix, sizeof(m3u8_url_suffix) - 1);

	return p;
}

vod_status_t
m3u8_builder_build_master_playlist(
	request_context_t* request_context,
	m3u8_config_t* conf,
	vod_str_t* base_url,
	media_set_t* media_set,
	vod_str_t* result)
{
	adaptation_sets_t adaptation_sets;
	adaptation_set_t* first_audio_adaptation_set = NULL;
	adaptation_set_t* adaptation_set;
	media_track_t** cur_track_ptr;
	media_track_t* cur_track;
	media_track_t* tracks[MEDIA_TYPE_COUNT];
	media_info_t* video;
	media_info_t* audio = NULL;
	vod_status_t rc;
	uint32_t muxed_tracks;
	uint32_t bitrate;
	size_t max_video_stream_inf;
	size_t base_url_len;
	size_t result_size;
	u_char* p;

	// get the adaptations sets
	rc = manifest_utils_get_adaptation_sets(
		request_context, 
		media_set, 
		ADAPTATION_SETS_FLAG_MUXED | ADAPTATION_SETS_FLAG_SINGLE_LANG_TRACK, 
		&adaptation_sets);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// get the response size
	base_url_len = base_url->len + 1 + conf->index_file_name_prefix.len +			// 1 = /
		MANIFEST_UTILS_TRACKS_SPEC_MAX_SIZE + sizeof(m3u8_url_suffix) - 1;

	result_size = sizeof(m3u8_header);

	if (adaptation_sets.total_count > 1)
	{
		// alternative audio
		// Note: in case of audio only, the first track is printed twice - once as #EXT-X-STREAM-INF
		//		and once as #EXT-X-MEDIA
		first_audio_adaptation_set = adaptation_sets.first + adaptation_sets.count[MEDIA_TYPE_VIDEO];
		for (adaptation_set = first_audio_adaptation_set;
			adaptation_set < adaptation_sets.last; 
			adaptation_set++)
		{
			cur_track = adaptation_set->first[0];

			result_size += cur_track->media_info.label.len;

			if (base_url->len != 0)
			{
				result_size += vod_max(cur_track->file_info.uri.len, media_set->uri.len);
			}
		}

		result_size +=
			sizeof("\n\n") - 1 +
			(sizeof(M3U8_ALTERNATIVE_AUDIO_PART1) - 1 + LANG_ISO639_1_LEN +
			sizeof(M3U8_ALTERNATIVE_AUDIO_PART2_DEFAULT) - 1 +
			base_url_len +
			sizeof("\"\n") - 1) * (adaptation_sets.total_count - adaptation_sets.count[MEDIA_TYPE_VIDEO]);
	}

	// variants
	muxed_tracks = adaptation_sets.first->type == ADAPTATION_TYPE_MUXED ? MEDIA_TYPE_COUNT : 1;

	if (base_url->len != 0)
	{
		for (cur_track_ptr = adaptation_sets.first->first;
			cur_track_ptr < adaptation_sets.first->last;
			cur_track_ptr += muxed_tracks)
		{
			cur_track = cur_track_ptr[0];
			if (cur_track == NULL)
			{
				cur_track = cur_track_ptr[1];
			}

			result_size += vod_max(cur_track->file_info.uri.len, media_set->uri.len);
		}
	}

	max_video_stream_inf =
		sizeof(m3u8_stream_inf_video) - 1 + 3 * VOD_INT32_LEN + MAX_CODEC_NAME_SIZE +
			MAX_CODEC_NAME_SIZE + 1 +		// 1 = ,
		sizeof("\"\n\n") - 1;
	if (adaptation_sets.total_count > 1)
	{
		max_video_stream_inf += sizeof(M3U8_ALTERNATIVE_AUDIO_STREAM_TAG) - 1;
	}

	result_size +=
		(max_video_stream_inf +		 // using only video since it's larger than audio
		base_url_len) * adaptation_sets.first->count;

	// allocate the buffer
	result->data = vod_alloc(request_context->pool, result_size);
	if (result->data == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"m3u8_builder_build_master_playlist: vod_alloc failed (2)");
		return VOD_ALLOC_FAILED;
	}

	// write the header
	p = vod_copy(result->data, m3u8_header, sizeof(m3u8_header) - 1);

	if (adaptation_sets.total_count > 1)
	{
		// output alternative audio 
		*p++ = '\n';

		tracks[MEDIA_TYPE_VIDEO] = NULL;
		for (adaptation_set = first_audio_adaptation_set;
			adaptation_set < adaptation_sets.last; 
			adaptation_set++)
		{
			// take only the first track
			tracks[MEDIA_TYPE_AUDIO] = adaptation_set->first[0];

			// output EXT-X-MEDIA
			p = vod_sprintf(p, M3U8_ALTERNATIVE_AUDIO_PART1,
				lang_get_iso639_1_name(tracks[MEDIA_TYPE_AUDIO]->media_info.language),
				&tracks[MEDIA_TYPE_AUDIO]->media_info.label);
			if (adaptation_set == first_audio_adaptation_set)
			{
				p = vod_copy(p, M3U8_ALTERNATIVE_AUDIO_PART2_DEFAULT, sizeof(M3U8_ALTERNATIVE_AUDIO_PART2_DEFAULT) - 1);
			}
			else
			{
				p = vod_copy(p, M3U8_ALTERNATIVE_AUDIO_PART2_NON_DEFAULT, sizeof(M3U8_ALTERNATIVE_AUDIO_PART2_NON_DEFAULT) - 1);
			}

			p = m3u8_builder_append_index_url(
				p,
				conf,
				media_set,
				tracks,
				base_url);

			*p++ = '"';
			*p++ = '\n';
		}

		*p++ = '\n';
	}

	// output variants
	for (cur_track_ptr = adaptation_sets.first->first;
		cur_track_ptr < adaptation_sets.first->last;
		cur_track_ptr += muxed_tracks)
	{
		// get the audio / video tracks
		if (muxed_tracks == 2)
		{
			tracks[MEDIA_TYPE_VIDEO] = cur_track_ptr[MEDIA_TYPE_VIDEO];
			tracks[MEDIA_TYPE_AUDIO] = cur_track_ptr[MEDIA_TYPE_AUDIO];
		}
		else
		{
			if (adaptation_sets.first->type == MEDIA_TYPE_VIDEO)
			{
				tracks[MEDIA_TYPE_VIDEO] = cur_track_ptr[0];
				tracks[MEDIA_TYPE_AUDIO] = NULL;
			}
			else
			{
				tracks[MEDIA_TYPE_VIDEO] = NULL;
				tracks[MEDIA_TYPE_AUDIO] = cur_track_ptr[0];
			}
		}

		// output EXT-X-STREAM-INF
		if (tracks[MEDIA_TYPE_VIDEO] != NULL)
		{
			video = &tracks[MEDIA_TYPE_VIDEO]->media_info;
			bitrate = video->bitrate;
			if (tracks[MEDIA_TYPE_AUDIO] != NULL)
			{
				audio = &tracks[MEDIA_TYPE_AUDIO]->media_info;
				bitrate += audio->bitrate;
			}
			p = vod_sprintf(p, m3u8_stream_inf_video,
				bitrate,
				(uint32_t)video->u.video.width,
				(uint32_t)video->u.video.height,
				&video->codec_name);
			if (tracks[MEDIA_TYPE_AUDIO] != NULL)
			{
				*p++ = ',';
				p = vod_copy(p, audio->codec_name.data, audio->codec_name.len);
			}
		}
		else
		{
			audio = &tracks[MEDIA_TYPE_AUDIO]->media_info;
			p = vod_sprintf(p, m3u8_stream_inf_audio, audio->bitrate, &audio->codec_name);
		}

		*p++ = '\"';
		if (adaptation_sets.total_count > 1)
		{
			p = vod_copy(p, M3U8_ALTERNATIVE_AUDIO_STREAM_TAG, sizeof(M3U8_ALTERNATIVE_AUDIO_STREAM_TAG) - 1);
		}
		*p++ = '\n';

		// output the url
		p = m3u8_builder_append_index_url(
			p,
			conf,
			media_set,
			tracks,
			base_url);

		*p++ = '\n';
	}

	result->len = p - result->data;

	if (result->len > result_size)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"m3u8_builder_build_master_playlist: result length %uz exceeded allocated length %uz",
			result->len, result_size);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

void 
m3u8_builder_init_config(
	m3u8_config_t* conf, 
	uint32_t max_segment_duration, 
	hls_encryption_type_t encryption_method)
{
	if (encryption_method == HLS_ENC_SAMPLE_AES ||
		conf->encryption_key_format.len != 0 ||
		conf->encryption_key_format_versions.len != 0)
	{
		conf->m3u8_version = 5;
	}
	else
	{
		conf->m3u8_version = 3;
	}

	conf->iframes_m3u8_header_len = vod_snprintf(
		conf->iframes_m3u8_header,
		sizeof(conf->iframes_m3u8_header) - 1,
		iframes_m3u8_header_format,
		vod_div_ceil(max_segment_duration, 1000)) - conf->iframes_m3u8_header;
}
