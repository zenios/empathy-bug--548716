/*
 * Copyright (C) 2007-2008 Collabora Ltd.
 * Copyright (C) 2007 Marco Barisione <marco@barisione.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Authors: Marco Barisione <marco@barisione.org>
 *          Jonny Lamb <jonny.lamb@collabora.co.uk>
 */

#include <config.h>

#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib/gi18n-lib.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>

#include <telepathy-glib/proxy-subclass.h>
#include <telepathy-glib/util.h>

#include "empathy-tp-file.h"
#include "empathy-tp-contact-factory.h"
#include "empathy-marshal.h"
#include "empathy-time.h"
#include "empathy-utils.h"

#include "extensions/extensions.h"

#define DEBUG_FLAG EMPATHY_DEBUG_FT
#include "empathy-debug.h"

/**
 * SECTION:empathy-tp-file
 * @title: EmpathyTpFile
 * @short_description: Object which represents a Telepathy file channel
 * @include: libempathy/empathy-tp-file.h
 *
 * #EmpathyTpFile is an object which represents a Telepathy file channel.
 */

/**
 * EmpathyTpFile:
 * @parent: parent object
 *
 * Object which represents a Telepathy file channel.
 */

/**
 * EMPATHY_TP_FILE_UNKNOWN_SIZE:
 *
 * Value used for the "size" or "estimated-size" properties when the size of
 * the transferred file is unknown.
 */

/* Functions to copy the content of a GInputStream to a GOutputStream */

#define N_BUFFERS 2
#define BUFFER_SIZE 4096
#define STALLED_TIMEOUT 5

typedef struct {
  GInputStream *in;
  GOutputStream *out;
  GCancellable  *cancellable;
  char *buff[N_BUFFERS]; /* the temporary buffers */
  gsize count[N_BUFFERS]; /* how many bytes are used in the buffers */
  gboolean is_full[N_BUFFERS]; /* whether the buffers contain data */
  gint curr_read; /* index of the buffer used for reading */
  gint curr_write; /* index of the buffer used for writing */
  gboolean is_reading; /* we are reading */
  gboolean is_writing; /* we are writing */
  guint n_closed; /* number of streams that have been closed */
  gint ref_count;
} CopyData;

static void schedule_next (CopyData *copy);

static void
copy_data_unref (CopyData *copy)
{
  if (--copy->ref_count == 0)
    {
      gint i;

      /* Free the data only if both the input and output streams have
       * been closed. */
      copy->n_closed++;
      if (copy->n_closed < 2)
        return;

      if (copy->in != NULL)
        g_object_unref (copy->in);

      if (copy->out != NULL)
        g_object_unref (copy->out);

      for (i = 0; i < N_BUFFERS; i++)
        g_free (copy->buff[i]);

      g_object_unref (copy->cancellable);
      g_free (copy);
    }
}

static void
io_error (CopyData *copy,
          GError *error)
{
  g_cancellable_cancel (copy->cancellable);

  if (error == NULL)
    g_warning ("I/O error");
  else if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_CANCELLED)
    ; /* Ignore cancellations */
  else
    g_warning ("I/O error: %d: %s\n", error->code, error->message);

  if (copy->in != NULL)
    g_input_stream_close (copy->in, NULL, NULL);

  if (copy->out != NULL)
    g_output_stream_close (copy->out, NULL, NULL);

  copy_data_unref (copy);
}

static void
close_done (GObject *source_object,
            GAsyncResult *res,
            gpointer user_data)
{
  CopyData *copy = user_data;

  g_object_unref (source_object);
  copy_data_unref (copy);
}

static void
write_done_cb (GObject *source_object,
               GAsyncResult *res,
               gpointer user_data)
{
  CopyData *copy = user_data;
  gssize count_write;
  GError *error = NULL;

  count_write = g_output_stream_write_finish (copy->out, res, &error);

  if (count_write <= 0)
    {
      io_error (copy, error);
      g_error_free (error);
      return;
    }

  copy->is_full[copy->curr_write] = FALSE;
  copy->curr_write = (copy->curr_write + 1) % N_BUFFERS;
  copy->is_writing = FALSE;

  schedule_next (copy);
}

static void
read_done_cb (GObject *source_object,
              GAsyncResult *res,
              gpointer user_data)
{
  CopyData *copy = user_data;
  gssize count_read;
  GError *error = NULL;

  count_read = g_input_stream_read_finish (copy->in, res, &error);

  if (count_read == 0)
    {
      g_input_stream_close_async (copy->in, 0, copy->cancellable,
          close_done, copy);
      copy->in = NULL;
    }
  else if (count_read < 0)
    {
      io_error (copy, error);
      g_error_free (error);
      return;
    }

  copy->count[copy->curr_read] = count_read;
  copy->is_full[copy->curr_read] = TRUE;
  copy->curr_read = (copy->curr_read + 1) % N_BUFFERS;
  copy->is_reading = FALSE;

  schedule_next (copy);
}

