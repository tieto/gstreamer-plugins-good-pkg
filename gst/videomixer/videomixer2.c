/* Generic video mixer plugin
 * Copyright (C) 2004, 2008 Wim Taymans <wim@fluendo.com>
 * Copyright (C) 2010 Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-videomixer
 *
 * Videomixer2 can accept AYUV, ARGB and BGRA video streams. For each of the requested
 * sink pads it will compare the incoming geometry and framerate to define the
 * output parameters. Indeed output video frames will have the geometry of the
 * biggest incoming video stream and the framerate of the fastest incoming one.
 *
 * All sink pads must be either AYUV, ARGB or BGRA, but a mixture of them is not 
 * supported. The src pad will have the same colorspace as the sinks. 
 * No colorspace conversion is done. 
 * 
 * Individual parameters for each input stream can be configured on the
 * #GstVideoMixer2Pad.
 *
 * <refsect2>
 * <title>Sample pipelines</title>
 * |[
 * gst-launch-0.11 \
 *   videotestsrc pattern=1 ! \
 *   video/x-raw,format=AYUV,framerate=\(fraction\)10/1,width=100,height=100 ! \
 *   videobox border-alpha=0 top=-70 bottom=-70 right=-220 ! \
 *   videomixer name=mix sink_0::alpha=0.7 sink_1::alpha=0.5 ! \
 *   videoconvert ! xvimagesink \
 *   videotestsrc ! \
 *   video/x-raw,format=AYUV,framerate=\(fraction\)5/1,width=320,height=240 ! mix.
 * ]| A pipeline to demonstrate videomixer used together with videobox.
 * This should show a 320x240 pixels video test source with some transparency
 * showing the background checker pattern. Another video test source with just
 * the snow pattern of 100x100 pixels is overlayed on top of the first one on
 * the left vertically centered with a small transparency showing the first
 * video test source behind and the checker pattern under it. Note that the
 * framerate of the output video is 10 frames per second.
 * |[
 * gst-launch-0.11 videotestsrc pattern=1 ! \
 *   video/x-raw, framerate=\(fraction\)10/1, width=100, height=100 ! \
 *   videomixer name=mix ! videoconvert ! ximagesink \
 *   videotestsrc !  \
 *   video/x-raw, framerate=\(fraction\)5/1, width=320, height=240 ! mix.
 * ]| A pipeline to demostrate bgra mixing. (This does not demonstrate alpha blending). 
 * |[
 * gst-launch-0.11 videotestsrc pattern=1 ! \
 *   video/x-raw,format =I420, framerate=\(fraction\)10/1, width=100, height=100 ! \
 *   videomixer name=mix ! videoconvert ! ximagesink \
 *   videotestsrc ! \
 *   video/x-raw,format=I420, framerate=\(fraction\)5/1, width=320, height=240 ! mix.
 * ]| A pipeline to test I420
 * |[
 * gst-launch-0.11 videomixer name=mixer sink_1::alpha=0.5 sink_1::xpos=50 sink_1::ypos=50 ! \
 *   videoconvert ! ximagesink \
 *   videotestsrc pattern=snow timestamp-offset=3000000000 ! \
 *   "video/x-raw,format=AYUV,width=640,height=480,framerate=(fraction)30/1" ! \
 *   timeoverlay ! queue2 ! mixer. \
 *   videotestsrc pattern=smpte ! \
 *   "video/x-raw,format=AYUV,width=800,height=600,framerate=(fraction)10/1" ! \
 *   timeoverlay ! queue2 ! mixer.
 * ]| A pipeline to demonstrate synchronized mixing (the second stream starts after 3 seconds)
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "videomixer2.h"
#include "videomixer2pad.h"

#ifdef DISABLE_ORC
#define orc_memset memset
#else
#include <orc/orcfunctions.h>
#endif

GST_DEBUG_CATEGORY_STATIC (gst_videomixer2_debug);
#define GST_CAT_DEFAULT gst_videomixer2_debug

#define GST_VIDEO_MIXER2_GET_LOCK(mix) \
  (&GST_VIDEO_MIXER2(mix)->lock)
#define GST_VIDEO_MIXER2_LOCK(mix) \
  (g_mutex_lock(GST_VIDEO_MIXER2_GET_LOCK (mix)))
#define GST_VIDEO_MIXER2_UNLOCK(mix) \
  (g_mutex_unlock(GST_VIDEO_MIXER2_GET_LOCK (mix)))

#define FORMATS " { AYUV, BGRA, ARGB, RGBA, ABGR, Y444, Y42B, YUY2, UYVY, "\
                "   YVYU, I420, YV12, Y41B, RGB, BGR, xRGB, xBGR, RGBx, BGRx } "

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (FORMATS))
    );

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink_%d",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (FORMATS))
    );

static void gst_videomixer2_child_proxy_init (gpointer g_iface,
    gpointer iface_data);
static gboolean gst_videomixer2_push_sink_event (GstVideoMixer2 * mix,
    GstEvent * event);
static void gst_videomixer2_release_pad (GstElement * element, GstPad * pad);
static void gst_videomixer2_reset_qos (GstVideoMixer2 * mix);

struct _GstVideoMixer2Collect
{
  GstCollectData2 collect;      /* we extend the CollectData */

  GstVideoMixer2Pad *mixpad;

  GstBuffer *queued;            /* buffer for which we don't know the end time yet */

  GstBuffer *buffer;            /* buffer that should be blended now */
  GstClockTime start_time;
  GstClockTime end_time;
};

#define DEFAULT_PAD_ZORDER 0
#define DEFAULT_PAD_XPOS   0
#define DEFAULT_PAD_YPOS   0
#define DEFAULT_PAD_ALPHA  1.0
enum
{
  PROP_PAD_0,
  PROP_PAD_ZORDER,
  PROP_PAD_XPOS,
  PROP_PAD_YPOS,
  PROP_PAD_ALPHA
};

G_DEFINE_TYPE (GstVideoMixer2Pad, gst_videomixer2_pad, GST_TYPE_PAD);

static void
gst_videomixer2_collect_free (GstCollectData2 * data)
{
  GstVideoMixer2Collect *cdata = (GstVideoMixer2Collect *) data;

  gst_buffer_replace (&cdata->buffer, NULL);
}

static gboolean gst_videomixer2_src_setcaps (GstPad * pad, GstVideoMixer2 * mix,
    GstCaps * caps);

