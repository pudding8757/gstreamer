/* GStreamer
 * Copyright (C) 2017 Seungha Yang <pudding8757@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <gst/base/gstadapter.h>

#include "gstbasedemux.h"

GST_DEBUG_CATEGORY_STATIC (gst_base_demux_debug);
#define GST_CAT_DEFAULT gst_base_demux_debug

/* Supported formats */
static const GstFormat fmtlist[] = {
  GST_FORMAT_DEFAULT,
  GST_FORMAT_BYTES,
  GST_FORMAT_TIME,
  GST_FORMAT_UNDEFINED
};

#define GST_BASE_DEMUX_GET_PRIVATE(obj)  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_BASE_DEMUX, GstBaseDemuxPrivate))


struct _GstBaseDemuxPrivate
{
  GstAdapter *adapter;

  gboolean streams_aware;

  gint64 duration;
  GstFormat duration_fmt;

  gint64 offset;
  gint64 sync_offset;

  guint32 offset_seek_seqnum;

  gsize skip;

  gbolean discont;
  gboolean flushing;
  gboolean drain;
  gsize stream_struct_size;
};

static GstElementClass *parent_class = NULL;

static void gst_base_demux_class_init (GstBaseDemuxClass * klass);
static void gst_base_demux_init (GstBaseDemux * demux,
    GstBaseDemuxClass * klass);

GType
gst_base_demux_get_type (void)
{
  static volatile gsize base_demux_type = 0;

  if (g_once_init_enter (&base_demux_type)) {
    static const GTypeInfo base_demux_info = {
      sizeof (GstBaseDemuxClass),
      (GBaseInitFunc) NULL,
      (GBaseFinalizeFunc) NULL,
      (GClassInitFunc) gst_base_demux_class_init,
      NULL,
      NULL,
      sizeof (GstBaseDemux),
      0,
      (GInstanceInitFunc) gst_base_demux_init,
    };
    GType _type;

    _type = g_type_register_static (GST_TYPE_ELEMENT,
        "GstBaseDemux", &base_demux_info, G_TYPE_FLAG_ABSTRACT);
    g_once_init_leave (&base_demux_type, _type);
  }
  return (GType) base_demux_type;
}

static void gst_base_demux_finalize (GObject * object);
static GstStateChangeReturn gst_base_demux_change_state (GstElement * element,
    GstStateChange transition);
static void gst_base_demux_reset (GstBaseDemux * demux, gboolean hard);
static gboolean gst_base_demux_sink_activate (GstPad * sinkpad,
    GstObject * parent);
static gboolean gst_base_demux_sink_activate_mode (GstPad * pad,
    GstObject * parent, GstPadMode mode, gboolean active);
static GstFlowReturn gst_base_demux_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer);
static void gst_base_demux_loop (GstPad * pad);

static gboolean gst_base_demux_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_base_demux_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_base_demux_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_base_demux_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_base_demux_sink_event_default (GstBaseDemux * demux,
    GstEvent * event);
static gboolean gst_base_demux_sink_query_default (GstBaseDemux * demux,
    GstQuery * query);
static gboolean gst_base_demux_src_event_default (GstPad * pad,
    GstBaseDemux * demux, GstEvent * event);
static gboolean gst_base_demux_src_query_default (GstPad * pad,
    GstBaseDemux * demux, GstQuery * query);

static void
gst_base_demux_finalize (GObject * object)
{
  GstBaseDemux *demux = GST_BASE_DEMUX (object);

  g_object_unref (demux->priv->adapter);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_base_demux_class_init (GstBaseDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = G_OBJECT_CLASS (klass);
  g_type_class_add_private (klass, sizeof (GstBaseDemuxPrivate));
  parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_base_demux_finalize);

  gstelement_class = (GstElementClass *) klass;
  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_base_demux_change_state);

  /* Default handlers */
  klass->sink_event = gst_base_demux_sink_event_default;
  klass->sink_query = gst_base_demux_sink_query_default;
  klass->src_event = gst_base_demux_src_event_default;
  klass->src_query = gst_base_demux_src_query_default;

  GST_DEBUG_CATEGORY_INIT (gst_base_demux_debug, "basedemux", 0,
      "basedemux element");
}

