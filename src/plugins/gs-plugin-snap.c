/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015-2016 Canonical Ltd
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>

#include <snapd-glib/snapd-glib.h>
#include <gnome-software.h>

#include "gs-snapd.h"

struct GsPluginData {
	GsAuth		*auth;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	// FIXME
	/*if (!gs_snapd_exists ()) {
		g_debug ("disabling '%s' as snapd not running",
			 gs_plugin_get_name (plugin));
		gs_plugin_set_enabled (plugin, FALSE);
	}*/

	priv->auth = gs_auth_new ("snapd");
	gs_auth_set_provider_name (priv->auth, "Snap Store");
	gs_auth_set_provider_schema (priv->auth, "com.ubuntu.UbuntuOne.GnomeSoftware");
	gs_plugin_add_auth (plugin, priv->auth);

	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "desktop-categories");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "ubuntu-reviews");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_BETTER_THAN, "packagekit");
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* load from disk */
	gs_auth_add_metadata (priv->auth, "macaroon", NULL);
	if (!gs_auth_store_load (priv->auth,
				 GS_AUTH_STORE_FLAG_USERNAME |
				 GS_AUTH_STORE_FLAG_METADATA,
				 cancellable, error))
		return FALSE;

	/* success */
	return TRUE;
}

static SnapdAuthData *
get_auth (GsPlugin *plugin)
{
	GsAuth *auth;
	const gchar *serialized_macaroon;
	g_autoptr(GVariant) macaroon_variant = NULL;
	const gchar *macaroon;
	g_auto(GStrv) discharges = NULL;

	auth = gs_plugin_get_auth_by_id (plugin, "snapd");
	if (auth == NULL)
		return NULL;
	serialized_macaroon = gs_auth_get_metadata_item (auth, "macaroon");
	if (serialized_macaroon == NULL)
		return NULL;
	macaroon_variant = g_variant_parse (G_VARIANT_TYPE ("(sas)"),
					    serialized_macaroon,
					    NULL,
					    NULL,
					    NULL);
	if (macaroon_variant == NULL)
		return NULL;
	g_variant_get (macaroon_variant, "(&s^as)", &macaroon, &discharges);

	return snapd_auth_data_new (macaroon, discharges);
}

static SnapdClient *
get_client (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	g_autoptr(SnapdClient) client = NULL;

	client = snapd_client_new ();
	if (!snapd_client_connect_sync (client, cancellable, error))
		return NULL;
	//snapd_client_set_auth_data (client, get_auth (plugin));

	return g_steal_pointer (&client);
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_clear_object (&priv->auth);
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GsAppList *list,
			 GCancellable *cancellable,
			 GError **error)
{
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr(GPtrArray) snaps = NULL;
	guint i;

	client = get_client (plugin, cancellable, error);
	if (client == NULL)
		return FALSE;
	snaps = snapd_client_list_sync (client, cancellable, error);
	if (snaps == NULL) {
		// FIXME: Convert error?
		return FALSE;
	}
	for (i = 0; i < snaps->len; i++) {
		SnapdSnap *snap = snaps->pdata[i];
		g_autoptr(GsApp) app = NULL;

		if (snapd_snap_get_status (snap) != SNAPD_SNAP_STATUS_ACTIVE)
			continue;

		app = gs_app_new (snapd_snap_get_name (snap));
		gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_SNAP);
		gs_app_set_management_plugin (app, "snap");
		gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_REVIEWABLE);
		gs_app_list_add (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
		      gchar **values,
		      GsAppList *list,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autoptr(SnapdClient) client = NULL;
	g_autofree gchar *query = NULL;
	g_autoptr(GPtrArray) snaps = NULL;
	guint i;

	client = get_client (plugin, cancellable, error);
	if (client == NULL)
		return FALSE;
	query = g_strjoinv (" ", values);

	snaps = snapd_client_find_sync (client, SNAPD_FIND_FLAGS_NONE, query, NULL, cancellable, error);
	if (snaps == NULL) {
		// FIXME: Convert error?
		return FALSE;
	}
	for (i = 0; i < snaps->len; i++) {
		SnapdSnap *snap = snaps->pdata[i];
		gint64 size;
		g_autoptr(GsApp) app = NULL;

		app = gs_app_new (snapd_snap_get_name (snap));
		gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_SNAP);
		gs_app_set_management_plugin (app, "snap");
		gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_REVIEWABLE);
		size = snapd_snap_get_download_size (snap);
		if (size > 0)
			gs_app_set_size_download (app, (guint64) size);
		gs_app_list_add (list, app);
	}

	return TRUE;
}