static void
schedule_next (CopyData *copy)
{
  if (copy->in != NULL &&
      !copy->is_reading &&
      !copy->is_full[copy->curr_read])
    {
      /* We are not reading and the current buffer is empty, so
       * start an async read. */
      copy->is_reading = TRUE;
      g_input_stream_read_async (copy->in,
          copy->buff[copy->curr_read],
          BUFFER_SIZE, 0, copy->cancellable,
          read_done_cb, copy);
    }

  if (!copy->is_writing &&
      copy->is_full[copy->curr_write])
    {
      if (copy->count[copy->curr_write] == 0)
        {
          /* The last read on the buffer read 0 bytes, this
           * means that we got an EOF, so we can close
           * the output channel. */
          g_output_stream_close_async (copy->out, 0,
              copy->cancellable,
              close_done, copy);
      copy->out = NULL;
        }
      else
        {
          /* We are not writing and the current buffer contains
           * data, so start an async write. */
          copy->is_writing = TRUE;
          g_output_stream_write_async (copy->out,
              copy->buff[copy->curr_write],
              copy->count[copy->curr_write],
              0, copy->cancellable,
              write_done_cb, copy);
        }
    }
}

static void
copy_stream (GInputStream *in,
             GOutputStream *out,
             GCancellable *cancellable)
{
  CopyData *copy;
  gint i;

  g_return_if_fail (in != NULL);
  g_return_if_fail (out != NULL);

  copy = g_new0 (CopyData, 1);
  copy->in = g_object_ref (in);
  copy->out = g_object_ref (out);
  copy->ref_count = 1;

  if (cancellable != NULL)
    copy->cancellable = g_object_ref (cancellable);
  else
    copy->cancellable = g_cancellable_new ();

  for (i = 0; i < N_BUFFERS; i++)
    copy->buff[i] = g_malloc (BUFFER_SIZE);

  schedule_next (copy);
}

/* EmpathyTpFile object */

struct _EmpathyTpFilePriv {
  EmpathyTpContactFactory *factory;
  MissionControl *mc;
  TpChannel *channel;
  gboolean ready;

  EmpathyContact *contact;
  GInputStream *in_stream;
  GOutputStream *out_stream;

  /* org.freedesktop.Telepathy.Channel.Type.FileTransfer D-Bus properties */
  TpFileTransferState state;
  gchar *content_type;
  gchar *filename;
  guint64 size;
  TpFileHashType content_hash_type;
  gchar *content_hash;
  gchar *description;
  guint64 transferred_bytes;

  gboolean incoming;
  TpFileTransferStateChangeReason state_change_reason;
  time_t last_update_time;
  guint64 last_update_transferred_bytes;
  gdouble speed;
  gint remaining_time;
  guint stalled_id;
  GValue *socket_address;
  GCancellable *cancellable;
};

enum {
  PROP_0,
  PROP_CHANNEL,
  PROP_STATE,
  PROP_INCOMING,
  PROP_READY,
  PROP_FILENAME,
  PROP_SIZE,
  PROP_CONTENT_TYPE,
  PROP_TRANSFERRED_BYTES,
  PROP_CONTENT_HASH_TYPE,
  PROP_CONTENT_HASH,
};

enum {
	REFRESH,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (EmpathyTpFile, empathy_tp_file, G_TYPE_OBJECT);

static void
empathy_tp_file_init (EmpathyTpFile *tp_file)
{
  EmpathyTpFilePriv *priv;

  priv = G_TYPE_INSTANCE_GET_PRIVATE ((tp_file),
      EMPATHY_TYPE_TP_FILE, EmpathyTpFilePriv);

  tp_file->priv = priv;
}

static void
tp_file_invalidated_cb (TpProxy       *proxy,
			guint          domain,
			gint           code,
			gchar         *message,
			EmpathyTpFile *tp_file)
{
  DEBUG ("Channel invalidated: %s", message);

  if (tp_file->priv->state != TP_FILE_TRANSFER_STATE_COMPLETED &&
      tp_file->priv->state != TP_FILE_TRANSFER_STATE_CANCELLED)
    {
      /* The channel is not in a finished state, an error occured */
      tp_file->priv->state = TP_FILE_TRANSFER_STATE_CANCELLED;
      tp_file->priv->state_change_reason =
          TP_FILE_TRANSFER_STATE_CHANGE_REASON_LOCAL_ERROR;
      g_object_notify (G_OBJECT (tp_file), "state");
    }
}

static void
tp_file_finalize (GObject *object)
{
  EmpathyTpFile *tp_file = EMPATHY_TP_FILE (object);

  if (tp_file->priv->channel)
    {
      g_signal_handlers_disconnect_by_func (tp_file->priv->channel,
          tp_file_invalidated_cb, object);
      g_object_unref (tp_file->priv->channel);
      tp_file->priv->channel = NULL;
    }

  if (tp_file->priv->factory)
    {
      g_object_unref (tp_file->priv->factory);
    }
  if (tp_file->priv->mc)
    {
      g_object_unref (tp_file->priv->mc);
    }

  g_free (tp_file->priv->filename);
  if (tp_file->priv->socket_address != NULL)
    tp_g_value_slice_free (tp_file->priv->socket_address);
  g_free (tp_file->priv->description);
  g_free (tp_file->priv->content_hash);
  g_free (tp_file->priv->content_type);

  if (tp_file->priv->in_stream)
    g_object_unref (tp_file->priv->in_stream);

  if (tp_file->priv->out_stream)
    g_object_unref (tp_file->priv->out_stream);

  if (tp_file->priv->contact)
    g_object_unref (tp_file->priv->contact);

  if (tp_file->priv->cancellable)
    g_object_unref (tp_file->priv->cancellable);

  if (tp_file->priv->stalled_id != 0)
    g_source_remove (tp_file->priv->stalled_id);

  G_OBJECT_CLASS (empathy_tp_file_parent_class)->finalize (object);
}

static gboolean
tp_file_stalled_cb (EmpathyTpFile *tp_file)
{
  /* We didn't get transferred bytes update for a while, the transfer is
   * stalled. */

  tp_file->priv->speed = 0;
  tp_file->priv->remaining_time = -1;
  g_signal_emit (tp_file, signals[REFRESH], 0);

  return FALSE;
}

static void
tp_file_start_transfer (EmpathyTpFile *tp_file)
{
  gint fd;
  struct sockaddr_un addr;
  GArray *array;

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    {
      DEBUG ("Failed to create socket, closing channel");
      empathy_tp_file_cancel (tp_file);
      return;
    }

  array = g_value_get_boxed (tp_file->priv->socket_address);

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, array->data, array->len);