static void
gst_base_demux_init (GstBaseDemux * demux, GstBaseDemuxClass * bclass)
{
  GstPadTemplate *pad_template;

  GST_DEBUG_OBJECT (demux, "gst_base_demux_init");

  demux->priv = GST_BASE_DEMUX_GET_PRIVATE (demux);
  demux->priv->adapter = gst_adapter_new ();
  demux->priv->stream_struct_size = sizeof (GstBaseDemuxStream);

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (bclass), "sink");
  g_return_if_fail (pad_template != NULL);
  demux->sinkpad = gst_pad_new_from_template (pad_template, "sink");
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_base_demux_sink_event));
  gst_pad_set_query_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_base_demux_sink_query));
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_base_demux_chain));
  gst_pad_set_activate_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_base_demux_sink_activate));
  gst_pad_set_activatemode_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_base_demux_sink_activate_mode));
  GST_PAD_SET_PROXY_ALLOCATION (demux->sinkpad);
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);

  GST_DEBUG_OBJECT (demux, "sinkpad created");

  demux->pad_mode = GST_PAD_MODE_NONE;
  demux->flowcombiner = gst_flow_combiner_new ();

  /* init state */
  gst_base_demux_reset (demux, TRUE);
  GST_DEBUG_OBJECT (demux, "init ok");
}

static void
gst_base_demux_reset (GstBaseDemux * demux, gboolean hard)
{
  GstBaseDemuxClass *klass;
  gboolean result = TRUE;
  klass = GST_BASE_DEMUX_GET_CLASS (demux);

  GST_OBJECT_LOCK (demux);
  gst_segment_init (&demux->segment, GST_FORMAT_TIME);
  demux->priv->duration = -1;
  demux->priv->streams_aware = GST_OBJECT_PARENT (demux)
      && GST_OBJECT_FLAG_IS_SET (GST_OBJECT_PARENT (demux),
      GST_BIN_FLAG_STREAMS_AWARE);
  if (demux->priv->adapter)
    gst_adapter_clear (demux->priv->adapter);

  demux->upstream_format = GST_FORMAT_UNDEFINED;
  GST_OBJECT_UNLOCK (demux);

  if (klass->reset)
    klass->reset (demux, hard);
}

static GstStateChangeReturn
gst_base_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstBaseDemux *demux;
  GstStateChangeReturn result;

  demux = GST_BASE_DEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_base_demux_reset (demux, hard);
      break;
    default:
      break;
  }

  result = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_base_demux_reset (demux, hard);
      break;
    default:
      break;
  }

  return result;
}

static gboolean
gst_base_demux_sink_activate (GstPad * sinkpad, GstObject * parent)
{
  GstSchedulingFlags sched_flags;
  GstBaseDemux *demux;
  GstQuery *query;
  gboolean pull_mode;

  demux = GST_BASE_DEMUX (parent);

  GST_DEBUG_OBJECT (demux, "sink activate");

  query = gst_query_new_scheduling ();
  if (!gst_pad_peer_query (sinkpad, query)) {
    gst_query_unref (query);
    goto basedemux_push;
  }

  gst_query_parse_scheduling (query, &sched_flags, NULL, NULL, NULL);

  pull_mode = gst_query_has_scheduling_mode (query, GST_PAD_MODE_PULL)
      && ((sched_flags & GST_SCHEDULING_FLAG_SEEKABLE) != 0);

  gst_query_unref (query);

  if (!pull_mode)
    goto basedemux_push;

  GST_DEBUG_OBJECT (demux, "trying to activate in pull mode");
  if (!gst_pad_activate_mode (sinkpad, GST_PAD_MODE_PULL, TRUE))
    goto basedemux_push;

  /* In pull mode, upstream is BYTES */
  demux->upstream_format = GST_FORMAT_BYTES;

  return gst_pad_start_task (sinkpad, (GstTaskFunction) gst_base_demux_loop,
      sinkpad, NULL);
  /* fallback */
basedemux_push:
  {
    GST_DEBUG_OBJECT (demux, "trying to activate in push mode");
    return gst_pad_activate_mode (sinkpad, GST_PAD_MODE_PUSH, TRUE);
  }
}

static gboolean
gst_base_demux_activate (GstBaseDemux * demux, gboolean active)
{
  GstBaseDemuxClass *klass;
  gboolean result = TRUE;

  GST_DEBUG_OBJECT (demux, "activate %d", active);

  klass = GST_BASE_DEMUX_GET_CLASS (demux);

  if (active) {
    if (demux->pad_mode == GST_PAD_MODE_NONE && klass->start)
      result = klass->start (demux);
  } else {
    /* We must make sure streaming has finished before resetting things
     * and calling the ::stop vfunc */
    GST_PAD_STREAM_LOCK (demux->sinkpad);
    GST_PAD_STREAM_UNLOCK (demux->sinkpad);

    if (demux->pad_mode != GST_PAD_MODE_NONE && klass->stop)
      result = klass->stop (demux);

    demux->pad_mode = GST_PAD_MODE_NONE;
    demux->upstream_format = GST_FORMAT_UNDEFINED;
  }
  GST_DEBUG_OBJECT (demux, "activate return: %d", result);
  return result;
}

