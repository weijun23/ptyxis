/*
 * ptyxis-surface-monitor.c
 *
 * Copyright 2023 Christian Hergert <christian@sourceandstack.com>
 * Copyright 2024 Sebastian Wick
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "ptyxis-enums.h"
#include "ptyxis-surface-monitor.h"

#define DELAY_INTERACTIVE_MSEC 100
#define DELAY_MIN_MSEC         500
#define DELAY_MAX_MSEC         10000

struct _PtyxisSurfaceMonitor
{
  GObject   parent_instance;
  GWeakRef  surface_wr;
  GSource  *update_source;
  int       current_delay_msec;
  guint     has_pressed_key : 1;
  guint     is_polling : 1;
};

enum {
  PROP_0,
  PROP_SURFACE,
  N_PROPS
};

G_DEFINE_FINAL_TYPE (PtyxisSurfaceMonitor, ptyxis_surface_monitor, G_TYPE_OBJECT)

static GParamSpec *properties[N_PROPS];

static gint64
ptyxis_surface_monitor_get_ready_time (PtyxisSurfaceMonitor *self)
{
  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));

  if (self->current_delay_msec < 1000)
    return g_get_monotonic_time () + (self->current_delay_msec * 1000);

  return (g_get_monotonic_time () / G_USEC_PER_SEC * G_USEC_PER_SEC) + (self->current_delay_msec * 1000);
}

static void
ptyxis_surface_monitor_reset_delay (PtyxisSurfaceMonitor *self)
{
  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));
  g_assert (self->update_source != NULL);

  self->current_delay_msec = DELAY_MIN_MSEC;
  g_source_set_ready_time (self->update_source,
                           ptyxis_surface_monitor_get_ready_time (self));
}

static void
ptyxis_surface_monitor_same_delay (PtyxisSurfaceMonitor *self)
{
  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));

  g_source_set_ready_time (self->update_source,
                           ptyxis_surface_monitor_get_ready_time (self));
}

static void
ptyxis_surface_monitor_backoff_delay (PtyxisSurfaceMonitor *self)
{
  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));
  g_assert (self->update_source != NULL);

  self->current_delay_msec = CLAMP (self->current_delay_msec * 2, DELAY_MIN_MSEC, DELAY_MAX_MSEC);
  g_source_set_ready_time (self->update_source,
                           ptyxis_surface_monitor_get_ready_time (self));
}

static void
ptyxis_surface_monitor_poll_agent_cb (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  PtyxisTerminalSurface *surface = (PtyxisTerminalSurface *)object;
  g_autoptr(PtyxisSurfaceMonitor) self = user_data;

  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));

  self->is_polling = FALSE;

  if (self->update_source == NULL)
    return;

  if (ptyxis_terminal_surface_poll_agent_finish (surface, result, NULL))
    ptyxis_surface_monitor_reset_delay (self);
  else
    ptyxis_surface_monitor_backoff_delay (self);
}

static gboolean
ptyxis_surface_monitor_update_source_func (gpointer user_data)
{
  PtyxisSurfaceMonitor *self = user_data;
  g_autoptr(PtyxisTerminalSurface) surface = NULL;
  PtyxisIpcProcess *process;

  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));

  if ((surface = g_weak_ref_get (&self->surface_wr)) &&
      (process = ptyxis_terminal_surface_get_process (surface)))
    {
      ptyxis_surface_monitor_same_delay (self);

      if (!self->is_polling)
        {
          self->is_polling = TRUE;

          ptyxis_terminal_surface_poll_agent_async (surface,
                                                    NULL,
                                                    ptyxis_surface_monitor_poll_agent_cb,
                                                    g_object_ref (self));
        }

      return G_SOURCE_CONTINUE;
    }

  g_clear_pointer (&self->update_source, g_source_unref);

  return G_SOURCE_REMOVE;
}

static gboolean
ptyxis_surface_monitor_dispatch (GSource     *source,
                                 GSourceFunc  callback,
                                 gpointer     data)
{
  return callback (data);
}

static const GSourceFuncs source_funcs = {
  .dispatch = ptyxis_surface_monitor_dispatch,
};

static void
ptyxis_surface_monitor_queue_update (PtyxisSurfaceMonitor *self)
{
  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));

  if G_UNLIKELY (self->update_source == NULL)
    {
      self->update_source = g_source_new ((GSourceFuncs *)&source_funcs, sizeof (GSource));
      g_source_set_callback (self->update_source,
                             ptyxis_surface_monitor_update_source_func,
                             self, NULL);
      g_source_set_static_name (self->update_source, "[ptyxis-surface-monitor]");
      g_source_set_priority (self->update_source, G_PRIORITY_LOW);
      ptyxis_surface_monitor_reset_delay (self);
      g_source_attach (self->update_source, NULL);
      return;
    }

  if G_UNLIKELY (self->current_delay_msec > DELAY_MIN_MSEC)
    {
      ptyxis_surface_monitor_reset_delay (self);
      return;
    }
}

static void
ptyxis_surface_monitor_terminal_contents_changed_cb (PtyxisSurfaceMonitor *self,
                                                     PtyxisTerminal       *terminal)
{
  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  ptyxis_surface_monitor_queue_update (self);
}

static gboolean
ptyxis_surface_monitor_key_pressed_cb (PtyxisSurfaceMonitor  *self,
                                       guint                  keyval,
                                       guint                  keycode,
                                       GdkModifierType        state,
                                       GtkEventControllerKey *key)
{
  gboolean low_delay = FALSE;

  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));
  g_assert (GTK_IS_EVENT_CONTROLLER_KEY (key));

  ptyxis_surface_monitor_set_has_pressed_key (self, TRUE);

  if (self->update_source == NULL)
    return GDK_EVENT_PROPAGATE;

  state &= gtk_accelerator_get_default_mod_mask ();

  switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_ISO_Enter:
    case GDK_KEY_KP_Enter:
      low_delay = TRUE;
      break;

    case GDK_KEY_c:
    case GDK_KEY_d:
      low_delay = !!(state & GDK_CONTROL_MASK);
      break;

    default:
      break;
    }

  if (low_delay)
    {
      self->current_delay_msec = DELAY_INTERACTIVE_MSEC;
      g_source_set_ready_time (self->update_source,
                               ptyxis_surface_monitor_get_ready_time (self));
    }

  return GDK_EVENT_PROPAGATE;
}

static void
ptyxis_surface_monitor_setup (PtyxisSurfaceMonitor  *self,
                              PtyxisTerminalSurface *surface,
                              GtkWidget             *owner)
{
  PtyxisTerminal *terminal;
  GtkEventController *controller;

  g_assert (PTYXIS_IS_SURFACE_MONITOR (self));
  g_assert (PTYXIS_IS_TERMINAL_SURFACE (surface));
  g_assert (GTK_IS_WIDGET (owner));

  g_weak_ref_set (&self->surface_wr, surface);

  terminal = ptyxis_terminal_surface_get_terminal (surface);

  g_signal_connect_object (terminal,
                           "contents-changed",
                           G_CALLBACK (ptyxis_surface_monitor_terminal_contents_changed_cb),
                           self,
                           G_CONNECT_SWAPPED);

  controller = gtk_event_controller_key_new ();
  g_signal_connect_object (controller,
                           "key-pressed",
                           G_CALLBACK (ptyxis_surface_monitor_key_pressed_cb),
                           self,
                           G_CONNECT_SWAPPED);
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  gtk_widget_add_controller (owner, controller);
}

static void
ptyxis_surface_monitor_finalize (GObject *object)
{
  PtyxisSurfaceMonitor *self = (PtyxisSurfaceMonitor *)object;

  if (self->update_source != NULL)
    {
      g_source_destroy (self->update_source);
      g_clear_pointer (&self->update_source, g_source_unref);
    }

  g_weak_ref_clear (&self->surface_wr);

  G_OBJECT_CLASS (ptyxis_surface_monitor_parent_class)->finalize (object);
}

static void
ptyxis_surface_monitor_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  PtyxisSurfaceMonitor *self = PTYXIS_SURFACE_MONITOR (object);

  switch (prop_id)
    {
    case PROP_SURFACE:
      g_value_take_object (value, g_weak_ref_get (&self->surface_wr));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_surface_monitor_class_init (PtyxisSurfaceMonitorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = ptyxis_surface_monitor_finalize;
  object_class->get_property = ptyxis_surface_monitor_get_property;

  properties[PROP_SURFACE] =
    g_param_spec_object ("surface", NULL, NULL,
                         PTYXIS_TYPE_TERMINAL_SURFACE,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
ptyxis_surface_monitor_init (PtyxisSurfaceMonitor *self)
{
  self->current_delay_msec = DELAY_MIN_MSEC;

  g_weak_ref_init (&self->surface_wr, NULL);
}

PtyxisSurfaceMonitor *
ptyxis_surface_monitor_new (PtyxisTerminalSurface *surface,
                            GtkWidget             *owner)
{
  PtyxisSurfaceMonitor *self;

  g_return_val_if_fail (PTYXIS_IS_TERMINAL_SURFACE (surface), NULL);
  g_return_val_if_fail (GTK_IS_WIDGET (owner), NULL);

  self = g_object_new (PTYXIS_TYPE_SURFACE_MONITOR, NULL);
  ptyxis_surface_monitor_setup (self, surface, owner);

  return self;
}

gboolean
ptyxis_surface_monitor_get_has_pressed_key (PtyxisSurfaceMonitor *self)
{
  g_return_val_if_fail (PTYXIS_IS_SURFACE_MONITOR (self), FALSE);

  return self->has_pressed_key;
}

void
ptyxis_surface_monitor_set_has_pressed_key (PtyxisSurfaceMonitor *self,
                                            gboolean              has_pressed_key)
{
  g_return_if_fail (PTYXIS_IS_SURFACE_MONITOR (self));

  self->has_pressed_key = has_pressed_key;
}