  if (connect (fd, (struct sockaddr*) &addr, sizeof (addr)) < 0)
    {
      DEBUG ("Failed to connect socket, closing channel");
      empathy_tp_file_cancel (tp_file);
      close (fd);
      return;
    }

  DEBUG ("Start the transfer");

  tp_file->priv->last_update_time = empathy_time_get_current ();
  tp_file->priv->last_update_transferred_bytes = tp_file->priv->transferred_bytes;
  tp_file->priv->stalled_id = g_timeout_add_seconds (STALLED_TIMEOUT,
    (GSourceFunc) tp_file_stalled_cb, tp_file);

  tp_file->priv->cancellable = g_cancellable_new ();
  if (tp_file->priv->incoming)
    {
      GInputStream *socket_stream;

      socket_stream = g_unix_input_stream_new (fd, TRUE);
      copy_stream (socket_stream, tp_file->priv->out_stream,
          tp_file->priv->cancellable);
      g_object_unref (socket_stream);
    }
  else
    {
      GOutputStream *socket_stream;

      socket_stream = g_unix_output_stream_new (fd, TRUE);
      copy_stream (tp_file->priv->in_stream, socket_stream,
          tp_file->priv->cancellable);
      g_object_unref (socket_stream);
    }
}

static void
tp_file_state_changed_cb (TpChannel *channel,
                          guint state,
                          guint reason,
                          gpointer user_data,
                          GObject *weak_object)
{
  EmpathyTpFile *tp_file = EMPATHY_TP_FILE (weak_object);

  if (state == tp_file->priv->state)
    return;

  DEBUG ("File transfer state changed:\n"
      "\tfilename = %s, old state = %u, state = %u, reason = %u\n"
      "\tincoming = %s, in_stream = %s, out_stream = %s",
      tp_file->priv->filename, tp_file->priv->state, state, reason,
      tp_file->priv->incoming ? "yes" : "no",
      tp_file->priv->in_stream ? "present" : "not present",
      tp_file->priv->out_stream ? "present" : "not present");

  /* If the channel is open AND we have the socket path, we can start the
   * transfer. The socket path could be NULL if we are not doing the actual
   * data transfer but are just an observer for the channel. */
  if (state == TP_FILE_TRANSFER_STATE_OPEN &&
      tp_file->priv->socket_address != NULL)
    tp_file_start_transfer (tp_file);

  tp_file->priv->state = state;
  tp_file->priv->state_change_reason = reason;

  g_object_notify (G_OBJECT (tp_file), "state");
}

static void
tp_file_transferred_bytes_changed_cb (TpChannel *channel,
                                      guint64 count,
                                      gpointer user_data,
                                      GObject *weak_object)
{
  EmpathyTpFile *tp_file = EMPATHY_TP_FILE (weak_object);
  time_t curr_time, elapsed_time;
  guint64 transferred_bytes;

  /* If we didn't progress since last update, return */
  if (tp_file->priv->transferred_bytes == count)
    return;

  /* Update the transferred bytes count */
  tp_file->priv->transferred_bytes = count;
  g_object_notify (G_OBJECT (tp_file), "transferred-bytes");

  /* We got a progress, reset the stalled timeout */
  if (tp_file->priv->stalled_id != 0)
    g_source_remove (tp_file->priv->stalled_id);
  tp_file->priv->stalled_id = g_timeout_add_seconds (STALLED_TIMEOUT,
    (GSourceFunc) tp_file_stalled_cb, tp_file);

  /* Calculate the transfer speed and remaining time estimation. We recalculate
   * that each second to get more dynamic values that react faster to network
   * changes. This is better than calculating the average from the begining of
   * the transfer, I think. */
  curr_time = empathy_time_get_current ();
  elapsed_time = curr_time - tp_file->priv->last_update_time;
  if (elapsed_time >= 1)
    {
      transferred_bytes = count - tp_file->priv->last_update_transferred_bytes;
      tp_file->priv->speed = (gdouble) transferred_bytes / (gdouble) elapsed_time;
      tp_file->priv->remaining_time = (tp_file->priv->size - count) /
        tp_file->priv->speed;
      tp_file->priv->last_update_transferred_bytes = count;
      tp_file->priv->last_update_time = curr_time;

      g_signal_emit (tp_file, signals[REFRESH], 0);
    }
}