static gboolean
gst_videomixer2_update_src_caps (GstVideoMixer2 * mix)
{
  GSList *l;
  gint best_width = -1, best_height = -1;
  gdouble best_fps = -1, cur_fps;
  gint best_fps_n = -1, best_fps_d = -1;
  gboolean ret = TRUE;

  GST_VIDEO_MIXER2_LOCK (mix);

  for (l = mix->sinkpads; l; l = l->next) {
    GstVideoMixer2Pad *mpad = l->data;
    gint this_width, this_height;
    gint fps_n, fps_d;
    gint width, height;

    fps_n = GST_VIDEO_INFO_FPS_N (&mpad->info);
    fps_d = GST_VIDEO_INFO_FPS_D (&mpad->info);
    width = GST_VIDEO_INFO_WIDTH (&mpad->info);
    height = GST_VIDEO_INFO_HEIGHT (&mpad->info);

    if (fps_n == 0 || fps_d == 0 || width == 0 || height == 0)
      continue;

    this_width = width + MAX (mpad->xpos, 0);
    this_height = height + MAX (mpad->ypos, 0);

    if (best_width < this_width)
      best_width = this_width;
    if (best_height < this_height)
      best_height = this_height;

    if (fps_d == 0)
      cur_fps = 0.0;
    else
      gst_util_fraction_to_double (fps_n, fps_d, &cur_fps);

    if (best_fps < cur_fps) {
      best_fps = cur_fps;
      best_fps_n = fps_n;
      best_fps_d = fps_d;
    }
  }

  if (best_fps_n <= 0 && best_fps_d <= 0) {
    best_fps_n = 25;
    best_fps_d = 1;
    best_fps = 25.0;
  }

  if (best_width > 0 && best_height > 0 && best_fps > 0) {
    GstCaps *caps, *peercaps;
    GstStructure *s;
    GstVideoInfo info;

    if (GST_VIDEO_INFO_FPS_N (&mix->info) != best_fps_n ||
        GST_VIDEO_INFO_FPS_D (&mix->info) != best_fps_d) {
      if (mix->segment.position != -1) {
        mix->ts_offset = mix->segment.position - mix->segment.start;
        mix->nframes = 0;
      }
    }
    gst_video_info_set_format (&info, GST_VIDEO_INFO_FORMAT (&mix->info),
        best_width, best_height);
    info.fps_n = best_fps_n;
    info.fps_d = best_fps_d;
    info.par_n = GST_VIDEO_INFO_PAR_N (&mix->info);
    info.par_d = GST_VIDEO_INFO_PAR_D (&mix->info);

    caps = gst_video_info_to_caps (&info);

    peercaps = gst_pad_peer_query_caps (mix->srcpad, NULL);
    if (peercaps) {
      GstCaps *tmp;

      s = gst_caps_get_structure (caps, 0);
      gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT, "height",
          GST_TYPE_INT_RANGE, 1, G_MAXINT, "framerate", GST_TYPE_FRACTION_RANGE,
          0, 1, G_MAXINT, 1, NULL);

      tmp = gst_caps_intersect (caps, peercaps);
      gst_caps_unref (caps);
      gst_caps_unref (peercaps);
      caps = tmp;
      if (gst_caps_is_empty (caps)) {
        GST_DEBUG_OBJECT (mix, "empty caps");
        ret = FALSE;
        GST_VIDEO_MIXER2_UNLOCK (mix);
        goto done;
      }

      caps = gst_caps_truncate (caps);
      s = gst_caps_get_structure (caps, 0);
      gst_structure_fixate_field_nearest_int (s, "width", best_width);
      gst_structure_fixate_field_nearest_int (s, "height", best_height);
      gst_structure_fixate_field_nearest_fraction (s, "framerate", best_fps_n,
          best_fps_d);

      gst_structure_get_int (s, "width", &info.width);
      gst_structure_get_int (s, "height", &info.height);
      gst_structure_get_fraction (s, "fraction", &info.fps_n, &info.fps_d);
    }

    caps = gst_video_info_to_caps (&info);

    GST_VIDEO_MIXER2_UNLOCK (mix);
    ret = gst_videomixer2_src_setcaps (mix->srcpad, mix, caps);
    gst_caps_unref (caps);
  } else {
    GST_VIDEO_MIXER2_UNLOCK (mix);
  }

done:
  return ret;
}


static gboolean
gst_videomixer2_pad_sink_setcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  GstVideoMixer2 *mix;
  GstVideoMixer2Pad *mixpad;
  GstVideoInfo info;
  gboolean ret = FALSE;

  GST_INFO_OBJECT (pad, "Setting caps %" GST_PTR_FORMAT, caps);

  mix = GST_VIDEO_MIXER2 (parent);
  mixpad = GST_VIDEO_MIXER2_PAD (pad);

  if (!gst_video_info_from_caps (&info, caps)) {
    GST_ERROR_OBJECT (pad, "Failed to parse caps");
    goto beach;
  }

  GST_VIDEO_MIXER2_LOCK (mix);
  if (GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN) {
    if (GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_INFO_FORMAT (&info) ||
        GST_VIDEO_INFO_PAR_N (&mix->info) != GST_VIDEO_INFO_PAR_N (&info) ||
        GST_VIDEO_INFO_PAR_D (&mix->info) != GST_VIDEO_INFO_PAR_D (&info)) {
      GST_ERROR_OBJECT (pad, "Caps not compatible with other pads' caps");
      GST_VIDEO_MIXER2_UNLOCK (mix);
      goto beach;
    }
  }

  mix->info = info;
  mixpad->info = info;

  GST_VIDEO_MIXER2_UNLOCK (mix);

  ret = gst_videomixer2_update_src_caps (mix);

beach:
  return ret;
}

static GstCaps *
gst_videomixer2_pad_sink_getcaps (GstPad * pad, GstObject * parent,
    GstCaps * filter)
{
  GstVideoMixer2 *mix;
  GstCaps *srccaps;
  GstStructure *s;
  gint i, n;

  mix = GST_VIDEO_MIXER2 (parent);

  srccaps = gst_pad_get_current_caps (GST_PAD (mix->srcpad));
  if (srccaps == NULL)
    srccaps = gst_pad_get_pad_template_caps (GST_PAD (mix->srcpad));

  srccaps = gst_caps_make_writable (srccaps);

  n = gst_caps_get_size (srccaps);
  for (i = 0; i < n; i++) {
    s = gst_caps_get_structure (srccaps, i);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    if (!gst_structure_has_field (s, "pixel-aspect-ratio"))
      gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);
  }

  GST_DEBUG_OBJECT (pad, "Returning %" GST_PTR_FORMAT, srccaps);

  return srccaps;
}

static gboolean
gst_videomixer2_pad_sink_acceptcaps (GstPad * pad, GstObject * parent,
    GstCaps * caps)
{
  gboolean ret;
  GstVideoMixer2 *mix;
  GstCaps *accepted_caps;
  gint i, n;
  GstStructure *s;

  mix = GST_VIDEO_MIXER2 (parent);
  GST_DEBUG_OBJECT (pad, "%" GST_PTR_FORMAT, caps);

  accepted_caps = gst_pad_get_current_caps (GST_PAD (mix->srcpad));
  if (accepted_caps == NULL)
    accepted_caps = gst_pad_get_pad_template_caps (GST_PAD (mix->srcpad));

  accepted_caps = gst_caps_make_writable (accepted_caps);
  GST_LOG_OBJECT (pad, "src caps %" GST_PTR_FORMAT, accepted_caps);

  n = gst_caps_get_size (accepted_caps);
  for (i = 0; i < n; i++) {
    s = gst_caps_get_structure (accepted_caps, i);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    if (!gst_structure_has_field (s, "pixel-aspect-ratio"))
      gst_structure_set (s, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);
  }

  ret = gst_caps_can_intersect (caps, accepted_caps);
  GST_INFO_OBJECT (pad, "%saccepted caps %" GST_PTR_FORMAT, (ret ? "" : "not "),
      caps);
  GST_INFO_OBJECT (pad, "acceptable caps are %" GST_PTR_FORMAT, accepted_caps);
  gst_caps_unref (accepted_caps);

  return ret;
}

static gboolean
gst_videomixer2_pad_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);
      caps = gst_videomixer2_pad_sink_getcaps (pad, parent, filter);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      ret = TRUE;
      break;
    }
    case GST_QUERY_ACCEPT_CAPS:
    {
      GstCaps *caps;

      gst_query_parse_accept_caps (query, &caps);
      ret = gst_videomixer2_pad_sink_acceptcaps (pad, parent, caps);
      gst_query_set_accept_caps_result (query, ret);
      ret = TRUE;
      break;
    }
    default:
      ret = gst_pad_query_default (pad, parent, query);
      break;
  }
  return ret;
}