static gboolean
gst_base_demux_sink_activate_mode (GstPad * pad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean result;
  GstBaseDemux *demux;

  demux = GST_BASE_DEMUX (parent);

  GST_DEBUG_OBJECT (demux, "sink %sactivate in %s mode",
      (active) ? "" : "de", gst_pad_mode_get_name (mode));

  if (!gst_base_demux_activate (demux, active))
    goto activate_failed;

  switch (mode) {
    case GST_PAD_MODE_PULL:
      if (active) {
        result = TRUE;
      } else {
        result = gst_pad_stop_task (pad);
      }
      break;
    default:
      result = TRUE;
      break;
  }
  if (result)
    demux->pad_mode = active ? mode : GST_PAD_MODE_NONE;

  GST_DEBUG_OBJECT (demux, "sink activate return: %d", result);

  return result;

  /* ERRORS */
activate_failed:
  {
    GST_DEBUG_OBJECT (demux, "activate failed");
    return FALSE;
  }
}

static GstFlowReturn
gst_base_demux_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstBaseDemux *demux;
  GstBaseParseClass *bclass;

  demux = GST_BASE_DEMUX (parent);
  bclass = GST_BASE_DEMUX_GET_CLASS (demux);

  /* early out for speed, if we need to skip */
  if (buffer && GST_BUFFER_IS_DISCONT (buffer))
    parse->priv->skip = 0;
  if (parse->priv->skip > 0) {
    gsize bsize = gst_buffer_get_size (buffer);
    GST_DEBUG ("Got %" G_GSIZE_FORMAT " buffer, need to skip %u", bsize,
        parse->priv->skip);
    if (parse->priv->skip >= bsize) {
      parse->priv->skip -= bsize;
      GST_DEBUG ("All the buffer is skipped");
      parse->priv->offset += bsize;
      parse->priv->sync_offset = parse->priv->offset;
      return GST_FLOW_OK;
    }
    buffer = gst_buffer_make_writable (buffer);
    gst_buffer_resize (buffer, parse->priv->skip, bsize - parse->priv->skip);
    parse->priv->offset += parse->priv->skip;
    GST_DEBUG ("Done skipping, we have %u left on this buffer",
        (unsigned) (bsize - parse->priv->skip));
    parse->priv->skip = 0;
    parse->priv->discont = TRUE;
  }



  return GST_FLOW_OK;
}

static void
gst_base_demux_loop (GstPad * pad)
{

}

static gboolean
gst_base_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstBaseDemux *demux = GST_BASE_DEMUX (parent);
  GstBaseDemuxClass *bclass = GST_BASE_DEMUX_GET_CLASS (demux);

  return bclass->sink_event (demux, event);
}

static gboolean
gst_base_demux_sink_event_default (GstBaseDemux * demux, GstEvent * event)
{
  GstPad *pad;
  gboolean ret;

  pad = GST_BASE_DEMUX_SINK_PAD (demux);

  GST_DEBUG_OBJECT (demux, "handling event %d, %s", GST_EVENT_TYPE (event),
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      if (gst_event_get_seqnum (event) == demux->priv->offset_seek_seqnum) {
        goto drop;
      }
      break;
    case GST_EVENT_FLUSH_STOP:
      gst_base_demux_reset (demux, FALSE);
      if (gst_event_get_seqnum (event) == demux->priv->offset_seek_seqnum) {
        goto drop;
      }
      break;
    default:
      ret = gst_pad_event_default (pad, GST_OBJECT (demux), event);
      break;
  }

  return ret;

drop:
  gst_event_unref (event);
  return TRUE;
}

static gboolean
gst_base_demux_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstBaseDemuxClass *bclass;
  GstBaseDemux *demux;
  gboolean ret;

  demux = GST_BASE_DEMUX (parent);
  bclass = GST_BASE_DEMUX_GET_CLASS (demux);

  GST_DEBUG_OBJECT (demux, "%s query", GST_QUERY_TYPE_NAME (query));

  if (bclass->sink_query)
    ret = bclass->sink_query (demux, query);
  else
    ret = FALSE;

  GST_LOG_OBJECT (demux, "%s query result: %d %" GST_PTR_FORMAT,
      GST_QUERY_TYPE_NAME (query), ret, query);

  return ret;
}