static gboolean
load_icon (GsPlugin *plugin, GsApp *app, SnapdClient *client, SnapdSnap *snap, GCancellable *cancellable, GError **error)
{
	const gchar *icon_url;
	g_autoptr(GdkPixbuf) icon_pixbuf = NULL;

	icon_url = snapd_snap_get_icon (snap);
	if (g_str_has_prefix (icon_url, "/")) {
		/*g_autoptr(SnapdIcon) icon = NULL;
		gsize icon_response_length;

		icon = snapd_client_get_icon_sync (
		if (gs_snapd_request ("GET", icon_url, NULL,
				      macaroon, discharges,
				      NULL, NULL,
				      NULL, &icon_response, &icon_response_length,
				      cancellable, NULL)) {
			g_autoptr(GdkPixbufLoader) loader = NULL;

			loader = gdk_pixbuf_loader_new ();
			gdk_pixbuf_loader_write (loader,
						 (guchar *) icon_response,
						 icon_response_length,
						 NULL);
			gdk_pixbuf_loader_close (loader, NULL);
			icon_pixbuf = g_object_ref (gdk_pixbuf_loader_get_pixbuf (loader));
		}
		else
			g_printerr ("Failed to get icon\n");*/
	}
	else {
		g_autoptr(SoupMessage) message = NULL;
		g_autoptr(GdkPixbufLoader) loader = NULL;

		message = soup_message_new (SOUP_METHOD_GET, icon_url);
		if (message != NULL) {
			soup_session_send_message (gs_plugin_get_soup_session (plugin), message);
			loader = gdk_pixbuf_loader_new ();
			gdk_pixbuf_loader_write (loader,
						 (guint8 *) message->response_body->data,
						 (gsize) message->response_body->length,
						 NULL);
			gdk_pixbuf_loader_close (loader, NULL);
			icon_pixbuf = g_object_ref (gdk_pixbuf_loader_get_pixbuf (loader));
		}
	}

	if (icon_pixbuf) {
		gs_app_set_pixbuf (app, icon_pixbuf);
	} else {
		g_autoptr(AsIcon) icon = as_icon_new ();
		as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
		as_icon_set_name (icon, "package-x-generic");
		gs_app_add_icon (app, icon);
	}

	return TRUE;
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autoptr(SnapdClient) client = NULL;
	g_autoptr(SnapdSnap) snap = NULL;
	gint64 size;
	GPtrArray *apps;
	const gchar *launch_name = NULL;

	/* not us */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	client = get_client (plugin, cancellable, error);
	if (client == NULL)
		return FALSE;

	snap = snapd_client_list_one_sync (client, gs_app_get_id (app), cancellable, error);
	if (snap == NULL) {
		// FIXME: Convert error?
		return FALSE;
	}

	switch (snapd_snap_get_status (snap)) {
	default:
	case SNAPD_SNAP_STATUS_UNKNOWN:
		break;
	case SNAPD_SNAP_STATUS_INSTALLED:
	case SNAPD_SNAP_STATUS_ACTIVE:
		if (gs_app_get_state (app) == AS_APP_STATE_AVAILABLE)
			gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		break;
	case SNAPD_SNAP_STATUS_AVAILABLE:
	case SNAPD_SNAP_STATUS_PRICED:
		break;
	}

	gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, snapd_snap_get_summary (snap));
	gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST, snapd_snap_get_description (snap));
	gs_app_set_version (app, snapd_snap_get_version (snap));
	size = snapd_snap_get_installed_size (snap);
	if (size > 0)
		gs_app_set_size_installed (app, (guint64) size);
	size = snapd_snap_get_download_size (snap);
	gs_app_add_quirk (app, AS_APP_QUIRK_PROVENANCE);

	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON != 0)
		if (!load_icon (plugin, app, client, snap, cancellable, error))
			return FALSE;

	apps = snapd_snap_get_apps (snap);
	if (apps != NULL && apps->len > 0)
		launch_name = snapd_app_get_name (apps->pdata[0]);

	if (launch_name)
		gs_app_set_metadata (app, "snap::launch-name", launch_name);
	else
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);

	return TRUE;
}