static void
gst_videomixer2_pad_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVideoMixer2Pad *pad = GST_VIDEO_MIXER2_PAD (object);

  switch (prop_id) {
    case PROP_PAD_ZORDER:
      g_value_set_uint (value, pad->zorder);
      break;
    case PROP_PAD_XPOS:
      g_value_set_int (value, pad->xpos);
      break;
    case PROP_PAD_YPOS:
      g_value_set_int (value, pad->ypos);
      break;
    case PROP_PAD_ALPHA:
      g_value_set_double (value, pad->alpha);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static int
pad_zorder_compare (const GstVideoMixer2Pad * pad1,
    const GstVideoMixer2Pad * pad2)
{
  return pad1->zorder - pad2->zorder;
}

static void
gst_videomixer2_pad_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVideoMixer2Pad *pad = GST_VIDEO_MIXER2_PAD (object);
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (gst_pad_get_parent (GST_PAD (pad)));

  switch (prop_id) {
    case PROP_PAD_ZORDER:
      GST_VIDEO_MIXER2_LOCK (mix);
      pad->zorder = g_value_get_uint (value);

      mix->sinkpads = g_slist_sort (mix->sinkpads,
          (GCompareFunc) pad_zorder_compare);
      GST_VIDEO_MIXER2_UNLOCK (mix);
      break;
    case PROP_PAD_XPOS:
      pad->xpos = g_value_get_int (value);
      break;
    case PROP_PAD_YPOS:
      pad->ypos = g_value_get_int (value);
      break;
    case PROP_PAD_ALPHA:
      pad->alpha = g_value_get_double (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  gst_object_unref (mix);
}

static void
gst_videomixer2_pad_class_init (GstVideoMixer2PadClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = gst_videomixer2_pad_set_property;
  gobject_class->get_property = gst_videomixer2_pad_get_property;

  g_object_class_install_property (gobject_class, PROP_PAD_ZORDER,
      g_param_spec_uint ("zorder", "Z-Order", "Z Order of the picture",
          0, 10000, DEFAULT_PAD_ZORDER,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAD_XPOS,
      g_param_spec_int ("xpos", "X Position", "X Position of the picture",
          G_MININT, G_MAXINT, DEFAULT_PAD_XPOS,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAD_YPOS,
      g_param_spec_int ("ypos", "Y Position", "Y Position of the picture",
          G_MININT, G_MAXINT, DEFAULT_PAD_YPOS,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PAD_ALPHA,
      g_param_spec_double ("alpha", "Alpha", "Alpha of the picture", 0.0, 1.0,
          DEFAULT_PAD_ALPHA,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));
}

static void
gst_videomixer2_pad_init (GstVideoMixer2Pad * mixerpad)
{
  /* setup some pad functions */
  gst_pad_set_query_function (GST_PAD (mixerpad),
      gst_videomixer2_pad_sink_query);

  mixerpad->zorder = DEFAULT_PAD_ZORDER;
  mixerpad->xpos = DEFAULT_PAD_XPOS;
  mixerpad->ypos = DEFAULT_PAD_YPOS;
  mixerpad->alpha = DEFAULT_PAD_ALPHA;
}

/* GstVideoMixer2 */
#define DEFAULT_BACKGROUND VIDEO_MIXER2_BACKGROUND_CHECKER
enum
{
  PROP_0,
  PROP_BACKGROUND
};

#define GST_TYPE_VIDEO_MIXER2_BACKGROUND (gst_videomixer2_background_get_type())
static GType
gst_videomixer2_background_get_type (void)
{
  static GType video_mixer_background_type = 0;

  static const GEnumValue video_mixer_background[] = {
    {VIDEO_MIXER2_BACKGROUND_CHECKER, "Checker pattern", "checker"},
    {VIDEO_MIXER2_BACKGROUND_BLACK, "Black", "black"},
    {VIDEO_MIXER2_BACKGROUND_WHITE, "White", "white"},
    {VIDEO_MIXER2_BACKGROUND_TRANSPARENT,
        "Transparent Background to enable further mixing", "transparent"},
    {0, NULL, NULL},
  };

  if (!video_mixer_background_type) {
    video_mixer_background_type =
        g_enum_register_static ("GstVideoMixer2Background",
        video_mixer_background);
  }
  return video_mixer_background_type;
}

#define gst_videomixer2_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVideoMixer2, gst_videomixer2, GST_TYPE_ELEMENT,
    G_IMPLEMENT_INTERFACE (GST_TYPE_CHILD_PROXY,
        gst_videomixer2_child_proxy_init));

static void
gst_videomixer2_update_qos (GstVideoMixer2 * mix, gdouble proportion,
    GstClockTimeDiff diff, GstClockTime timestamp)
{
  GST_DEBUG_OBJECT (mix,
      "Updating QoS: proportion %lf, diff %s%" GST_TIME_FORMAT ", timestamp %"
      GST_TIME_FORMAT, proportion, (diff < 0) ? "-" : "",
      GST_TIME_ARGS (ABS (diff)), GST_TIME_ARGS (timestamp));

  GST_OBJECT_LOCK (mix);
  mix->proportion = proportion;
  if (G_LIKELY (timestamp != GST_CLOCK_TIME_NONE)) {
    if (G_UNLIKELY (diff > 0))
      mix->earliest_time =
          timestamp + 2 * diff + gst_util_uint64_scale_int (GST_SECOND,
          GST_VIDEO_INFO_FPS_D (&mix->info), GST_VIDEO_INFO_FPS_N (&mix->info));
    else
      mix->earliest_time = timestamp + diff;
  } else {
    mix->earliest_time = GST_CLOCK_TIME_NONE;
  }
  GST_OBJECT_UNLOCK (mix);
}

static void
gst_videomixer2_reset_qos (GstVideoMixer2 * mix)
{
  gst_videomixer2_update_qos (mix, 0.5, 0, GST_CLOCK_TIME_NONE);
  mix->qos_processed = mix->qos_dropped = 0;
}

static void
gst_videomixer2_read_qos (GstVideoMixer2 * mix, gdouble * proportion,
    GstClockTime * time)
{
  GST_OBJECT_LOCK (mix);
  *proportion = mix->proportion;
  *time = mix->earliest_time;
  GST_OBJECT_UNLOCK (mix);
}

static void
gst_videomixer2_reset (GstVideoMixer2 * mix)
{
  GSList *l;

  gst_video_info_init (&mix->info);
  mix->ts_offset = 0;
  mix->nframes = 0;

  gst_segment_init (&mix->segment, GST_FORMAT_TIME);
  mix->segment.position = -1;

  gst_videomixer2_reset_qos (mix);

  for (l = mix->sinkpads; l; l = l->next) {
    GstVideoMixer2Pad *p = l->data;
    GstVideoMixer2Collect *mixcol = p->mixcol;

    gst_buffer_replace (&mixcol->buffer, NULL);
    mixcol->start_time = -1;
    mixcol->end_time = -1;

    gst_video_info_init (&p->info);
  }

  mix->newseg_pending = TRUE;
  mix->flush_stop_pending = FALSE;
}

/*  1 == OK
 *  0 == need more data
 * -1 == EOS
 * -2 == error
 */
static gint
gst_videomixer2_fill_queues (GstVideoMixer2 * mix,
    GstClockTime output_start_time, GstClockTime output_end_time)
{
  GSList *l;
  gboolean eos = TRUE;
  gboolean need_more_data = FALSE;

  for (l = mix->sinkpads; l; l = l->next) {
    GstVideoMixer2Pad *pad = l->data;
    GstVideoMixer2Collect *mixcol = pad->mixcol;
    GstSegment *segment = &pad->mixcol->collect.segment;
    GstBuffer *buf;

    buf = gst_collect_pads2_peek (mix->collect, &mixcol->collect);
    if (buf) {
      GstClockTime start_time, end_time;

      start_time = GST_BUFFER_TIMESTAMP (buf);
      if (start_time == -1) {
        gst_buffer_unref (buf);
        GST_ERROR_OBJECT (pad, "Need timestamped buffers!");
        return -2;
      }

      /* FIXME: Make all this work with negative rates */

      if ((mixcol->buffer && start_time < GST_BUFFER_TIMESTAMP (mixcol->buffer))
          || (mixcol->queued
              && start_time < GST_BUFFER_TIMESTAMP (mixcol->queued))) {
        GST_WARNING_OBJECT (pad, "Buffer from the past, dropping");
        gst_buffer_unref (buf);
        buf = gst_collect_pads2_pop (mix->collect, &mixcol->collect);
        gst_buffer_unref (buf);
        need_more_data = TRUE;
        continue;
      }

      if (mixcol->queued) {
        end_time = start_time - GST_BUFFER_TIMESTAMP (mixcol->queued);
        start_time = GST_BUFFER_TIMESTAMP (mixcol->queued);
        gst_buffer_unref (buf);
        buf = gst_buffer_ref (mixcol->queued);
      } else {
        end_time = GST_BUFFER_DURATION (buf);

        if (end_time == -1) {
          mixcol->queued = buf;
          need_more_data = TRUE;
          continue;
        }
      }

      g_assert (start_time != -1 && end_time != -1);
      end_time += start_time;   /* convert from duration to position */

      if (mixcol->end_time != -1 && mixcol->end_time > end_time) {
        GST_WARNING_OBJECT (pad, "Buffer from the past, dropping");
        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads2_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }

      /* Check if it's inside the segment */
      if (start_time >= segment->stop || end_time < segment->start) {
        GST_DEBUG_OBJECT (pad, "Buffer outside the segment");

        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads2_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }

      /* Clip to segment and convert to running time */
      start_time = MAX (start_time, segment->start);
      if (segment->stop != -1)
        end_time = MIN (end_time, segment->stop);
      start_time =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME, start_time);
      end_time =
          gst_segment_to_running_time (segment, GST_FORMAT_TIME, end_time);
      g_assert (start_time != -1 && end_time != -1);

      /* Convert to the output segment rate */
      if (ABS (mix->segment.rate) != 1.0) {
        start_time *= ABS (mix->segment.rate);
        end_time *= ABS (mix->segment.rate);
      }

      if (end_time >= output_start_time && start_time < output_end_time) {
        GST_DEBUG_OBJECT (pad,
            "Taking new buffer with start time %" GST_TIME_FORMAT,
            GST_TIME_ARGS (start_time));
        gst_buffer_replace (&mixcol->buffer, buf);
        mixcol->start_time = start_time;
        mixcol->end_time = end_time;

        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads2_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }
        eos = FALSE;
      } else if (start_time >= output_end_time) {
        GST_DEBUG_OBJECT (pad, "Keeping buffer until %" GST_TIME_FORMAT,
            GST_TIME_ARGS (start_time));
        gst_buffer_unref (buf);
        eos = FALSE;
      } else {
        GST_DEBUG_OBJECT (pad, "Too old buffer -- dropping");
        if (buf == mixcol->queued) {
          gst_buffer_unref (buf);
          gst_buffer_replace (&mixcol->queued, NULL);
        } else {
          gst_buffer_unref (buf);
          buf = gst_collect_pads2_pop (mix->collect, &mixcol->collect);
          gst_buffer_unref (buf);
        }

        need_more_data = TRUE;
        continue;
      }
    } else {
      if (mixcol->end_time != -1) {
        if (mixcol->end_time < output_start_time) {
          gst_buffer_replace (&mixcol->buffer, NULL);
          mixcol->start_time = mixcol->end_time = -1;
          if (!GST_COLLECT_PADS2_STATE_IS_SET (mixcol,
                  GST_COLLECT_PADS2_STATE_EOS))
            need_more_data = TRUE;
        } else {
          eos = FALSE;
        }
      }
    }
  }

  if (need_more_data)
    return 0;
  if (eos)
    return -1;

  return 1;
}

static GstFlowReturn
gst_videomixer2_blend_buffers (GstVideoMixer2 * mix,
    GstClockTime output_start_time, GstClockTime output_end_time,
    GstBuffer ** outbuf)
{
  GSList *l;
  guint outsize;
  BlendFunction composite;
  GstVideoFrame outframe;
  static GstAllocationParams params = { 0, 0, 0, 15, };

  outsize = GST_VIDEO_INFO_SIZE (&mix->info);

  *outbuf = gst_buffer_new_allocate (NULL, outsize, &params);
  GST_BUFFER_TIMESTAMP (*outbuf) = output_start_time;
  GST_BUFFER_DURATION (*outbuf) = output_end_time - output_start_time;

  gst_video_frame_map (&outframe, &mix->info, *outbuf, GST_MAP_READWRITE);

  /* default to blending */
  composite = mix->blend;
  switch (mix->background) {
    case VIDEO_MIXER2_BACKGROUND_CHECKER:
      mix->fill_checker (&outframe);
      break;
    case VIDEO_MIXER2_BACKGROUND_BLACK:
      mix->fill_color (&outframe, 16, 128, 128);
      break;
    case VIDEO_MIXER2_BACKGROUND_WHITE:
      mix->fill_color (&outframe, 240, 128, 128);
      break;
    case VIDEO_MIXER2_BACKGROUND_TRANSPARENT:
      gst_buffer_memset (*outbuf, 0, 0, outsize);
      /* use overlay to keep background transparent */
      composite = mix->overlay;
      break;
  }

  for (l = mix->sinkpads; l; l = l->next) {
    GstVideoMixer2Pad *pad = l->data;
    GstVideoMixer2Collect *mixcol = pad->mixcol;

    if (mixcol->buffer != NULL) {
      GstClockTime timestamp;
      gint64 stream_time;
      GstSegment *seg;
      GstVideoFrame frame;

      seg = &mixcol->collect.segment;

      timestamp = GST_BUFFER_TIMESTAMP (mixcol->buffer);

      stream_time =
          gst_segment_to_stream_time (seg, GST_FORMAT_TIME, timestamp);

      /* sync object properties on stream time */
      if (GST_CLOCK_TIME_IS_VALID (stream_time))
        gst_object_sync_values (GST_OBJECT (pad), stream_time);

      gst_video_frame_map (&frame, &pad->info, mixcol->buffer, GST_MAP_READ);

      composite (&frame, pad->xpos, pad->ypos, pad->alpha, &outframe);

      gst_video_frame_unmap (&frame);
    }
  }
  gst_video_frame_unmap (&outframe);

  return GST_FLOW_OK;
}

/* Perform qos calculations before processing the next frame. Returns TRUE if
 * the frame should be processed, FALSE if the frame can be dropped entirely */
static gint64
gst_videomixer2_do_qos (GstVideoMixer2 * mix, GstClockTime timestamp)
{
  GstClockTime qostime, earliest_time;
  gdouble proportion;
  gint64 jitter;

  /* no timestamp, can't do QoS => process frame */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (timestamp))) {
    GST_LOG_OBJECT (mix, "invalid timestamp, can't do QoS, process frame");
    return -1;
  }

  /* get latest QoS observation values */
  gst_videomixer2_read_qos (mix, &proportion, &earliest_time);

  /* skip qos if we have no observation (yet) => process frame */
  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (earliest_time))) {
    GST_LOG_OBJECT (mix, "no observation yet, process frame");
    return -1;
  }

  /* qos is done on running time */
  qostime =
      gst_segment_to_running_time (&mix->segment, GST_FORMAT_TIME, timestamp);

  /* see how our next timestamp relates to the latest qos timestamp */
  GST_LOG_OBJECT (mix, "qostime %" GST_TIME_FORMAT ", earliest %"
      GST_TIME_FORMAT, GST_TIME_ARGS (qostime), GST_TIME_ARGS (earliest_time));

  jitter = GST_CLOCK_DIFF (qostime, earliest_time);
  if (qostime != GST_CLOCK_TIME_NONE && jitter > 0) {
    GST_DEBUG_OBJECT (mix, "we are late, drop frame");
    return jitter;
  }

  GST_LOG_OBJECT (mix, "process frame");
  return jitter;
}

