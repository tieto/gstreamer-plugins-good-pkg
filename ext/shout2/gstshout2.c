/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) <2006> Tim-Philipp Müller <tim centricular net>
 * Copyright (C) <2012> Ralph Giles <giles@mozilla.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstshout2.h"
#include <stdlib.h>
#include <string.h>

#include "gst/gst-i18n-plugin.h"

GST_DEBUG_CATEGORY_STATIC (shout2_debug);
#define GST_CAT_DEFAULT shout2_debug


enum
{
  SIGNAL_CONNECTION_PROBLEM,    /* 0.11 FIXME: remove this */
  LAST_SIGNAL
};

enum
{
  ARG_0,
  ARG_IP,                       /* the ip of the server */
  ARG_PORT,                     /* the encoder port number on the server */
  ARG_PASSWORD,                 /* the encoder password on the server */
  ARG_USERNAME,                 /* the encoder username on the server */
  ARG_PUBLIC,                   /* is this stream public? */
  ARG_STREAMNAME,               /* Name of the stream */
  ARG_DESCRIPTION,              /* Description of the stream */
  ARG_GENRE,                    /* Genre of the stream */

  ARG_PROTOCOL,                 /* Protocol to connect with */

  ARG_MOUNT,                    /* mountpoint of stream (icecast only) */
  ARG_URL                       /* Url of stream (I'm guessing) */
};

#define DEFAULT_IP           "127.0.0.1"
#define DEFAULT_PORT         8000
#define DEFAULT_PASSWORD     "hackme"
#define DEFAULT_USERNAME     "source"
#define DEFAULT_PUBLIC     FALSE
#define DEFAULT_STREAMNAME   ""
#define DEFAULT_DESCRIPTION  ""
#define DEFAULT_GENRE        ""
#define DEFAULT_MOUNT        ""
#define DEFAULT_URL          ""
#define DEFAULT_PROTOCOL     SHOUT2SEND_PROTOCOL_HTTP

#ifdef SHOUT_FORMAT_WEBM
#define WEBM_CAPS "; video/webm"
#else
#define WEBM_CAPS ""
#endif
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/ogg; audio/ogg; video/ogg; "
        "audio/mpeg, mpegversion = (int) 1, layer = (int) [ 1, 3 ]" WEBM_CAPS));

static void gst_shout2send_finalize (GstShout2send * shout2send);

static gboolean gst_shout2send_event (GstBaseSink * sink, GstEvent * event);
static gboolean gst_shout2send_unlock (GstBaseSink * basesink);
static gboolean gst_shout2send_unlock_stop (GstBaseSink * basesink);
static GstFlowReturn gst_shout2send_render (GstBaseSink * sink,
    GstBuffer * buffer);
static gboolean gst_shout2send_start (GstBaseSink * basesink);
static gboolean gst_shout2send_stop (GstBaseSink * basesink);

static void gst_shout2send_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_shout2send_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_shout2send_setcaps (GstBaseSink * basesink, GstCaps * caps);

static guint gst_shout2send_signals[LAST_SIGNAL] = { 0 };

#define GST_TYPE_SHOUT_PROTOCOL (gst_shout2send_protocol_get_type())
static GType
gst_shout2send_protocol_get_type (void)
{
  static GType shout2send_protocol_type = 0;
  static const GEnumValue shout2send_protocol[] = {
    {SHOUT2SEND_PROTOCOL_XAUDIOCAST,
        "Xaudiocast Protocol (icecast 1.3.x)", "xaudiocast"},
    {SHOUT2SEND_PROTOCOL_ICY, "Icy Protocol (ShoutCast)", "icy"},
    {SHOUT2SEND_PROTOCOL_HTTP, "Http Protocol (icecast 2.x)", "http"},
    {0, NULL, NULL},
  };

  if (!shout2send_protocol_type) {
    shout2send_protocol_type =
        g_enum_register_static ("GstShout2SendProtocol", shout2send_protocol);
  }


  return shout2send_protocol_type;
}

#define gst_shout2send_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstShout2send, gst_shout2send, GST_TYPE_BASE_SINK,
    G_IMPLEMENT_INTERFACE (GST_TYPE_TAG_SETTER, NULL));