static void
progress_cb (SnapdClient *client, SnapdTask *main_task, GPtrArray *tasks, gpointer user_data)
{
	GsApp *app = user_data;
	guint i;
	gint64 done, total;

	done = 0;
	total = 0;
	for (i = 0; i < tasks->len; i++) {
		SnapdTask *task = tasks->pdata[i];
		done += snapd_task_get_progress_done (task);
		total += snapd_task_get_progress_total (task);
	}

	if (total > 0)
		gs_app_set_progress (app, (guint) (100 * done / total));
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autoptr(SnapdClient) client = NULL;

	/* We can only install apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	client = get_client (plugin, cancellable, error);
	if (client == NULL)
		return FALSE;
	if (!snapd_client_install_sync (client,
					gs_app_get_id (app), NULL,
					progress_cb, app,
					cancellable, error)) {
		gs_app_set_state_recover (app);
		return FALSE;
	}
	gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	const gchar *launch_name;
	g_autofree gchar *binary_name = NULL;
	g_autoptr(GAppInfo) info = NULL;

	/* We can only launch apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	launch_name = gs_app_get_metadata_item (app, "snap::launch-name");
	if (!launch_name)
		return TRUE;

	if (g_strcmp0 (launch_name, gs_app_get_id (app)) == 0)
		binary_name = g_strdup_printf ("/snap/bin/%s", launch_name);
	else
		binary_name = g_strdup_printf ("/snap/bin/%s.%s", gs_app_get_id (app), launch_name);

	// FIXME: Since we don't currently know if this app needs a terminal or not we launch everything with one
	// https://bugs.launchpad.net/bugs/1595023
	info = g_app_info_create_from_commandline (binary_name, NULL, G_APP_INFO_CREATE_NEEDS_TERMINAL, error);
	if (info == NULL)
		return FALSE;

	return g_app_info_launch (info, NULL, NULL, error);
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autoptr(SnapdClient) client = NULL;

	/* We can only remove apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	client = get_client (plugin, cancellable, error);
	if (client == NULL)
		return FALSE;
	if (!snapd_client_remove_sync (client,
				       gs_app_get_id (app),
				       progress_cb, app,
				       cancellable, error)) {
		gs_app_set_state_recover (app);
		return FALSE;
	}
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	return TRUE;
}

gboolean
gs_plugin_auth_login (GsPlugin *plugin, GsAuth *auth,
		      GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(SnapdAuthData) snapd_auth = NULL;
	g_autoptr(GVariant) macaroon_variant = NULL;
	g_autofree gchar *serialized_macaroon = NULL;
	g_autoptr(GError) local_error = NULL;

	if (auth != priv->auth)
		return TRUE;

	snapd_auth = snapd_login_sync (gs_auth_get_username (auth), gs_auth_get_password (auth), gs_auth_get_pin (auth), cancellable, error);
	if (snapd_auth == NULL) {
		if (g_error_matches (local_error, SNAPD_ERROR, SNAPD_ERROR_TWO_FACTOR_REQUIRED)) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_PIN_REQUIRED,
					     local_error->message);
		}
		else {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_FAILED,
					     local_error->message);
		}
		return FALSE;
	}

	macaroon_variant = g_variant_new ("(s^as)",
					  snapd_auth_data_get_macaroon (snapd_auth),
					  snapd_auth_data_get_discharges (snapd_auth));
	serialized_macaroon = g_variant_print (macaroon_variant, FALSE);
	gs_auth_add_metadata (auth, "macaroon", serialized_macaroon);

	/* store */
	if (!gs_auth_store_save (auth,
				 GS_AUTH_STORE_FLAG_USERNAME |
				 GS_AUTH_STORE_FLAG_METADATA,
				 cancellable, error))
		return FALSE;

	gs_auth_add_flags (priv->auth, GS_AUTH_FLAG_VALID);

	return TRUE;
}

gboolean
gs_plugin_auth_lost_password (GsPlugin *plugin, GsAuth *auth,
			      GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (auth != priv->auth)
		return TRUE;

	// FIXME: snapd might not be using Ubuntu One accounts
	// https://bugs.launchpad.net/bugs/1598667
	g_set_error_literal (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_AUTH_INVALID,
			     "do online using @https://login.ubuntu.com/+forgot_password");
	return FALSE;
}

gboolean
gs_plugin_auth_register (GsPlugin *plugin, GsAuth *auth,
			 GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (auth != priv->auth)
		return TRUE;

	// FIXME: snapd might not be using Ubuntu One accounts
	// https://bugs.launchpad.net/bugs/1598667
	g_set_error_literal (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_AUTH_INVALID,
			     "do online using @https://login.ubuntu.com/+login");
	return FALSE;
}