static GstFlowReturn
gst_videomixer2_collected (GstCollectPads2 * pads, GstVideoMixer2 * mix)
{
  GstFlowReturn ret;
  GstClockTime output_start_time, output_end_time;
  GstBuffer *outbuf = NULL;
  gint res;
  gint64 jitter;

  /* If we're not negotiated yet... */
  if (GST_VIDEO_INFO_FORMAT (&mix->info) == GST_VIDEO_FORMAT_UNKNOWN)
    return GST_FLOW_NOT_NEGOTIATED;

  if (g_atomic_int_compare_and_exchange (&mix->flush_stop_pending, TRUE, FALSE)) {
    GST_DEBUG_OBJECT (mix, "pending flush stop");
    gst_pad_push_event (mix->srcpad, gst_event_new_flush_stop (TRUE));
  }

  GST_VIDEO_MIXER2_LOCK (mix);

  if (mix->newseg_pending) {
    GST_DEBUG_OBJECT (mix, "Sending NEWSEGMENT event");
    if (!gst_pad_push_event (mix->srcpad,
            gst_event_new_segment (&mix->segment))) {
      ret = GST_FLOW_ERROR;
      goto done;
    }
    mix->newseg_pending = FALSE;
  }

  if (mix->segment.position == -1)
    output_start_time = mix->segment.start;
  else
    output_start_time = mix->segment.position;

  if (output_start_time >= mix->segment.stop) {
    GST_DEBUG_OBJECT (mix, "Segment done");
    gst_pad_push_event (mix->srcpad, gst_event_new_eos ());
    ret = GST_FLOW_EOS;
    goto done;
  }

  output_end_time =
      mix->ts_offset + gst_util_uint64_scale (mix->nframes + 1,
      GST_SECOND * GST_VIDEO_INFO_FPS_D (&mix->info),
      GST_VIDEO_INFO_FPS_N (&mix->info));
  if (mix->segment.stop != -1)
    output_end_time = MIN (output_end_time, mix->segment.stop);

  res = gst_videomixer2_fill_queues (mix, output_start_time, output_end_time);

  if (res == 0) {
    GST_DEBUG_OBJECT (mix, "Need more data for decisions");
    ret = GST_FLOW_OK;
    goto done;
  } else if (res == -1) {
    GST_DEBUG_OBJECT (mix, "All sinkpads are EOS -- forwarding");
    gst_pad_push_event (mix->srcpad, gst_event_new_eos ());
    ret = GST_FLOW_EOS;
    goto done;
  } else if (res == -2) {
    GST_ERROR_OBJECT (mix, "Error collecting buffers");
    ret = GST_FLOW_ERROR;
    goto done;
  }

  jitter = gst_videomixer2_do_qos (mix, output_start_time);
  if (jitter <= 0) {
    ret =
        gst_videomixer2_blend_buffers (mix, output_start_time,
        output_end_time, &outbuf);
    mix->qos_processed++;
  } else {
    GstMessage *msg;

    mix->qos_dropped++;

    /* TODO: live */
    msg =
        gst_message_new_qos (GST_OBJECT_CAST (mix), FALSE,
        gst_segment_to_running_time (&mix->segment, GST_FORMAT_TIME,
            output_start_time), gst_segment_to_stream_time (&mix->segment,
            GST_FORMAT_TIME, output_start_time), output_start_time,
        output_end_time - output_start_time);
    gst_message_set_qos_values (msg, jitter, mix->proportion, 1000000);
    gst_message_set_qos_stats (msg, GST_FORMAT_BUFFERS, mix->qos_processed,
        mix->qos_dropped);
    gst_element_post_message (GST_ELEMENT_CAST (mix), msg);

    ret = GST_FLOW_OK;
  }

  mix->segment.position = output_end_time;
  mix->nframes++;

  GST_VIDEO_MIXER2_UNLOCK (mix);
  if (outbuf) {
    GST_LOG_OBJECT (mix,
        "Pushing buffer with ts %" GST_TIME_FORMAT " and duration %"
        GST_TIME_FORMAT, GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (outbuf)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (outbuf)));
    ret = gst_pad_push (mix->srcpad, outbuf);
  }
  GST_VIDEO_MIXER2_LOCK (mix);

