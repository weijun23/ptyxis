/*
 * ptyxis-terminal-intent.c
 *
 * Copyright 2025 Christian Hergert <christian@sourceandstack.com>
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

#include "gio/gdesktopappinfo.h"

#include "ptyxis-agent-ipc.h"

#include "ptyxis-application.h"
#include "ptyxis-profile.h"
#include "ptyxis-tab.h"
#include "ptyxis-terminal-intent.h"
#include "ptyxis-window.h"

struct _PtyxisTerminalIntent
{
  FdoTerminal1Skeleton parent_instance;
};

static void default_init_terminal1 (FdoTerminal1Iface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (PtyxisTerminalIntent, ptyxis_terminal_intent, FDO_TYPE_TERMINAL1_SKELETON,
                               G_IMPLEMENT_INTERFACE (FDO_TYPE_TERMINAL1, default_init_terminal1))

static void
ptyxis_terminal_intent_class_init (PtyxisTerminalIntentClass *klass)
{
}

static void
ptyxis_terminal_intent_init (PtyxisTerminalIntent *self)
{
}

static gboolean
unpack_commands (GVariant   *commands,
                 GPtrArray  *argvs,
                 GPtrArray  *envs,
                 GPtrArray  *cwd_uris,
                 GError    **error)
{
  GVariantIter iter;
  GVariant *command;

  g_variant_iter_init (&iter, commands);
  while (g_variant_iter_loop (&iter, "@a{sv}", &command))
    {
      g_auto(GStrv) argv = NULL;
      g_auto(GStrv) env = NULL;
      const char *working_directory = NULL;
      g_autoptr(GFile) file = NULL;
      g_autofree char *cwd_uri = NULL;

      g_assert (command != NULL);
      g_assert (g_variant_is_of_type (command, G_VARIANT_TYPE_VARDICT));

      g_variant_lookup (command, "exec", "^aay", &argv);
      g_variant_lookup (command, "env", "^aay", &env);
      g_variant_lookup (command, "working_directory", "^&ay", &working_directory);

      if (argv != NULL && (argv[0] == NULL || argv[0][0] == '\0'))
        argv = NULL;

      if (env != NULL)
        {
          if (env[0] == NULL)
            env = NULL;

          for (size_t i = 0; env != NULL && env[i] != NULL; i++)
            {
              if (strchr (env[i], '=') == NULL)
                {
                  g_set_error_literal (error,
                                       G_DBUS_ERROR,
                                       G_DBUS_ERROR_INVALID_ARGS,
                                       "environ pair missing `=`");
                  return FALSE;
                }
            }
        }

      if (working_directory != NULL)
        {
          if (working_directory[0] == '/')
            file = g_file_new_for_path (working_directory);
          else if (working_directory[0] != 0)
            file = g_file_new_build_filename (g_get_home_dir (), working_directory, NULL);

          if (file != NULL)
            cwd_uri = g_file_get_uri (file);
        }

      g_ptr_array_add (argvs, g_steal_pointer (&argv));
      g_ptr_array_add (envs, g_steal_pointer (&env));
      g_ptr_array_add (cwd_uris, g_steal_pointer (&cwd_uri));
    }

  return TRUE;
}

static gboolean
unpack_desktop_entry (const char  *desktop_entry_path,
                      char       **name,
                      char       **path,
                      GError     **error)
{
  g_autoptr (GDesktopAppInfo) desktop_app_info = NULL;
  g_autofree char *path_value = NULL;
  g_autoptr(GFile) path_file = NULL;

  if (desktop_entry_path[0] == '\0')
    return TRUE;

  desktop_app_info = g_desktop_app_info_new_from_filename (desktop_entry_path);

  if (desktop_app_info == NULL)
    {
      g_set_error_literal (error,
                           G_DBUS_ERROR,
                           G_DBUS_ERROR_INVALID_ARGS,
                           "Not a valid path to a Desktop Entry");
      return FALSE;
    }

  *name = g_strdup (g_app_info_get_name (G_APP_INFO (desktop_app_info)));

  path_value = g_desktop_app_info_get_string (desktop_app_info, "Path");

  if (path_value != NULL)
    {
      if (g_path_is_absolute (path_value))
        path_file = g_file_new_for_path (path_value);
      else
        path_file = g_file_new_build_filename (g_get_home_dir (), path_value, NULL);

      if (path_file != NULL)
        *path = g_file_get_uri (path_file);
    }

  return TRUE;
}

static gboolean
ptyxis_terminal_intent_handle_launch_command (FdoTerminal1          *intent,
                                              GDBusMethodInvocation *invocation,
                                              GVariant              *commands,
                                              const char            *desktop_entry_path,
                                              GVariant              *options,
                                              GVariant              *platform_data)
{
  g_autoptr(PtyxisProfile) profile = NULL;
  g_autoptr(PtyxisIpcContainer) container = NULL;
  g_autoptr(GPtrArray) argvs = NULL;
  g_autoptr(GPtrArray) envs = NULL;
  g_autoptr(GPtrArray) cwd_uris = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  gboolean keep_terminal_open = FALSE;
  PtyxisWindow *window;

  g_assert (PTYXIS_IS_TERMINAL_INTENT (intent));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (commands != NULL);
  g_assert (g_variant_is_of_type (commands, G_VARIANT_TYPE_ARRAY));
  g_assert (desktop_entry_path != NULL);
  g_assert (options != NULL);
  g_assert (g_variant_is_of_type (options, G_VARIANT_TYPE_VARDICT));
  g_assert (platform_data != NULL);
  g_assert (g_variant_is_of_type (platform_data, G_VARIANT_TYPE_VARDICT));

  profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);
  container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, "session");

  if (container == NULL)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              g_error_new_literal (G_DBUS_ERROR,
                                                                   G_DBUS_ERROR_INVALID_ARGS,
                                                                   "Failed to locate session container"));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  argvs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  envs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  cwd_uris = g_ptr_array_new_with_free_func (g_free);

  if (!unpack_commands (commands, argvs, envs, cwd_uris, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              g_steal_pointer (&error));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!unpack_desktop_entry (desktop_entry_path, &name, &path, &error))
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              g_steal_pointer (&error));
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  g_variant_lookup (options, "keep-terminal-open", "b", &keep_terminal_open);

  if (argvs->len == 0)
    {
      PtyxisTab *tab;

      window = ptyxis_window_new ();
      tab = ptyxis_window_get_active_tab (window);

      if (path != NULL)
        ptyxis_tab_set_initial_working_directory_uri (tab, path);

      if (name != NULL)
        {
          ptyxis_tab_set_ignore_osc_title (tab, TRUE);
          ptyxis_tab_set_title_prefix (tab, name);
        }

      if (keep_terminal_open)
        ptyxis_tab_set_keep_terminal_open (tab, TRUE);
    }
  else
    window = ptyxis_window_new_empty ();

  for (size_t i = 0; i < argvs->len; i++)
    {
      PtyxisTab *tab;
      const char *const *argv = argvs->pdata[i];
      const char *const *env = envs->pdata[i];
      const char *cwd_uri = cwd_uris->pdata[i];

      tab = ptyxis_tab_new (profile);

      if (argv != NULL)
        ptyxis_tab_set_command (tab, argv);

      if (env != NULL)
        ptyxis_tab_set_environment (tab, env);

      if (cwd_uri != NULL)
        ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);
      else if (path != NULL)
        ptyxis_tab_set_initial_working_directory_uri (tab, path);

      if (name != NULL)
        {
          ptyxis_tab_set_ignore_osc_title (tab, TRUE);
          ptyxis_tab_set_title_prefix (tab, name);
        }

      if (keep_terminal_open)
        ptyxis_tab_set_keep_terminal_open (tab, TRUE);

      ptyxis_window_add_tab (window, tab);

      if (i == 0)
        ptyxis_window_set_active_tab (window, tab);
    }

  gtk_window_present (GTK_WINDOW (window));

  fdo_terminal1_complete_launch_command (intent, g_steal_pointer (&invocation));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
default_init_terminal1 (FdoTerminal1Iface *iface)
{
  iface->handle_launch_command = ptyxis_terminal_intent_handle_launch_command;
}

PtyxisTerminalIntent *
ptyxis_terminal_intent_new (void)
{
  return g_object_new (PTYXIS_TYPE_TERMINAL_INTENT, NULL);
}