static void
tp_file_check_if_ready (EmpathyTpFile *tp_file)
{
  if (tp_file->priv->ready || tp_file->priv->contact == NULL ||
      tp_file->priv->state == TP_FILE_TRANSFER_STATE_NONE)
    return;

  tp_file->priv->ready = TRUE;
  g_object_notify (G_OBJECT (tp_file), "ready");
}

static void
tp_file_got_contact_cb (EmpathyTpContactFactory *factory,
                        EmpathyContact *contact,
                        const GError *error,
                        gpointer user_data,
                        GObject *weak_object)
{
  EmpathyTpFile *tp_file = EMPATHY_TP_FILE (weak_object);

  if (error)
    {
      DEBUG ("Error: %s", error->message);
      empathy_tp_file_cancel (tp_file);
      return;
    }

  tp_file->priv->contact = g_object_ref (contact);
  tp_file_check_if_ready (tp_file);
}

static void
tp_file_get_all_cb (TpProxy *proxy,
                    GHashTable *properties,
                    const GError *error,
                    gpointer user_data,
                    GObject *file_obj)
{
  EmpathyTpFile *tp_file = EMPATHY_TP_FILE (file_obj);

  if (error)
    {
      DEBUG ("Error: %s", error->message);
      tp_cli_channel_call_close (tp_file->priv->channel, -1, NULL, NULL, NULL,
          NULL);
      return;
    }

  tp_file->priv->size = g_value_get_uint64 (
      g_hash_table_lookup (properties, "Size"));
  g_object_notify (file_obj, "size");

  tp_file->priv->state = g_value_get_uint (
      g_hash_table_lookup (properties, "State"));
  g_object_notify (file_obj, "state");

  tp_file->priv->transferred_bytes = g_value_get_uint64 (
      g_hash_table_lookup (properties, "TransferredBytes"));
  g_object_notify (file_obj, "transferred-bytes");

  tp_file->priv->filename = g_value_dup_string (
      g_hash_table_lookup (properties, "Filename"));
  g_object_notify (file_obj, "filename");

  tp_file->priv->content_hash = g_value_dup_string (
      g_hash_table_lookup (properties, "ContentHash"));
  g_object_notify (file_obj, "content-hash");

  tp_file->priv->content_hash_type = g_value_get_uint (
      g_hash_table_lookup (properties, "ContentHashType"));
  g_object_notify (file_obj, "content-hash-type");

  tp_file->priv->content_type = g_value_dup_string (
      g_hash_table_lookup (properties, "ContentType"));
  g_object_notify (file_obj, "content-type");

  tp_file->priv->description = g_value_dup_string (
      g_hash_table_lookup (properties, "Description"));

  tp_file_check_if_ready (tp_file);
}

static void
tp_file_get_requested_cb (TpProxy *proxy,
                          const GValue *requested,
                          const GError *error,
                          gpointer user_data,
                          GObject *weak_object)
{
  EmpathyTpFile *tp_file = EMPATHY_TP_FILE (weak_object);

  if (error)
    {
      DEBUG ("Error: %s", error->message);
      tp_cli_channel_call_close (tp_file->priv->channel, -1, NULL, NULL, NULL,
          NULL);
      return;
    }

  tp_file->priv->incoming = !g_value_get_boolean (requested);
  g_object_notify (G_OBJECT (tp_file), "incoming");

  tp_file_check_if_ready (tp_file);
}