done:
  GST_VIDEO_MIXER2_UNLOCK (mix);

  return ret;
}

static gboolean
gst_videomixer2_query_caps (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstCaps *filter, *caps;
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (parent);
  GstStructure *s;
  gint n;

  gst_query_parse_caps (query, &filter);

  if (GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN) {
    caps = gst_pad_get_current_caps (mix->srcpad);
  } else {
    caps = gst_pad_get_pad_template_caps (mix->srcpad);
  }

  caps = gst_caps_make_writable (caps);

  n = gst_caps_get_size (caps) - 1;
  for (; n >= 0; n--) {
    s = gst_caps_get_structure (caps, n);
    gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);
    if (GST_VIDEO_INFO_FPS_D (&mix->info) != 0) {
      gst_structure_set (s,
          "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1, NULL);
    }
  }
  gst_query_set_caps_result (query, caps);

  return TRUE;
}

static gboolean
gst_videomixer2_query_duration (GstVideoMixer2 * mix, GstQuery * query)
{
  GValue item = { 0 };
  gint64 max;
  gboolean res;
  GstFormat format;
  GstIterator *it;
  gboolean done;

  /* parse format */
  gst_query_parse_duration (query, &format, NULL);

  max = -1;
  res = TRUE;
  done = FALSE;

  /* Take maximum of all durations */
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_OK:
      {
        GstPad *pad;
        gint64 duration;

        pad = g_value_get_object (&item);

        /* ask sink peer for duration */
        res &= gst_pad_peer_query_duration (pad, format, &duration);
        /* take max from all valid return values */
        if (res) {
          /* valid unknown length, stop searching */
          if (duration == -1) {
            max = duration;
            done = TRUE;
          }
          /* else see if bigger than current max */
          else if (duration > max)
            max = duration;
        }
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        max = -1;
        res = TRUE;
        gst_iterator_resync (it);
        break;
      default:
        res = FALSE;
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  if (res) {
    /* and store the max */
    GST_DEBUG_OBJECT (mix, "Total duration in format %s: %"
        GST_TIME_FORMAT, gst_format_get_name (format), GST_TIME_ARGS (max));
    gst_query_set_duration (query, format, max);
  }

  return res;
}

static gboolean
gst_videomixer2_query_latency (GstVideoMixer2 * mix, GstQuery * query)
{
  GstClockTime min, max;
  gboolean live;
  gboolean res;
  GstIterator *it;
  gboolean done;
  GValue item = { 0 };

  res = TRUE;
  done = FALSE;
  live = FALSE;
  min = 0;
  max = GST_CLOCK_TIME_NONE;

  /* Take maximum of all latency values */
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_OK:
      {
        GstPad *pad = g_value_get_object (&item);
        GstQuery *peerquery;
        GstClockTime min_cur, max_cur;
        gboolean live_cur;

        peerquery = gst_query_new_latency ();

        /* Ask peer for latency */
        res &= gst_pad_peer_query (pad, peerquery);

        /* take max from all valid return values */
        if (res) {
          gst_query_parse_latency (peerquery, &live_cur, &min_cur, &max_cur);

          if (min_cur > min)
            min = min_cur;

          if (max_cur != GST_CLOCK_TIME_NONE &&
              ((max != GST_CLOCK_TIME_NONE && max_cur > max) ||
                  (max == GST_CLOCK_TIME_NONE)))
            max = max_cur;

          live = live || live_cur;
        }

        gst_query_unref (peerquery);
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        live = FALSE;
        min = 0;
        max = GST_CLOCK_TIME_NONE;
        res = TRUE;
        gst_iterator_resync (it);
        break;
      default:
        res = FALSE;
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);

  if (res) {
    /* store the results */
    GST_DEBUG_OBJECT (mix, "Calculated total latency: live %s, min %"
        GST_TIME_FORMAT ", max %" GST_TIME_FORMAT,
        (live ? "yes" : "no"), GST_TIME_ARGS (min), GST_TIME_ARGS (max));
    gst_query_set_latency (query, live, min, max);
  }

  return res;
}

static gboolean
gst_videomixer2_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (parent);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_POSITION:
    {
      GstFormat format;

      gst_query_parse_position (query, &format, NULL);

      switch (format) {
        case GST_FORMAT_TIME:
          gst_query_set_position (query, format,
              gst_segment_to_stream_time (&mix->segment, GST_FORMAT_TIME,
                  mix->segment.position));
          res = TRUE;
          break;
        default:
          break;
      }
      break;
    }
    case GST_QUERY_DURATION:
      res = gst_videomixer2_query_duration (mix, query);
      break;
    case GST_QUERY_LATENCY:
      res = gst_videomixer2_query_latency (mix, query);
      break;
    case GST_QUERY_CAPS:
      res = gst_videomixer2_query_caps (pad, parent, query);
      break;
    default:
      /* FIXME, needs a custom query handler because we have multiple
       * sinkpads */
      res = FALSE;
      gst_query_unref (query);
      break;
  }
  return res;
}

