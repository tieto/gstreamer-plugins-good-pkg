/* GStreamer
 * Copyright (C) 1999,2000 Erik Walthinsen <omega@cse.ogi.edu>
 *                    2000 Wim Taymans <wim.taymans@chello.be>
 *
 * gstosshelper.h: OSS helper routines.
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


#ifndef __GST_OSS_HELPER_H__
#define __GST_OSS_HELPER_H__


#include <gst/gst.h>
#include <sys/types.h>

#include "gstosshelper.h"


G_BEGIN_DECLS


GstCaps* gst_oss_helper_probe_caps (gint fd);

gchar *gst_oss_helper_get_card_name (const gchar * mixer_name);



G_END_DECLS


#endif /* __GST_OSS_HELPER_H__ */