static GObject *
tp_file_constructor (GType type,
                     guint n_props,
                     GObjectConstructParam *props)
{
  GObject *file_obj;
  EmpathyTpFile *tp_file;
  TpHandle handle;
  TpConnection *connection;

  file_obj = G_OBJECT_CLASS (empathy_tp_file_parent_class)->constructor (type,
      n_props, props);

  tp_file = EMPATHY_TP_FILE (file_obj);

  connection = tp_channel_borrow_connection (tp_file->priv->channel);
  tp_file->priv->factory = empathy_tp_contact_factory_dup_singleton (connection);
  tp_file->priv->mc = empathy_mission_control_dup_singleton ();
  tp_file->priv->state_change_reason =
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_NONE;

  g_signal_connect (tp_file->priv->channel, "invalidated",
    G_CALLBACK (tp_file_invalidated_cb), tp_file);

  tp_cli_channel_type_file_transfer_connect_to_file_transfer_state_changed (
      tp_file->priv->channel, tp_file_state_changed_cb, NULL, NULL,
      G_OBJECT (tp_file), NULL);

  tp_cli_channel_type_file_transfer_connect_to_transferred_bytes_changed (
      tp_file->priv->channel, tp_file_transferred_bytes_changed_cb,
      NULL, NULL, G_OBJECT (tp_file), NULL);

  tp_cli_dbus_properties_call_get (tp_file->priv->channel, -1,
      TP_IFACE_CHANNEL, "Requested",
      tp_file_get_requested_cb, NULL, NULL, file_obj);

  tp_cli_dbus_properties_call_get_all (tp_file->priv->channel, -1,
      TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER,
      tp_file_get_all_cb, NULL, NULL, file_obj);

  handle = tp_channel_get_handle (tp_file->priv->channel, NULL);
  empathy_tp_contact_factory_get_from_handle (tp_file->priv->factory,
      handle, tp_file_got_contact_cb, NULL, NULL, file_obj);

  return file_obj;
}

static void
tp_file_get_property (GObject *object,
                      guint param_id,
                      GValue *value,
                      GParamSpec *pspec)
{
  EmpathyTpFile *tp_file;

  tp_file = EMPATHY_TP_FILE (object);

  switch (param_id)
    {
      case PROP_CHANNEL:
        g_value_set_object (value, tp_file->priv->channel);
        break;
      case PROP_INCOMING:
        g_value_set_boolean (value, tp_file->priv->incoming);
        break;
      case PROP_STATE:
        g_value_set_uint (value, tp_file->priv->state);
        break;
      case PROP_CONTENT_TYPE:
        g_value_set_string (value, tp_file->priv->content_type);
        break;
      case PROP_FILENAME:
        g_value_set_string (value, tp_file->priv->filename);
        break;
      case PROP_SIZE:
        g_value_set_uint64 (value, tp_file->priv->size);
        break;
      case PROP_CONTENT_HASH_TYPE:
        g_value_set_uint (value, tp_file->priv->content_hash_type);
        break;
      case PROP_CONTENT_HASH:
        g_value_set_string (value, tp_file->priv->content_hash);
        break;
      case PROP_TRANSFERRED_BYTES:
        g_value_set_uint64 (value, tp_file->priv->transferred_bytes);
        break;
      case PROP_READY:
        g_value_set_boolean (value, tp_file->priv->ready);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    };
}

static void
tp_file_channel_set_dbus_property (gpointer proxy,
                                   const gchar *property,
                                   const GValue *value)
{
        DEBUG ("Setting %s property", property);
        tp_cli_dbus_properties_call_set (TP_PROXY (proxy), -1,
            TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, property, value,
            NULL, NULL, NULL, NULL);
}

static void
tp_file_set_property (GObject *object,
                      guint param_id,
                      const GValue *value,
                      GParamSpec *pspec)
{
  EmpathyTpFile *tp_file = (EmpathyTpFile *) object;
  switch (param_id)
    {
      case PROP_CHANNEL:
        tp_file->priv->channel = g_object_ref (g_value_get_object (value));
        break;
      case PROP_STATE:
        tp_file->priv->state = g_value_get_uint (value);
        break;
      case PROP_INCOMING:
        tp_file->priv->incoming = g_value_get_boolean (value);
        break;
      case PROP_FILENAME:
        g_free (tp_file->priv->filename);
        tp_file->priv->filename = g_value_dup_string (value);
        tp_file_channel_set_dbus_property (tp_file->priv->channel,
            "Filename", value);
        break;
      case PROP_SIZE:
        tp_file->priv->size = g_value_get_uint64 (value);
        tp_file_channel_set_dbus_property (tp_file->priv->channel,
            "Size", value);
        break;
      case PROP_CONTENT_TYPE:
        tp_file_channel_set_dbus_property (tp_file->priv->channel,
            "ContentType", value);
        g_free (tp_file->priv->content_type);
        tp_file->priv->content_type = g_value_dup_string (value);
        break;
      case PROP_CONTENT_HASH:
        tp_file_channel_set_dbus_property (tp_file->priv->channel,
            "ContentHash", value);
        g_free (tp_file->priv->content_hash);
        tp_file->priv->content_hash = g_value_dup_string (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
        break;
    };
}

static GHashTable *ft_table = NULL;

static void
tp_file_weak_notify_cb (gpointer channel,
                        GObject *tp_file)
{
  g_hash_table_remove (ft_table, channel);
}

/**
 * empathy_tp_file_new:
 * @channel: a #TpChannel
 *
 * Creates a new #EmpathyTpFile wrapping @channel, or return a new ref to an
 * existing #EmpathyTpFile for that channel. The returned #EmpathyTpFile
 * should be unrefed with g_object_unref() when finished with.
 *
 * Return value: a new #EmpathyTpFile
 */
EmpathyTpFile *
empathy_tp_file_new (TpChannel *channel)
{
  EmpathyTpFile *tp_file;

  g_return_val_if_fail (TP_IS_CHANNEL (channel), NULL);

  if (ft_table != NULL)
    {
      tp_file = g_hash_table_lookup (ft_table, channel);
      if (tp_file != NULL) {
        return g_object_ref (tp_file);
      }
    }
  else
    ft_table = g_hash_table_new_full (empathy_proxy_hash,
      empathy_proxy_equal, (GDestroyNotify) g_object_unref, NULL);

  tp_file = g_object_new (EMPATHY_TYPE_TP_FILE,
      "channel", channel,
      NULL);

  g_hash_table_insert (ft_table, g_object_ref (channel), tp_file);
  g_object_weak_ref (G_OBJECT (tp_file), tp_file_weak_notify_cb, channel);

  return tp_file;
}

/**
 * empathy_tp_file_get_channel
 * @tp_file: an #EmpathyTpFile
 *
 * Returns the #TpChannel associated with @tp_file.
 *
 * Returns: the #TpChannel associated with @tp_file
 */
TpChannel *
empathy_tp_file_get_channel (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), NULL);

  return tp_file->priv->channel;
}