static gboolean
gst_videomixer2_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (parent);
  gboolean result;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
    {
      GstQOSType type;
      GstClockTimeDiff diff;
      GstClockTime timestamp;
      gdouble proportion;

      gst_event_parse_qos (event, &type, &proportion, &diff, &timestamp);

      gst_videomixer2_update_qos (mix, proportion, diff, timestamp);

      result = gst_videomixer2_push_sink_event (mix, event);
      break;
    }
    case GST_EVENT_SEEK:
    {
      gdouble rate;
      GstFormat fmt;
      GstSeekFlags flags;
      GstSeekType start_type, stop_type;
      gint64 start, stop;
      GSList *l;
      gdouble abs_rate;

      /* parse the seek parameters */
      gst_event_parse_seek (event, &rate, &fmt, &flags, &start_type,
          &start, &stop_type, &stop);

      if (rate <= 0.0) {
        GST_ERROR_OBJECT (mix, "Negative rates not supported yet");
        result = FALSE;
        gst_event_unref (event);
        break;
      }

      GST_DEBUG_OBJECT (mix, "Handling SEEK event");

      /* check if we are flushing */
      if (flags & GST_SEEK_FLAG_FLUSH) {
        /* flushing seek, start flush downstream, the flush will be done
         * when all pads received a FLUSH_STOP. */
        gst_pad_push_event (mix->srcpad, gst_event_new_flush_start ());

        /* make sure we accept nothing anymore and return WRONG_STATE */
        gst_collect_pads2_set_flushing (mix->collect, TRUE);
      }

      /* now wait for the collected to be finished and mark a new
       * segment */
      GST_COLLECT_PADS2_STREAM_LOCK (mix->collect);

      abs_rate = ABS (rate);

      GST_VIDEO_MIXER2_LOCK (mix);
      for (l = mix->sinkpads; l; l = l->next) {
        GstVideoMixer2Pad *p = l->data;

        if (flags & GST_SEEK_FLAG_FLUSH) {
          gst_buffer_replace (&p->mixcol->buffer, NULL);
          p->mixcol->start_time = p->mixcol->end_time = -1;
          continue;
        }

        /* Convert to the output segment rate */
        if (ABS (mix->segment.rate) != abs_rate) {
          if (ABS (mix->segment.rate) != 1.0 && p->mixcol->buffer) {
            p->mixcol->start_time /= ABS (mix->segment.rate);
            p->mixcol->end_time /= ABS (mix->segment.rate);
          }
          if (abs_rate != 1.0 && p->mixcol->buffer) {
            p->mixcol->start_time *= abs_rate;
            p->mixcol->end_time *= abs_rate;
          }
        }
      }
      GST_VIDEO_MIXER2_UNLOCK (mix);

      gst_segment_do_seek (&mix->segment, rate, fmt, flags, start_type, start,
          stop_type, stop, NULL);
      mix->segment.position = -1;
      mix->ts_offset = 0;
      mix->nframes = 0;
      mix->newseg_pending = TRUE;

      if (flags & GST_SEEK_FLAG_FLUSH) {
        gst_collect_pads2_set_flushing (mix->collect, FALSE);

        /* we can't send FLUSH_STOP here since upstream could start pushing data
         * after we unlock mix->collect.
         * We set flush_stop_pending to TRUE instead and send FLUSH_STOP after
         * forwarding the seek upstream or from gst_videomixer_collected,
         * whichever happens first.
         */
        mix->flush_stop_pending = TRUE;
      }

      GST_COLLECT_PADS2_STREAM_UNLOCK (mix->collect);

      gst_videomixer2_reset_qos (mix);

      result = gst_videomixer2_push_sink_event (mix, event);

      if (g_atomic_int_compare_and_exchange (&mix->flush_stop_pending, TRUE,
              FALSE)) {
        GST_DEBUG_OBJECT (mix, "pending flush stop");
        gst_pad_push_event (mix->srcpad, gst_event_new_flush_stop (TRUE));
      }

      break;
    }
    case GST_EVENT_NAVIGATION:
      /* navigation is rather pointless. */
      result = FALSE;
      gst_event_unref (event);
      break;
    default:
      /* just forward the rest for now */
      result = gst_videomixer2_push_sink_event (mix, event);
      break;
  }

  return result;
}

static gboolean
gst_videomixer2_src_setcaps (GstPad * pad, GstVideoMixer2 * mix, GstCaps * caps)
{
  gboolean ret = FALSE;
  GstVideoInfo info;

  GST_INFO_OBJECT (pad, "set src caps: %" GST_PTR_FORMAT, caps);

  mix->blend = NULL;
  mix->overlay = NULL;
  mix->fill_checker = NULL;
  mix->fill_color = NULL;

  if (!gst_video_info_from_caps (&info, caps))
    goto done;

  GST_VIDEO_MIXER2_LOCK (mix);

  if (GST_VIDEO_INFO_FPS_N (&mix->info) != GST_VIDEO_INFO_FPS_N (&info) ||
      GST_VIDEO_INFO_FPS_D (&mix->info) != GST_VIDEO_INFO_FPS_D (&info)) {
    if (mix->segment.position != -1) {
      mix->ts_offset = mix->segment.position - mix->segment.start;
      mix->nframes = 0;
    }
    gst_videomixer2_reset_qos (mix);
  }

  mix->info = info;

  switch (GST_VIDEO_INFO_FORMAT (&mix->info)) {
    case GST_VIDEO_FORMAT_AYUV:
      mix->blend = gst_video_mixer_blend_ayuv;
      mix->overlay = gst_video_mixer_overlay_ayuv;
      mix->fill_checker = gst_video_mixer_fill_checker_ayuv;
      mix->fill_color = gst_video_mixer_fill_color_ayuv;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_ARGB:
      mix->blend = gst_video_mixer_blend_argb;
      mix->overlay = gst_video_mixer_overlay_argb;
      mix->fill_checker = gst_video_mixer_fill_checker_argb;
      mix->fill_color = gst_video_mixer_fill_color_argb;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_BGRA:
      mix->blend = gst_video_mixer_blend_bgra;
      mix->overlay = gst_video_mixer_overlay_bgra;
      mix->fill_checker = gst_video_mixer_fill_checker_bgra;
      mix->fill_color = gst_video_mixer_fill_color_bgra;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_ABGR:
      mix->blend = gst_video_mixer_blend_abgr;
      mix->overlay = gst_video_mixer_overlay_abgr;
      mix->fill_checker = gst_video_mixer_fill_checker_abgr;
      mix->fill_color = gst_video_mixer_fill_color_abgr;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_RGBA:
      mix->blend = gst_video_mixer_blend_rgba;
      mix->overlay = gst_video_mixer_overlay_rgba;
      mix->fill_checker = gst_video_mixer_fill_checker_rgba;
      mix->fill_color = gst_video_mixer_fill_color_rgba;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_Y444:
      mix->blend = gst_video_mixer_blend_y444;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_y444;
      mix->fill_color = gst_video_mixer_fill_color_y444;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_Y42B:
      mix->blend = gst_video_mixer_blend_y42b;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_y42b;
      mix->fill_color = gst_video_mixer_fill_color_y42b;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_YUY2:
      mix->blend = gst_video_mixer_blend_yuy2;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_yuy2;
      mix->fill_color = gst_video_mixer_fill_color_yuy2;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_UYVY:
      mix->blend = gst_video_mixer_blend_uyvy;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_uyvy;
      mix->fill_color = gst_video_mixer_fill_color_uyvy;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_YVYU:
      mix->blend = gst_video_mixer_blend_yvyu;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_yvyu;
      mix->fill_color = gst_video_mixer_fill_color_yvyu;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_I420:
      mix->blend = gst_video_mixer_blend_i420;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_i420;
      mix->fill_color = gst_video_mixer_fill_color_i420;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_YV12:
      mix->blend = gst_video_mixer_blend_yv12;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_yv12;
      mix->fill_color = gst_video_mixer_fill_color_yv12;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_Y41B:
      mix->blend = gst_video_mixer_blend_y41b;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_y41b;
      mix->fill_color = gst_video_mixer_fill_color_y41b;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_RGB:
      mix->blend = gst_video_mixer_blend_rgb;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_rgb;
      mix->fill_color = gst_video_mixer_fill_color_rgb;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_BGR:
      mix->blend = gst_video_mixer_blend_bgr;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_bgr;
      mix->fill_color = gst_video_mixer_fill_color_bgr;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_xRGB:
      mix->blend = gst_video_mixer_blend_xrgb;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_xrgb;
      mix->fill_color = gst_video_mixer_fill_color_xrgb;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_xBGR:
      mix->blend = gst_video_mixer_blend_xbgr;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_xbgr;
      mix->fill_color = gst_video_mixer_fill_color_xbgr;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_RGBx:
      mix->blend = gst_video_mixer_blend_rgbx;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_rgbx;
      mix->fill_color = gst_video_mixer_fill_color_rgbx;
      ret = TRUE;
      break;
    case GST_VIDEO_FORMAT_BGRx:
      mix->blend = gst_video_mixer_blend_bgrx;
      mix->overlay = mix->blend;
      mix->fill_checker = gst_video_mixer_fill_checker_bgrx;
      mix->fill_color = gst_video_mixer_fill_color_bgrx;
      ret = TRUE;
      break;
    default:
      break;
  }
  GST_VIDEO_MIXER2_UNLOCK (mix);

  ret = gst_pad_set_caps (pad, caps);
done:

  return ret;
}