static void
gst_shout2send_class_init (GstShout2sendClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = gst_shout2send_set_property;
  gobject_class->get_property = gst_shout2send_get_property;
  gobject_class->finalize = (GObjectFinalizeFunc) gst_shout2send_finalize;

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_IP,
      g_param_spec_string ("ip", "ip", "ip", DEFAULT_IP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_PORT,
      g_param_spec_int ("port", "port", "port", 1, G_MAXUSHORT, DEFAULT_PORT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_PASSWORD,
      g_param_spec_string ("password", "password", "password", DEFAULT_PASSWORD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_USERNAME,
      g_param_spec_string ("username", "username", "username", DEFAULT_USERNAME,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* metadata */
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_PUBLIC,
      g_param_spec_boolean ("public", "public",
          "If the stream should be listed on the server's stream directory",
          DEFAULT_PUBLIC, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_STREAMNAME,
      g_param_spec_string ("streamname", "streamname", "name of the stream",
          DEFAULT_STREAMNAME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_DESCRIPTION,
      g_param_spec_string ("description", "description", "description",
          DEFAULT_DESCRIPTION, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_GENRE,
      g_param_spec_string ("genre", "genre", "genre", DEFAULT_GENRE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_PROTOCOL,
      g_param_spec_enum ("protocol", "protocol", "Connection Protocol to use",
          GST_TYPE_SHOUT_PROTOCOL, DEFAULT_PROTOCOL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  /* icecast only */
  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_MOUNT,
      g_param_spec_string ("mount", "mount", "mount", DEFAULT_MOUNT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (G_OBJECT_CLASS (klass), ARG_URL,
      g_param_spec_string ("url", "url", "url", DEFAULT_URL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* signals */
  gst_shout2send_signals[SIGNAL_CONNECTION_PROBLEM] =
      g_signal_new ("connection-problem", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_CLEANUP, G_STRUCT_OFFSET (GstShout2sendClass,
          connection_problem), NULL, NULL, g_cclosure_marshal_VOID__INT,
      G_TYPE_NONE, 1, G_TYPE_INT);

  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_shout2send_start);
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_shout2send_stop);
  gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_shout2send_unlock);
  gstbasesink_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_shout2send_unlock_stop);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_shout2send_render);
  gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_shout2send_event);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_shout2send_setcaps);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "Icecast network sink",
      "Sink/Network", "Sends data to an icecast server",
      "Wim Taymans <wim.taymans@chello.be>, "
      "Pedro Corte-Real <typo@netcabo.pt>, "
      "Zaheer Abbas Merali <zaheerabbas at merali dot org>");

  GST_DEBUG_CATEGORY_INIT (shout2_debug, "shout2", 0, "shout2send element");
}

static void
gst_shout2send_init (GstShout2send * shout2send)
{
  gst_base_sink_set_sync (GST_BASE_SINK (shout2send), FALSE);

  shout2send->timer = gst_poll_new_timer ();

  shout2send->ip = g_strdup (DEFAULT_IP);
  shout2send->port = DEFAULT_PORT;
  shout2send->password = g_strdup (DEFAULT_PASSWORD);
  shout2send->username = g_strdup (DEFAULT_USERNAME);
  shout2send->streamname = g_strdup (DEFAULT_STREAMNAME);
  shout2send->description = g_strdup (DEFAULT_DESCRIPTION);
  shout2send->genre = g_strdup (DEFAULT_GENRE);
  shout2send->mount = g_strdup (DEFAULT_MOUNT);
  shout2send->url = g_strdup (DEFAULT_URL);
  shout2send->protocol = DEFAULT_PROTOCOL;
  shout2send->ispublic = DEFAULT_PUBLIC;

  shout2send->tags = gst_tag_list_new_empty ();
  shout2send->conn = NULL;
  shout2send->audio_format = SHOUT_FORMAT_VORBIS;
  shout2send->connected = FALSE;
  shout2send->songmetadata = NULL;
  shout2send->songartist = NULL;
  shout2send->songtitle = NULL;
}

static void
gst_shout2send_finalize (GstShout2send * shout2send)
{
  g_free (shout2send->ip);
  g_free (shout2send->password);
  g_free (shout2send->username);
  g_free (shout2send->streamname);
  g_free (shout2send->description);
  g_free (shout2send->genre);
  g_free (shout2send->mount);
  g_free (shout2send->url);

  gst_tag_list_unref (shout2send->tags);

  gst_poll_free (shout2send->timer);

  G_OBJECT_CLASS (parent_class)->finalize ((GObject *) (shout2send));
}

static void
set_shout_metadata (const GstTagList * list, const gchar * tag,
    gpointer user_data)
{
  GstShout2send *shout2send = (GstShout2send *) user_data;
  char **shout_metadata = &(shout2send->songmetadata);
  char **song_artist = &(shout2send->songartist);
  char **song_title = &(shout2send->songtitle);

  gchar *value;

  GST_DEBUG ("tag: %s being added", tag);
  if (strcmp (tag, GST_TAG_ARTIST) == 0) {
    if (gst_tag_get_type (tag) == G_TYPE_STRING) {
      if (!gst_tag_list_get_string (list, tag, &value)) {
        GST_DEBUG ("Error reading \"%s\" tag value", tag);
        return;
      }

      if (*song_artist != NULL)
        g_free (*song_artist);

      *song_artist = g_strdup (value);
    }
  } else if (strcmp (tag, GST_TAG_TITLE) == 0) {
    if (gst_tag_get_type (tag) == G_TYPE_STRING) {
      if (!gst_tag_list_get_string (list, tag, &value)) {
        GST_DEBUG ("Error reading \"%s\" tag value", tag);
        return;
      }

      if (*song_title != NULL)
        g_free (*song_title);

      *song_title = g_strdup (value);
    }
  }

  if (*shout_metadata != NULL)
    g_free (*shout_metadata);


  if (*song_title && *song_artist) {
    *shout_metadata = g_strdup_printf ("%s - %s", *song_artist, *song_title);
  } else if (*song_title && *song_artist == NULL) {
    *shout_metadata = g_strdup_printf ("Unknown - %s", *song_title);
  } else if (*song_title == NULL && *song_artist) {
    *shout_metadata = g_strdup_printf ("%s - Unknown", *song_artist);
  } else {
    *shout_metadata = g_strdup_printf ("Unknown - Unknown");
  }

  GST_LOG ("shout metadata is now: %s", *shout_metadata);
}

#if 0
static void
gst_shout2send_set_metadata (GstShout2send * shout2send)
{
  const GstTagList *user_tags;
  GstTagList *copy;
  char *tempmetadata;
  shout_metadata_t *pmetadata;

  g_return_if_fail (shout2send != NULL);
  user_tags = gst_tag_setter_get_tag_list (GST_TAG_SETTER (shout2send));
  if ((shout2send->tags == NULL) && (user_tags == NULL)) {
    return;
  }
  copy = gst_tag_list_merge (user_tags, shout2send->tags,
      gst_tag_setter_get_tag_merge_mode (GST_TAG_SETTER (shout2send)));
  /* lets get the artist and song tags */
  tempmetadata = NULL;
  gst_tag_list_foreach ((GstTagList *) copy, set_shout_metadata,
      (gpointer) & tempmetadata);
  if (tempmetadata) {
    pmetadata = shout_metadata_new ();
    shout_metadata_add (pmetadata, "song", tempmetadata);
    shout_set_metadata (shout2send->conn, pmetadata);
    shout_metadata_free (pmetadata);
  }

  gst_tag_list_unref (copy);
}
#endif


static gboolean
gst_shout2send_event (GstBaseSink * sink, GstEvent * event)
{
  GstShout2send *shout2send;
  gboolean ret = TRUE;

  shout2send = GST_SHOUT2SEND (sink);

  GST_LOG_OBJECT (shout2send, "got %s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_TAG:{
      /* vorbis audio doesnt need metadata setting on the icecast level, only mp3 */
      if (shout2send->tags && shout2send->audio_format == SHOUT_FORMAT_MP3) {
        GstTagList *list;

        gst_event_parse_tag (event, &list);
        GST_DEBUG_OBJECT (shout2send, "tags=%" GST_PTR_FORMAT, list);
        gst_tag_list_insert (shout2send->tags,
            list,
            gst_tag_setter_get_tag_merge_mode (GST_TAG_SETTER (shout2send)));
        /* lets get the artist and song tags */
        gst_tag_list_foreach ((GstTagList *) list,
            set_shout_metadata, shout2send);
        if (shout2send->songmetadata && shout2send->connected) {
          shout_metadata_t *pmetadata;

          GST_DEBUG_OBJECT (shout2send, "metadata now: %s",
              shout2send->songmetadata);

          pmetadata = shout_metadata_new ();
          shout_metadata_add (pmetadata, "song", shout2send->songmetadata);
          shout_set_metadata (shout2send->conn, pmetadata);
          shout_metadata_free (pmetadata);
        }
      }
      break;
    }
    default:{
      GST_LOG_OBJECT (shout2send, "let base class handle event");
      if (GST_BASE_SINK_CLASS (parent_class)->event) {
        event = gst_event_ref (event);
        ret = GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
      }
      break;
    }
  }

  return ret;
}

static gboolean
gst_shout2send_start (GstBaseSink * basesink)
{
  GstShout2send *sink = GST_SHOUT2SEND (basesink);
  const gchar *cur_prop;
  gshort proto = 3;
  gchar *version_string;

  GST_DEBUG_OBJECT (sink, "starting");

  sink->conn = shout_new ();

  switch (sink->protocol) {
    case SHOUT2SEND_PROTOCOL_XAUDIOCAST:
      proto = SHOUT_PROTOCOL_XAUDIOCAST;
      break;
    case SHOUT2SEND_PROTOCOL_ICY:
      proto = SHOUT_PROTOCOL_ICY;
      break;
    case SHOUT2SEND_PROTOCOL_HTTP:
      proto = SHOUT_PROTOCOL_HTTP;
      break;
  }

  cur_prop = "protocol";
  GST_DEBUG_OBJECT (sink, "setting protocol: %d", sink->protocol);
  if (shout_set_protocol (sink->conn, proto) != SHOUTERR_SUCCESS)
    goto set_failed;

  /* --- FIXME: shout requires an ip, and fails if it is given a host. */
  /* may want to put convert_to_ip(shout2send->ip) here */
  cur_prop = "ip";
  GST_DEBUG_OBJECT (sink, "setting ip: %s", sink->ip);
  if (shout_set_host (sink->conn, sink->ip) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "port";
  GST_DEBUG_OBJECT (sink, "setting port: %u", sink->port);
  if (shout_set_port (sink->conn, sink->port) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "password";
  GST_DEBUG_OBJECT (sink, "setting password: %s", sink->password);
  if (shout_set_password (sink->conn, sink->password) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "public";
  GST_DEBUG_OBJECT (sink, "setting %s: %u", cur_prop, sink->ispublic);
  if (shout_set_public (sink->conn,
          (sink->ispublic ? 1 : 0)) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "streamname";
  GST_DEBUG_OBJECT (sink, "setting %s: %s", cur_prop, sink->streamname);
  if (shout_set_name (sink->conn, sink->streamname) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "description";
  GST_DEBUG_OBJECT (sink, "setting %s: %s", cur_prop, sink->description);
  if (shout_set_description (sink->conn, sink->description) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "genre";
  GST_DEBUG_OBJECT (sink, "setting %s: %s", cur_prop, sink->genre);
  if (shout_set_genre (sink->conn, sink->genre) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "mount";
  GST_DEBUG_OBJECT (sink, "setting %s: %s", cur_prop, sink->mount);
  if (shout_set_mount (sink->conn, sink->mount) != SHOUTERR_SUCCESS)
    goto set_failed;

  cur_prop = "username";
  GST_DEBUG_OBJECT (sink, "setting %s: %s", cur_prop, "source");
  if (shout_set_user (sink->conn, sink->username) != SHOUTERR_SUCCESS)
    goto set_failed;

  version_string = gst_version_string ();
  cur_prop = "agent";
  GST_DEBUG_OBJECT (sink, "setting %s: %s", cur_prop, version_string);
  if (shout_set_agent (sink->conn, version_string) != SHOUTERR_SUCCESS) {
    g_free (version_string);
    goto set_failed;
  }

  g_free (version_string);
  return TRUE;

/* ERROR */
set_failed:
  {
    GST_ELEMENT_ERROR (sink, LIBRARY, SETTINGS, (NULL),
        ("Error setting %s: %s", cur_prop, shout_get_error (sink->conn)));
    return FALSE;
  }
}

static gboolean
gst_shout2send_connect (GstShout2send * sink)
{
  const char *format =
      (sink->audio_format == SHOUT_FORMAT_VORBIS) ? "vorbis" :
      ((sink->audio_format == SHOUT_FORMAT_MP3) ? "mp3" : "unknown");
#ifdef SHOUT_FORMAT_WEBM
  if (sink->audio_format == SHOUT_FORMAT_WEBM)
    format = "webm";
#endif
  GST_DEBUG_OBJECT (sink, "Connection format is: %s", format);

  if (shout_set_format (sink->conn, sink->audio_format) != SHOUTERR_SUCCESS)
    goto could_not_set_format;

  if (shout_open (sink->conn) != SHOUTERR_SUCCESS)
    goto could_not_connect;

  GST_DEBUG_OBJECT (sink, "connected to server");
  sink->connected = TRUE;

  /* let's set metadata */
  if (sink->songmetadata) {
    shout_metadata_t *pmetadata;

    GST_DEBUG_OBJECT (sink, "shout metadata now: %s", sink->songmetadata);
    pmetadata = shout_metadata_new ();
    shout_metadata_add (pmetadata, "song", sink->songmetadata);
    shout_set_metadata (sink->conn, pmetadata);
    shout_metadata_free (pmetadata);
  }

  return TRUE;

/* ERRORS */
could_not_set_format:
  {
    GST_ELEMENT_ERROR (sink, LIBRARY, SETTINGS, (NULL),
        ("Error setting connection format: %s", shout_get_error (sink->conn)));
    return FALSE;
  }

could_not_connect:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, OPEN_WRITE,
        (_("Could not connect to server")),
        ("shout_open() failed: err=%s", shout_get_error (sink->conn)));
    g_signal_emit (sink, gst_shout2send_signals[SIGNAL_CONNECTION_PROBLEM], 0,
        shout_get_errno (sink->conn));
    return FALSE;
  }
}

static gboolean
gst_shout2send_stop (GstBaseSink * basesink)
{
  GstShout2send *sink = GST_SHOUT2SEND (basesink);

  if (sink->conn) {
    if (sink->connected)
      shout_close (sink->conn);
    shout_free (sink->conn);
    sink->conn = NULL;
  }

  if (sink->songmetadata) {
    g_free (sink->songmetadata);
    sink->songmetadata = NULL;
  }

  sink->connected = FALSE;

  return TRUE;
}

static gboolean
gst_shout2send_unlock (GstBaseSink * basesink)
{
  GstShout2send *sink;

  sink = GST_SHOUT2SEND (basesink);

  GST_DEBUG_OBJECT (basesink, "unlock");
  gst_poll_set_flushing (sink->timer, TRUE);

  return TRUE;
}

static gboolean
gst_shout2send_unlock_stop (GstBaseSink * basesink)
{
  GstShout2send *sink;

  sink = GST_SHOUT2SEND (basesink);

  GST_DEBUG_OBJECT (basesink, "unlock_stop");
  gst_poll_set_flushing (sink->timer, FALSE);

  return TRUE;
}

static GstFlowReturn
gst_shout2send_render (GstBaseSink * basesink, GstBuffer * buf)
{
  GstShout2send *sink;
  glong ret;
  gint delay;
  GstFlowReturn fret;
  GstMapInfo map;

  sink = GST_SHOUT2SEND (basesink);

  /* presumably we connect here because we need to know the format before
   * we can set up the connection, which we don't know yet in _start() */
  if (!sink->connected) {
    if (!gst_shout2send_connect (sink))
      return GST_FLOW_ERROR;
  }

  delay = shout_delay (sink->conn);

  if (delay > 0) {
    GST_LOG_OBJECT (sink, "waiting %d msec", delay);
    if (gst_poll_wait (sink->timer, GST_MSECOND * delay) == -1) {
      GST_LOG_OBJECT (sink, "unlocked");

      fret = gst_base_sink_wait_preroll (basesink);
      if (fret != GST_FLOW_OK)
        return fret;
    }
  } else {
    GST_LOG_OBJECT (sink, "we're %d msec late", -delay);
  }

  gst_buffer_map (buf, &map, GST_MAP_READ);
  GST_LOG_OBJECT (sink, "sending %u bytes of data", (guint) map.size);
  ret = shout_send (sink->conn, map.data, map.size);
  gst_buffer_unmap (buf, &map);
  if (ret != SHOUTERR_SUCCESS)
    goto send_error;

  return GST_FLOW_OK;

/* ERRORS */
send_error:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, WRITE, (NULL),
        ("shout_send() failed: %s", shout_get_error (sink->conn)));
    g_signal_emit (sink, gst_shout2send_signals[SIGNAL_CONNECTION_PROBLEM], 0,
        shout_get_errno (sink->conn));
    return GST_FLOW_ERROR;
  }
}

static void
gst_shout2send_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstShout2send *shout2send;

  shout2send = GST_SHOUT2SEND (object);
  switch (prop_id) {

    case ARG_IP:
      if (shout2send->ip)
        g_free (shout2send->ip);
      shout2send->ip = g_strdup (g_value_get_string (value));
      break;
    case ARG_PORT:
      shout2send->port = g_value_get_int (value);
      break;
    case ARG_PASSWORD:
      if (shout2send->password)
        g_free (shout2send->password);
      shout2send->password = g_strdup (g_value_get_string (value));
      break;
    case ARG_USERNAME:
      if (shout2send->username)
        g_free (shout2send->username);
      shout2send->username = g_strdup (g_value_get_string (value));
      break;
    case ARG_PUBLIC:
      shout2send->ispublic = g_value_get_boolean (value);
      break;
    case ARG_STREAMNAME:       /* Name of the stream */
      if (shout2send->streamname)
        g_free (shout2send->streamname);
      shout2send->streamname = g_strdup (g_value_get_string (value));
      break;
    case ARG_DESCRIPTION:      /* Description of the stream */
      if (shout2send->description)
        g_free (shout2send->description);
      shout2send->description = g_strdup (g_value_get_string (value));
      break;
    case ARG_GENRE:            /* Genre of the stream */
      if (shout2send->genre)
        g_free (shout2send->genre);
      shout2send->genre = g_strdup (g_value_get_string (value));
      break;
    case ARG_PROTOCOL:         /* protocol to connect with */
      shout2send->protocol = g_value_get_enum (value);
      break;
    case ARG_MOUNT:            /* mountpoint of stream (icecast only) */
      if (shout2send->mount)
        g_free (shout2send->mount);
      shout2send->mount = g_strdup (g_value_get_string (value));
      break;
    case ARG_URL:              /* Url of the stream (I'm guessing) */
      if (shout2send->url)
        g_free (shout2send->url);
      shout2send->url = g_strdup (g_value_get_string (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_shout2send_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstShout2send *shout2send;

  shout2send = GST_SHOUT2SEND (object);
  switch (prop_id) {

    case ARG_IP:
      g_value_set_string (value, shout2send->ip);
      break;
    case ARG_PORT:
      g_value_set_int (value, shout2send->port);
      break;
    case ARG_PASSWORD:
      g_value_set_string (value, shout2send->password);
      break;
    case ARG_USERNAME:
      g_value_set_string (value, shout2send->username);
      break;
    case ARG_PUBLIC:
      g_value_set_boolean (value, shout2send->ispublic);
      break;
    case ARG_STREAMNAME:       /* Name of the stream */
      g_value_set_string (value, shout2send->streamname);
      break;
    case ARG_DESCRIPTION:      /* Description of the stream */
      g_value_set_string (value, shout2send->description);
      break;
    case ARG_GENRE:            /* Genre of the stream */
      g_value_set_string (value, shout2send->genre);
      break;
    case ARG_PROTOCOL:         /* protocol to connect with */
      g_value_set_enum (value, shout2send->protocol);
      break;
    case ARG_MOUNT:            /* mountpoint of stream (icecast only) */
      g_value_set_string (value, shout2send->mount);
      break;
    case ARG_URL:              /* Url of stream (I'm guessing) */
      g_value_set_string (value, shout2send->url);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_shout2send_setcaps (GstBaseSink * basesink, GstCaps * caps)
{
  const gchar *mimetype;
  GstShout2send *shout2send;
  gboolean ret = TRUE;

  shout2send = GST_SHOUT2SEND (basesink);

  mimetype = gst_structure_get_name (gst_caps_get_structure (caps, 0));

  GST_DEBUG_OBJECT (shout2send, "mimetype of caps given is: %s", mimetype);

  if (!strcmp (mimetype, "audio/mpeg")) {
    shout2send->audio_format = SHOUT_FORMAT_MP3;
  } else if (!strcmp (mimetype, "application/ogg")) {
    shout2send->audio_format = SHOUT_FORMAT_VORBIS;
#ifdef SHOUT_FORMAT_WEBM
  } else if (!strcmp (mimetype, "video/webm")) {
    shout2send->audio_format = SHOUT_FORMAT_WEBM;
#endif
  } else {
    ret = FALSE;
  }

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
#ifdef ENABLE_NLS
  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
#endif /* ENABLE_NLS */

  return gst_element_register (plugin, "shout2send", GST_RANK_NONE,
      GST_TYPE_SHOUT2SEND);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    shout2send,
    "Sends data to an icecast server using libshout2",
    plugin_init,
    VERSION, "LGPL", "libshout2", "http://www.icecast.org/download.html")