static void
tp_file_method_cb (TpChannel *channel,
                   const GValue *address,
                   const GError *error,
                   gpointer user_data,
                   GObject *weak_object)
{
  EmpathyTpFile *tp_file = (EmpathyTpFile *) weak_object;
  GArray *array;

  if (error)
    {
      DEBUG ("Error: %s", error->message);
      empathy_tp_file_cancel (tp_file);
      return;
    }

  if (G_VALUE_TYPE (address) == DBUS_TYPE_G_UCHAR_ARRAY)
    {
      tp_file->priv->socket_address = tp_g_value_slice_dup (address);
    }
  else if (G_VALUE_TYPE (address) == G_TYPE_STRING)
    {
      /* Old bugged version of telepathy-salut used to store the address
       * as a 's' instead of an 'ay' */
      const gchar *path;

      path = g_value_get_string (address);
      array = g_array_sized_new (TRUE, FALSE, sizeof (gchar), strlen (path));
      g_array_insert_vals (array, 0, path, strlen (path));

      tp_file->priv->socket_address = tp_g_value_slice_new (
          DBUS_TYPE_G_UCHAR_ARRAY);
      g_value_set_boxed (tp_file->priv->socket_address, array);

      g_array_free (array, TRUE);
    }
  else
    {
      DEBUG ("Wrong address type: %s", G_VALUE_TYPE_NAME (address));
      empathy_tp_file_cancel (tp_file);
      return;
    }

  array = g_value_get_boxed (tp_file->priv->socket_address);
  DEBUG ("Got unix socket path: %s", array->data);

  if (tp_file->priv->state == TP_FILE_TRANSFER_STATE_OPEN)
    tp_file_start_transfer (tp_file);
}

/**
 * empathy_tp_file_accept:
 * @tp_file: an #EmpathyTpFile
 * @offset: position where to start the transfer
 * @gfile: a #GFile where to write transfered data
 * @error: a #GError set if there is an error when opening @gfile
 *
 * Accepts a file transfer that's in the "local pending" state (i.e.
 * %TP_FILE_TRANSFER_STATE_LOCAL_PENDING).
 */
void
empathy_tp_file_accept (EmpathyTpFile *tp_file,
                        guint64 offset,
                        GFile *gfile,
                        GError **error)
{
  GValue nothing = { 0 };

  g_return_if_fail (EMPATHY_IS_TP_FILE (tp_file));
  g_return_if_fail (G_IS_FILE (gfile));

  tp_file->priv->out_stream = G_OUTPUT_STREAM (g_file_replace (gfile, NULL,
  	FALSE, 0, NULL, error));
  if (error && *error)
    return;

  g_free (tp_file->priv->filename);
  tp_file->priv->filename = g_file_get_basename (gfile);
  g_object_notify (G_OBJECT (tp_file), "filename");

  DEBUG ("Accepting file: filename=%s", tp_file->priv->filename);

  g_value_init (&nothing, G_TYPE_STRING);
  g_value_set_static_string (&nothing, "");

  tp_cli_channel_type_file_transfer_call_accept_file (tp_file->priv->channel,
      -1, TP_SOCKET_ADDRESS_TYPE_UNIX, TP_SOCKET_ACCESS_CONTROL_LOCALHOST,
      &nothing, offset, tp_file_method_cb, NULL, NULL, G_OBJECT (tp_file));
}

/**
 * empathy_tp_file_offer:
 * @tp_file: an #EmpathyTpFile
 * @gfile: a #GFile where to read the data to transfer
 * @error: a #GError set if there is an error when opening @gfile
 *
 * Offers a file transfer that's in the "not offered" state (i.e.
 * %TP_FILE_TRANSFER_STATE_NOT_OFFERED).
 */