static GstFlowReturn
gst_videomixer2_sink_clip (GstCollectPads2 * pads,
    GstCollectData2 * data, GstBuffer * buf, GstBuffer ** outbuf,
    GstVideoMixer2 * mix)
{
  GstVideoMixer2Pad *pad = GST_VIDEO_MIXER2_PAD (data->pad);
  GstVideoMixer2Collect *mixcol = pad->mixcol;
  GstClockTime start_time, end_time;

  start_time = GST_BUFFER_TIMESTAMP (buf);
  if (start_time == -1) {
    GST_ERROR_OBJECT (pad, "Timestamped buffers required!");
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  end_time = GST_BUFFER_DURATION (buf);
  if (end_time == -1)
    end_time =
        gst_util_uint64_scale_int (GST_SECOND,
        GST_VIDEO_INFO_FPS_D (&pad->info), GST_VIDEO_INFO_FPS_N (&pad->info));
  if (end_time == -1) {
    *outbuf = buf;
    return GST_FLOW_OK;
  }

  start_time = MAX (start_time, mixcol->collect.segment.start);
  start_time =
      gst_segment_to_running_time (&mixcol->collect.segment,
      GST_FORMAT_TIME, start_time);

  end_time += GST_BUFFER_TIMESTAMP (buf);
  if (mixcol->collect.segment.stop != -1)
    end_time = MIN (end_time, mixcol->collect.segment.stop);
  end_time =
      gst_segment_to_running_time (&mixcol->collect.segment,
      GST_FORMAT_TIME, end_time);

  /* Convert to the output segment rate */
  if (ABS (mix->segment.rate) != 1.0) {
    start_time *= ABS (mix->segment.rate);
    end_time *= ABS (mix->segment.rate);
  }

  if (mixcol->buffer != NULL && end_time < mixcol->end_time) {
    gst_buffer_unref (buf);
    *outbuf = NULL;
    return GST_FLOW_OK;
  }

  *outbuf = buf;
  return GST_FLOW_OK;
}

static gboolean
gst_videomixer2_sink_event (GstCollectPads2 * pads, GstCollectData2 * cdata,
    GstEvent * event, GstVideoMixer2 * mix)
{
  GstVideoMixer2Pad *pad = GST_VIDEO_MIXER2_PAD (cdata->pad);
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (pad, "Got %s event on pad %s:%s",
      GST_EVENT_TYPE_NAME (event), GST_DEBUG_PAD_NAME (pad));

  /* return FALSE => event will be forwarded */
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret =
          gst_videomixer2_pad_sink_setcaps (GST_PAD (pad), GST_OBJECT (mix),
          caps);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:{
      GstSegment seg;
      gst_event_copy_segment (event, &seg);

      g_assert (seg.format == GST_FORMAT_TIME);
      /* eat SEGMENT events */
      ret = TRUE;
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      mix->newseg_pending = TRUE;
      mix->flush_stop_pending = FALSE;
      gst_videomixer2_reset_qos (mix);
      gst_buffer_replace (&pad->mixcol->buffer, NULL);
      pad->mixcol->start_time = -1;
      pad->mixcol->end_time = -1;

      gst_segment_init (&mix->segment, GST_FORMAT_TIME);
      mix->segment.position = -1;
      mix->ts_offset = 0;
      mix->nframes = 0;

      ret = gst_pad_event_default (cdata->pad, GST_OBJECT (mix), event);
      break;
    case GST_EVENT_EOS:
      gst_event_unref (event);
      ret = TRUE;
      break;
    default:
      ret = gst_pad_event_default (cdata->pad, GST_OBJECT (mix), event);
      break;
  }

  return ret;
}

static gboolean
forward_event_func (GValue * item, GValue * ret, GstEvent * event)
{
  GstPad *pad = g_value_get_object (item);
  gst_event_ref (event);
  GST_LOG_OBJECT (pad, "About to send event %s", GST_EVENT_TYPE_NAME (event));
  if (!gst_pad_push_event (pad, event)) {
    g_value_set_boolean (ret, FALSE);
    GST_WARNING_OBJECT (pad, "Sending event  %p (%s) failed.",
        event, GST_EVENT_TYPE_NAME (event));
  } else {
    GST_LOG_OBJECT (pad, "Sent event  %p (%s).",
        event, GST_EVENT_TYPE_NAME (event));
  }
  return TRUE;
}

static gboolean
gst_videomixer2_push_sink_event (GstVideoMixer2 * mix, GstEvent * event)
{
  GstIterator *it;
  GValue vret = { 0 };

  GST_LOG_OBJECT (mix, "Forwarding event %p (%s)", event,
      GST_EVENT_TYPE_NAME (event));

  g_value_init (&vret, G_TYPE_BOOLEAN);
  g_value_set_boolean (&vret, TRUE);
  it = gst_element_iterate_sink_pads (GST_ELEMENT_CAST (mix));
  gst_iterator_fold (it, (GstIteratorFoldFunction) forward_event_func, &vret,
      event);
  gst_iterator_free (it);
  gst_event_unref (event);

  return g_value_get_boolean (&vret);
}

/* GstElement vmethods */
static GstStateChangeReturn
gst_videomixer2_change_state (GstElement * element, GstStateChange transition)
{
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      GST_LOG_OBJECT (mix, "starting collectpads");
      gst_collect_pads2_start (mix->collect);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_LOG_OBJECT (mix, "stopping collectpads");
      gst_collect_pads2_stop (mix->collect);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_videomixer2_reset (mix);
      break;
    default:
      break;
  }

  return ret;
}