static gboolean
gst_base_demux_sink_query_default (GstBaseDemux * demux, GstQuery * query)
{
  GstPad *pad;
  gboolean res;

  pad = GST_BASE_DEMUX_SINK_PAD (demux);

  switch (GST_QUERY_TYPE (query)) {
    default:
    {
      res = gst_pad_query_default (pad, GST_OBJECT (demux), query);
      break;
    }
  }

  return res;
}

static gboolean
gst_base_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstBaseDemux *demux = GST_BASE_DEMUX (parent);
  GstBaseDemuxClass *bclass = GST_BASE_DEMUX_GET_CLASS (demux);

  return bclass->src_event (pad, demux, event);
}

static gboolean
gst_base_demux_src_event_default (GstPad * pad, GstBaseDemux * demux,
    GstEvent * event)
{
  gboolean ret;

  GST_DEBUG_OBJECT (demux, "handling event %d, %s", GST_EVENT_TYPE (event),
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    default:
      ret = gst_pad_event_default (pad, GST_OBJECT (demux), event);
      break;
  }

  return ret;
}

static gboolean
gst_base_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstBaseDemuxClass *bclass;
  GstBaseDemux *demux;
  gboolean ret;

  demux = GST_BASE_DEMUX (parent);
  bclass = GST_BASE_DEMUX_GET_CLASS (demux);

  GST_DEBUG_OBJECT (demux, "%s query", GST_QUERY_TYPE_NAME (query));

  if (bclass->src_query)
    ret = bclass->src_query (pad, demux, query);
  else
    ret = FALSE;

  GST_LOG_OBJECT (demux, "%s query result: %d %" GST_PTR_FORMAT,
      GST_QUERY_TYPE_NAME (query), ret, query);

  return ret;
}

static gboolean
gst_base_demux_src_query_default (GstPad * pad, GstBaseDemux * demux,
    GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    default:
    {
      res = gst_pad_query_default (pad, GST_OBJECT (demux), query);
      break;
    }
  }

  return res;
}

void
gst_base_demux_set_stream_struct_size (GstBaseDemux * demux, gsize struct_size)
{
  GST_OBJECT_LOCK (demux);
  GST_BASE_DEMUX_GET_PRIVATE (demux)->stream_struct_size = struct_size;
  GST_OBJECT_UNLOCK (demux);
}

GstBaseDemuxStream *
gst_base_demux_stream_new (GstBaseDemux * demux, GstPad * pad,
    const gchar * stream_id, GstCaps * caps, GstStreamType type,
    GstStreamFlags flags)
{
  GstBaseDemuxStream *stream;

  stream = g_malloc0 (demux->priv->stream_struct_size);
  stream->pad = pad;
  stream->demux = demux;
  stream->stream_obj = gst_stream_new (stream_id, caps, type, flags);

  gst_pad_set_query_function (pad,
      GST_DEBUG_FUNCPTR (gst_base_demux_src_query));
  gst_pad_set_event_function (pad,
      GST_DEBUG_FUNCPTR (gst_base_demux_src_event));

  gst_segment_init (&stream->segment, GST_FORMAT_TIME);

  demux->next_streams = g_list_append (demux->next_streams, stream);

  return stream;
}

static void
gst_base_demux_stream_free (GstBaseDemuxStream * stream)
{
  if (!stream)
    return;

  if (stream->stream_obj)
    gst_object_unref (stream->stream_obj);

  g_free (stream);
}

/**
 *
 */
gboolean
gst_base_demux_stream_remove (GstBaseDemux * demux, GstBaseDemuxStream * stream)
{
  GList *target;
  target = g_list_find (demux->next_streams, stream);
  if (target) {
    gst_base_demux_stream_free (target->data);

    return TRUE;
  }

  return FALSE;
}

gboolean
gst_base_demux_seek_offset (GstBaseDemux * demux, guint64 offset)
{
  GstEvent *event;
  gboolean res;

  GST_DEBUG_OBJECT (demux, "Seeking to %" G_GUINT64_FORMAT, offset);

  event =
      gst_event_new_seek (1.0, GST_FORMAT_BYTES,
      GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, GST_SEEK_TYPE_SET, offset,
      GST_SEEK_TYPE_NONE, -1);

  /* store seqnum to drop flush events, they don't need to reach downstream */
  demux->offset_seek_seqnum = gst_event_get_seqnum (event);
  res = gst_pad_push_event (demux->sinkpad, event);
  demux->offset_seek_seqnum = GST_SEQNUM_INVALID;

  return res;
}