void
empathy_tp_file_offer (EmpathyTpFile *tp_file, GFile *gfile, GError **error)
{
  GValue nothing = { 0 };

  g_return_if_fail (EMPATHY_IS_TP_FILE (tp_file));

  tp_file->priv->in_stream = G_INPUT_STREAM (g_file_read (gfile, NULL, error));
  if (error && *error)
  	return;

  g_value_init (&nothing, G_TYPE_STRING);
  g_value_set_static_string (&nothing, "");

  tp_cli_channel_type_file_transfer_call_provide_file (tp_file->priv->channel,
      -1, TP_SOCKET_ADDRESS_TYPE_UNIX, TP_SOCKET_ACCESS_CONTROL_LOCALHOST,
      &nothing, tp_file_method_cb, NULL, NULL, G_OBJECT (tp_file));
}

/**
 * empathy_tp_file_get_contact:
 * @tp_file: an #EmpathyTpFile
 *
 * Returns the #EmpathyContact that @tp_file is open with.
 *
 * Return value: the #EmpathyContact that @tp_file is open with.
 */
EmpathyContact *
empathy_tp_file_get_contact (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), NULL);
  return tp_file->priv->contact;
}

const gchar *
empathy_tp_file_get_filename (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), NULL);
  return tp_file->priv->filename;
}

/**
 * empathy_tp_file_is_incoming:
 * @tp_file: an #EmpathyTpFile
 *
 * Returns whether @tp_file is incoming.
 *
 * Return value: %TRUE if the @tp_file is incoming, otherwise %FALSE
 */
gboolean
empathy_tp_file_is_incoming (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), FALSE);
  return tp_file->priv->incoming;
}

/**
 * empathy_tp_file_get_state:
 * @tp_file: an #EmpathyTpFile
 * @reason: return location for state change reason, or %NULL
 *
 * Gets the current state of @tp_file. If @reason is not %NULL, then
 * it is set to the reason of the last state change.
 *
 * Return value: a #TpFileTransferState
 */
TpFileTransferState
empathy_tp_file_get_state (EmpathyTpFile *tp_file,
                           TpFileTransferStateChangeReason *reason)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file),
      TP_FILE_TRANSFER_STATE_NONE);

  if (reason != NULL)
    *reason = tp_file->priv->state_change_reason;

  return tp_file->priv->state;
}

/**
 * empathy_tp_file_get_size:
 * @tp_file: an #EmpathyTpFile
 *
 * Gets the size of the file being transferred over @tp_file, in bytes.
 *
 * Return value: the size of the file being transferred, in bytes
 */
guint64
empathy_tp_file_get_size (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file),
      EMPATHY_TP_FILE_UNKNOWN_SIZE);
  return tp_file->priv->size;
}

/**
 * empathy_tp_file_get_transferred_bytes:
 * @tp_file: an #EmpathyTpFile
 *
 * Gets the number of transferred bytes of @tp_file so far, in bytes.
 *
 * Return value: number of transferred bytes of @tp_file, in bytes
 */
guint64
empathy_tp_file_get_transferred_bytes (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), 0);
  return tp_file->priv->transferred_bytes;
}

/**
 * empathy_tp_file_get_remaining_time:
 * @tp_file: a #EmpathyTpFile
 *
 * Gets the estimated time remaining of @tp_file, in seconds.
 *
 * Return value: the estimated time remaining of @tp_file, in seconds
 **/
gint
empathy_tp_file_get_remaining_time (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), -1);

  if (tp_file->priv->size == EMPATHY_TP_FILE_UNKNOWN_SIZE)
    return -1;

  if (tp_file->priv->transferred_bytes == tp_file->priv->size)
    return 0;

  return tp_file->priv->remaining_time;
}

/**
 * empathy_tp_file_get_speed:
 * @tp_file: an #EmpathyTpFile
 *
 * Gets the current speed of the transfer @tp_file, in bytes per
 * second.
 *
 * Return value: the current speed of the transfer @tp_file, in
 *               bytes per second
 **/
gdouble
empathy_tp_file_get_speed (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), 0);

  if (tp_file->priv->transferred_bytes == tp_file->priv->size)
    return 0;

  return tp_file->priv->speed;
}

const gchar *
empathy_tp_file_get_content_type (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), NULL);
  return tp_file->priv->content_type;
}

/**
 * empathy_tp_file_cancel:
 * @tp_file: an #EmpathyTpFile
 *
 * Cancels the file transfer, @tp_file.
 */
void
empathy_tp_file_cancel (EmpathyTpFile *tp_file)
{
  g_return_if_fail (EMPATHY_IS_TP_FILE (tp_file));

  DEBUG ("Closing channel..");
  tp_cli_channel_call_close (tp_file->priv->channel, -1,
    NULL, NULL, NULL, NULL);

  if (tp_file->priv->cancellable != NULL)
    g_cancellable_cancel (tp_file->priv->cancellable);
}

/**
 * empathy_tp_file_is_ready:
 * @tp_file: an #EmpathyTpFile
 *
 * Returns whether the file channel @tp_file is ready for use.
 *
 * @tp_file is classed as ready if its state is no longer
 * %TP_FILE_TRANSFER_STATE_NONE, or if details about the remote
 * contact have been fully received.
 *
 * Return value: %TRUE if @tp_file is ready for use
 */