static GstPad *
gst_videomixer2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * req_name, const GstCaps * caps)
{
  GstVideoMixer2 *mix;
  GstVideoMixer2Pad *mixpad;
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (element);

  mix = GST_VIDEO_MIXER2 (element);

  if (templ == gst_element_class_get_pad_template (klass, "sink_%d")) {
    gint serial = 0;
    gchar *name = NULL;
    GstVideoMixer2Collect *mixcol = NULL;

    GST_VIDEO_MIXER2_LOCK (mix);
    if (req_name == NULL || strlen (req_name) < 6
        || !g_str_has_prefix (req_name, "sink_")) {
      /* no name given when requesting the pad, use next available int */
      serial = mix->next_sinkpad++;
    } else {
      /* parse serial number from requested padname */
      serial = g_ascii_strtoull (&req_name[5], NULL, 10);
      if (serial >= mix->next_sinkpad)
        mix->next_sinkpad = serial + 1;
    }
    /* create new pad with the name */
    name = g_strdup_printf ("sink_%d", serial);
    mixpad = g_object_new (GST_TYPE_VIDEO_MIXER2_PAD, "name", name, "direction",
        templ->direction, "template", templ, NULL);
    g_free (name);

    mixpad->zorder = mix->numpads;
    mixpad->xpos = DEFAULT_PAD_XPOS;
    mixpad->ypos = DEFAULT_PAD_YPOS;
    mixpad->alpha = DEFAULT_PAD_ALPHA;

    mixcol = (GstVideoMixer2Collect *)
        gst_collect_pads2_add_pad_full (mix->collect, GST_PAD (mixpad),
        sizeof (GstVideoMixer2Collect),
        (GstCollectData2DestroyNotify) gst_videomixer2_collect_free, TRUE);

    /* Keep track of each other */
    mixcol->mixpad = mixpad;
    mixpad->mixcol = mixcol;

    mixcol->start_time = -1;
    mixcol->end_time = -1;

    /* Keep an internal list of mixpads for zordering */
    mix->sinkpads = g_slist_append (mix->sinkpads, mixpad);
    mix->numpads++;
    GST_VIDEO_MIXER2_UNLOCK (mix);
  } else {
    return NULL;
  }

  GST_DEBUG_OBJECT (element, "Adding pad %s", GST_PAD_NAME (mixpad));

  /* add the pad to the element */
  gst_element_add_pad (element, GST_PAD (mixpad));
  gst_child_proxy_child_added (G_OBJECT (mix), G_OBJECT (mixpad),
      GST_OBJECT_NAME (mixpad));

  return GST_PAD (mixpad);
}

static void
gst_videomixer2_release_pad (GstElement * element, GstPad * pad)
{
  GstVideoMixer2 *mix = NULL;
  GstVideoMixer2Pad *mixpad;
  gboolean update_caps;

  mix = GST_VIDEO_MIXER2 (element);

  GST_VIDEO_MIXER2_LOCK (mix);
  if (G_UNLIKELY (g_slist_find (mix->sinkpads, pad) == NULL)) {
    g_warning ("Unknown pad %s", GST_PAD_NAME (pad));
    goto error;
  }

  mixpad = GST_VIDEO_MIXER2_PAD (pad);

  mix->sinkpads = g_slist_remove (mix->sinkpads, pad);
  gst_child_proxy_child_removed (G_OBJECT (mix), G_OBJECT (mixpad),
      GST_OBJECT_NAME (mixpad));
  mix->numpads--;

  update_caps = GST_VIDEO_INFO_FORMAT (&mix->info) != GST_VIDEO_FORMAT_UNKNOWN;
  GST_VIDEO_MIXER2_UNLOCK (mix);

  gst_collect_pads2_remove_pad (mix->collect, pad);

  if (update_caps)
    gst_videomixer2_update_src_caps (mix);

  gst_element_remove_pad (element, pad);
  return;
error:
  GST_VIDEO_MIXER2_UNLOCK (mix);
}

/* GObject vmethods */
static void
gst_videomixer2_finalize (GObject * o)
{
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (o);

  gst_object_unref (mix->collect);
  g_mutex_clear (&mix->lock);

  G_OBJECT_CLASS (parent_class)->finalize (o);
}

static void
gst_videomixer2_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (object);

  switch (prop_id) {
    case PROP_BACKGROUND:
      g_value_set_enum (value, mix->background);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_videomixer2_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (object);

  switch (prop_id) {
    case PROP_BACKGROUND:
      mix->background = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstChildProxy implementation */
static GObject *
gst_videomixer2_child_proxy_get_child_by_index (GstChildProxy * child_proxy,
    guint index)
{
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (child_proxy);
  GObject *obj;

  GST_VIDEO_MIXER2_LOCK (mix);
  if ((obj = g_slist_nth_data (mix->sinkpads, index)))
    g_object_ref (obj);
  GST_VIDEO_MIXER2_UNLOCK (mix);
  return obj;
}

static guint
gst_videomixer2_child_proxy_get_children_count (GstChildProxy * child_proxy)
{
  guint count = 0;
  GstVideoMixer2 *mix = GST_VIDEO_MIXER2 (child_proxy);

  GST_VIDEO_MIXER2_LOCK (mix);
  count = mix->numpads;
  GST_VIDEO_MIXER2_UNLOCK (mix);
  GST_INFO_OBJECT (mix, "Children Count: %d", count);
  return count;
}

static void
gst_videomixer2_child_proxy_init (gpointer g_iface, gpointer iface_data)
{
  GstChildProxyInterface *iface = g_iface;

  GST_INFO ("intializing child proxy interface");
  iface->get_child_by_index = gst_videomixer2_child_proxy_get_child_by_index;
  iface->get_children_count = gst_videomixer2_child_proxy_get_children_count;
}

/* GObject boilerplate */
static void
gst_videomixer2_class_init (GstVideoMixer2Class * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  gobject_class->finalize = gst_videomixer2_finalize;

  gobject_class->get_property = gst_videomixer2_get_property;
  gobject_class->set_property = gst_videomixer2_set_property;

  g_object_class_install_property (gobject_class, PROP_BACKGROUND,
      g_param_spec_enum ("background", "Background", "Background type",
          GST_TYPE_VIDEO_MIXER2_BACKGROUND,
          DEFAULT_BACKGROUND, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_videomixer2_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_videomixer2_release_pad);
  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_videomixer2_change_state);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_factory));

  gst_element_class_set_static_metadata (gstelement_class, "Video mixer 2",
      "Filter/Editor/Video",
      "Mix multiple video streams", "Wim Taymans <wim@fluendo.com>, "
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");

  /* Register the pad class */
  g_type_class_ref (GST_TYPE_VIDEO_MIXER2_PAD);
}

static void
gst_videomixer2_init (GstVideoMixer2 * mix)
{
  GstElementClass *klass = GST_ELEMENT_GET_CLASS (mix);

  mix->srcpad =
      gst_pad_new_from_template (gst_element_class_get_pad_template (klass,
          "src"), "src");
  gst_pad_set_query_function (GST_PAD (mix->srcpad),
      GST_DEBUG_FUNCPTR (gst_videomixer2_src_query));
  gst_pad_set_event_function (GST_PAD (mix->srcpad),
      GST_DEBUG_FUNCPTR (gst_videomixer2_src_event));
  gst_element_add_pad (GST_ELEMENT (mix), mix->srcpad);

  mix->collect = gst_collect_pads2_new ();
  mix->background = DEFAULT_BACKGROUND;

  gst_collect_pads2_set_function (mix->collect,
      (GstCollectPads2Function) GST_DEBUG_FUNCPTR (gst_videomixer2_collected),
      mix);
  gst_collect_pads2_set_event_function (mix->collect,
      (GstCollectPads2EventFunction) gst_videomixer2_sink_event, mix);
  gst_collect_pads2_set_clip_function (mix->collect,
      (GstCollectPads2ClipFunction) gst_videomixer2_sink_clip, mix);

  g_mutex_init (&mix->lock);
  /* initialize variables */
  gst_videomixer2_reset (mix);
}

/* Element registration */
static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_videomixer2_debug, "videomixer", 0,
      "video mixer");

  gst_video_mixer_init_blend ();

  return gst_element_register (plugin, "videomixer", GST_RANK_PRIMARY,
      GST_TYPE_VIDEO_MIXER2);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    videomixer,
    "Video mixer", plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
