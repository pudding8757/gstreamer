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

#ifndef __GST_BASE_DEMUX_H__
#define __GST_BASE_DEMUX_H__

#include <gst/gst.h>
#include <gst/base/gstflowcombiner.h>

G_BEGIN_DECLS
#define GST_TYPE_BASE_DEMUX            (gst_base_demux_get_type())
#define GST_BASE_DEMUX(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_BASE_DEMUX,GstBaseDemux))
#define GST_BASE_DEMUX_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_BASE_DEMUX,GstBaseDemuxClass))
#define GST_BASE_DEMUX_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_BASE_DEMUX,GstBaseDemuxClass))
#define GST_IS_BASE_DEMUX(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_BASE_DEMUX))
#define GST_IS_BASE_DEMUX_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_BASE_DEMUX))
#define GST_BASE_DEMUX_CAST(obj)       ((GstBaseDemux *)(obj))
/**
 * GST_BASE_DEMUX_SINK_PAD:
 * @obj: base demux instance
 *
 * Gives the pointer to the sink #GstPad object of the element.
 */
#define GST_BASE_DEMUX_SINK_PAD(obj)    (GST_BASE_DEMUX_CAST (obj)->sinkpad)
typedef struct _GstBaseDemuxStream GstBaseDemuxStream;
typedef struct _GstBaseDemux GstBaseDemux;
typedef struct _GstBaseDemuxClass GstBaseDemuxClass;
typedef struct _GstBaseDemuxPrivate GstBaseDemuxPrivate;

#define GST_BASE_DEMUX_STREAM(obj)  ((GstBaseDemuxStream *)(obj))

/**
 * GstBaseDemuxBuffer:
 *
 */
typedef struct {
  GstBuffer * buffer;
  GstBuffer * out_buffer;

  guint64 offset;
  gint overhead;

  /*<private>*/
  gint size;
} GstBaseDemuxBuffer;

/**
 * GstBaseDemuxStream:
 *
 */
struct _GstBaseDemuxStream
{
  GstStream *stream_obj;

  GstPad *pad;
  GstBaseDemux * demux;

  GstSegment segment;

  gboolean discont;
};

/**
 * GstBaseDemux:
 * @element: the parent element.
 *
 * The opaque #GstBaseDemux data structure.
 */
struct _GstBaseDemux
{
  /*< public > */
  GstElement element;

  /*< protected > */
  GstPad *sinkpad;

  GstFlowCombiner *flowcombiner;

  GstPadMode pad_mode;
  GList *streams;
  GList *next_streams;

  GstSegment segment;
  GstFormat upstream_format;

  /*< private > */
  gpointer _gst_reserved[GST_PADDING_LARGE];
  GstBaseDemuxPrivate *priv;
};

/**
 * GstBaseDemuxClass:
 * @parent_class: the parent class
 * @start:          Optional.
 *                  Called when the element starts processing.
 *                  Allows opening external resources.
 * @stop:           Optional.
 *                  Called when the element stops processing.
 *                  Allows closing external resources.
 * @sink_event:     Optional.
 *                  Event handler on the sink pad. This function should chain
 *                  up to the parent implementation to let the default handler
 *                  run.
 * @src_event:      Optional.
 *                  Event handler on the source pad. Should chain up to the
 *                  parent to let the default handler run.
 * @sink_query:     Optional.
 *                   Query handler on the sink pad. This function should chain
 *                   up to the parent implementation to let the default handler
 *                   run (Since 1.2)
 * @src_query:      Optional.
 *                   Query handler on the source pad. Should chain up to the
 *                   parent to let the default handler run (Since 1.2)
 * @get_duration
 *
 * Subclasses can override any of the available virtual methods or not, as
 * needed. At minimum @handle_frame needs to be overridden.
 */
struct _GstBaseDemuxClass
{
  GstElementClass parent_class;

  /*< public > */
  /* virtual methods for subclasses */

  gboolean (*start) (GstBaseDemux * demux);

  gboolean (*stop) (GstBaseDemux * demux);

  gboolean (*sink_event) (GstBaseDemux * demux, GstEvent * event);

  gboolean (*src_event) (GstPad * pad, GstBaseDemux * demux, GstEvent * event);

  gboolean (*sink_query) (GstBaseDemux * demux, GstQuery * query);

  gboolean (*src_query) (GstPad * pad, GstBaseDemux * demux, GstQuery * query);

  gboolean (*get_duration) (GstBaseDemux * demux, GstClockTime * duration);

  GstFlowReturn (*handle_buffer) (GstBaseDemux * demux, GstBaseDemuxBuffer * buffer, gint *skipsize);

  void (*reset) (GstBaseDemux * demux, gboolean hard);

  /*< private > */
  gpointer _gst_reserved[GST_PADDING_LARGE];
};

GST_EXPORT
GType gst_base_demux_get_type (void);

GST_EXPORT
void     gst_base_demux_set_stream_struct_size (GstBaseDemux * demux,
                                                gsize struct_size);

GST_EXPORT
GstBaseDemuxStream *gst_base_demux_stream_new (GstBaseDemux * demux,
                                               GstPad * pad,
                                               const gchar * stream_id,
                                               GstCaps * caps,
                                               GstStreamType type,
                                               GstStreamFlags flags);

GST_EXPORT
gboolean gst_base_demux_stream_remove (GstBaseDemux * demux,
                                       GstBaseDemuxStream * stream);

GST_EXPORT
gboolean gst_base_demux_finish_buffer (GstBaseDemux * demux,
                                       GstBaseDemuxStream * stream,
                                       GstBaseDemuxBuffer * buffer,
                                       gint size);

GST_EXPORT
gboolean gst_base_demux_seek_offset (GstBaseDemux * demux, guint64 offset);

#ifdef G_DEFINE_AUTOPTR_CLEANUP_FUNC
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstBaseDemux, gst_object_unref)
#endif

G_END_DECLS

#endif /* __GST_BASE_DEMUX_H__ */