gboolean
empathy_tp_file_is_ready (EmpathyTpFile *tp_file)
{
  g_return_val_if_fail (EMPATHY_IS_TP_FILE (tp_file), FALSE);

  return tp_file->priv->ready;
}

static void
empathy_tp_file_class_init (EmpathyTpFileClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = tp_file_finalize;
  object_class->constructor = tp_file_constructor;
  object_class->get_property = tp_file_get_property;
  object_class->set_property = tp_file_set_property;

  /* Construct-only properties */

  /**
   * EmpathyTpFile:channel:
   *
   * The #TpChannel associated with the #EmpathyTpFile.
   */
  g_object_class_install_property (object_class,
      PROP_CHANNEL,
      g_param_spec_object ("channel",
          "telepathy channel",
          "The file transfer channel",
          TP_TYPE_CHANNEL,
          G_PARAM_READWRITE |
          G_PARAM_CONSTRUCT_ONLY));

  /**
   * EmpathyTpFile:state:
   *
   * The #TpFileTransferState of the #EmpathyTpFile.
   */
  g_object_class_install_property (object_class,
      PROP_STATE,
      g_param_spec_uint ("state",
          "state of the transfer",
          "The file transfer state",
          0,
          G_MAXUINT,
          G_MAXUINT,
          G_PARAM_READWRITE |
          G_PARAM_CONSTRUCT));

  /**
   * EmpathyTpFile:incoming:
   *
   * Whether the #EmpathyTpFile is incoming.
   */
  g_object_class_install_property (object_class,
      PROP_INCOMING,
      g_param_spec_boolean ("incoming",
          "incoming",
          "Whether the transfer is incoming",
          FALSE,
          G_PARAM_READWRITE |
          G_PARAM_CONSTRUCT));

  /**
   * EmpathyTpFile:ready:
   *
   * Whether the #EmpathyTpFile is ready to use. This property returns
   * the same as empathy_tp_file_is_ready().
   */
  g_object_class_install_property (object_class,
      PROP_READY,
      g_param_spec_boolean ("ready",
          "ready",
          "Whether the object is ready",
          FALSE,
          G_PARAM_READABLE));

  /**
   * EmpathyTpFile:filename:
   *
   * The name of the file being transferred.
   */
  g_object_class_install_property (object_class,
      PROP_FILENAME,
      g_param_spec_string ("filename",
          "name of the transfer",
          "The file transfer filename",
          "",
          G_PARAM_READWRITE));

  /**
   * EmpathyTpFile:size:
   *
   * The size of the file being transferred.
   */
  g_object_class_install_property (object_class,
      PROP_SIZE,
      g_param_spec_uint64 ("size",
          "size of the file",
          "The file transfer size",
          0,
          G_MAXUINT64,
          G_MAXUINT64,
          G_PARAM_READWRITE));

  /**
   * EmpathyTpFile:content-type:
   *
   * The content-type of the file being transferred.
   */
  g_object_class_install_property (object_class,
      PROP_CONTENT_TYPE,
      g_param_spec_string ("content-type",
          "file transfer content-type",
          "The file transfer content-type",
          "",
          G_PARAM_READWRITE));

  /**
   * EmpathyTpFile:content-hash-type:
   *
   * The type of hash type stored in #EmpathyTpFile:content-hash,
   * from #TpFileHashType.
   */
  g_object_class_install_property (object_class,
      PROP_CONTENT_HASH_TYPE,
      g_param_spec_uint ("content-hash-type",
          "file transfer hash type",
          "The type of the file transfer hash",
          0,
          G_MAXUINT,
          0,
          G_PARAM_READWRITE));

  /**
   * EmpathyTpFile:content-hash:
   *
   * A hash of the contents of the file being transferred.
   */
  g_object_class_install_property (object_class,
      PROP_CONTENT_HASH,
      g_param_spec_string ("content-hash",
          "file transfer hash",
          "The hash of the transfer's contents",
          "",
          G_PARAM_READWRITE));

  /**
   * EmpathyTpFile:transferred-bytes:
   *
   * The number of bytes transferred in the #EmpathyTpFile.
   */
  g_object_class_install_property (object_class,
      PROP_TRANSFERRED_BYTES,
      g_param_spec_uint64 ("transferred-bytes",
          "bytes transferred",
          "The number of bytes transferred",
          0,
          G_MAXUINT64,
          0,
          G_PARAM_READWRITE));

  /**
   * EmpathyTpFile::refresh:
   * @tp_file: the #EmpathyTpFile
   *
   * The progress of @tp_file has changed. This can either be an update
   * in the number of bytes transferred, or it can be to inform of the
   * transfer stalling.
   *
   * This signal is designed for clients to provide more user feedback
   * when something to do with @tp_file changes. To avoid emitting this
   * signal too much, it is guaranteed that it will only ever be fired
   * at least every two seconds.
   */
  signals[REFRESH] = g_signal_new ("refresh", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);

  g_type_class_add_private (object_class, sizeof (EmpathyTpFilePriv));
}

