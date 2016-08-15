/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
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

#define I_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE
#include <packagekit-glib2/packagekit.h>

#include <gs-plugin.h>

#define GS_PLUGIN_PACKAGEKIT_HISTORY_TIMEOUT	5000 /* ms */

/*
 * SECTION:
 * This returns update history using the system PackageKit instance.
 */

struct GsPluginPrivate {
	GDBusConnection		*connection;
};

/**
 * gs_plugin_get_name:
 */
const gchar *
gs_plugin_get_name (void)
{
	return "packagekit-history";
}

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
}

/**
 * gs_plugin_order_after:
 */
const gchar **
gs_plugin_order_after (GsPlugin *plugin)
{
	static const gchar *deps[] = {
		"appstream",		/* need pkgname */
		"packagekit-refine",	/* need pkgname */
		NULL };
	return deps;
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
	if (plugin->priv->connection != NULL)
		g_object_unref (plugin->priv->connection);
}

/**
 * gs_plugin_packagekit_refine_add_history:
 */
static void
gs_plugin_packagekit_refine_add_history (GsApp *app, GVariant *dict)
{
	const gchar *version;
	gboolean ret;
	guint64 timestamp;
	PkInfoEnum info_enum;
	g_autoptr(GsApp) history = NULL;

	/* create new history item with same ID as parent */
	history = gs_app_new (gs_app_get_id (app));
	gs_app_set_kind (history, AS_APP_KIND_GENERIC);
	gs_app_set_name (history, GS_APP_QUALITY_NORMAL, gs_app_get_name (app));

	/* get the installed state */
	ret = g_variant_lookup (dict, "info", "u", &info_enum);
	g_assert (ret);
	switch (info_enum) {
	case PK_INFO_ENUM_INSTALLING:
		gs_app_set_state (history, AS_APP_STATE_INSTALLED);
		break;
	case PK_INFO_ENUM_REMOVING:
		gs_app_set_state (history, AS_APP_STATE_AVAILABLE);
		break;
	case PK_INFO_ENUM_UPDATING:
		gs_app_set_state (history, AS_APP_STATE_UPDATABLE);
		break;
	default:
		g_debug ("ignoring history kind: %s",
			 pk_info_enum_to_string (info_enum));
		return;
	}

	/* set the history time and date */
	ret = g_variant_lookup (dict, "timestamp", "t", &timestamp);
	g_assert (ret);
	gs_app_set_install_date (history, timestamp);

	/* set the history version number */
	ret = g_variant_lookup (dict, "version", "&s", &version);
	g_assert (ret);
	gs_app_set_version (history, version);

	/* add the package to the main application */
	gs_app_add_history (app, history);

	/* use the last event as approximation of the package timestamp */
	gs_app_set_install_date (app, timestamp);
}

/**
 * gs_plugin_setup:
 */
gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	plugin->priv->connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM,
						   cancellable,
						   error);
	return plugin->priv->connection != NULL;
}

static gboolean
gs_plugin_packagekit_refine (GsPlugin *plugin,
			     GList *list,
			     GCancellable *cancellable,
			     GError **error)
{
	gboolean ret;
	GError *error_local = NULL;
	GList *l;
	GsApp *app;
	guint i = 0;
	GVariantIter iter;
	GVariant *value;
	g_autofree const gchar **package_names = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GVariant) tuple = NULL;

	/* get an array of package names */
	package_names = g_new0 (const gchar *, g_list_length (list) + 1);
	for (l = list; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		package_names[i++] = gs_app_get_source_default (app);
	}

	g_debug ("getting history for %i packages", g_list_length (list));
	result = g_dbus_connection_call_sync (plugin->priv->connection,
					      "org.freedesktop.PackageKit",
					      "/org/freedesktop/PackageKit",
					      "org.freedesktop.PackageKit",
					      "GetPackageHistory",
					      g_variant_new ("(^asu)", package_names, 0),
					      NULL,
					      G_DBUS_CALL_FLAGS_NONE,
					      GS_PLUGIN_PACKAGEKIT_HISTORY_TIMEOUT,
					      cancellable,
					      &error_local);
	if (result == NULL) {
		if (g_error_matches (error_local,
				     G_DBUS_ERROR,
				     G_DBUS_ERROR_UNKNOWN_METHOD)) {
			g_debug ("No history available as PackageKit is too old: %s",
				 error_local->message);

			/* just set this to something non-zero so we don't keep
			 * trying to call GetPackageHistory */
			for (l = list; l != NULL; l = l->next) {
				app = GS_APP (l->data);
				gs_app_set_install_date (app, GS_APP_INSTALL_DATE_UNKNOWN);
			}
		} else if (g_error_matches (error_local,
					    G_IO_ERROR,
					    G_IO_ERROR_TIMED_OUT)) {
			g_debug ("No history as PackageKit took too long: %s",
				 error_local->message);
			for (l = list; l != NULL; l = l->next) {
				app = GS_APP (l->data);
				gs_app_set_install_date (app, GS_APP_INSTALL_DATE_UNKNOWN);
			}
		}
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to get history: %s",
			     error_local->message);
		return FALSE;
	}

	/* get any results */
	tuple = g_variant_get_child_value (result, 0);
	for (l = list; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		ret = g_variant_lookup (tuple,
					gs_app_get_source_default (app),
					"@aa{sv}",
					&value);
		if (!ret) {
			/* make up a fake entry as we know this package was at
			 * least installed at some point in time */
			if (gs_app_get_state (app) == AS_APP_STATE_INSTALLED) {
				g_autoptr(GsApp) app_dummy = NULL;
				app_dummy = gs_app_new (gs_app_get_id (app));
				gs_app_set_install_date (app_dummy, GS_APP_INSTALL_DATE_UNKNOWN);
				gs_app_set_kind (app_dummy, AS_APP_KIND_GENERIC);
				gs_app_set_state (app_dummy, AS_APP_STATE_INSTALLED);
				gs_app_set_version (app_dummy, gs_app_get_version (app));
				gs_app_add_history (app, app_dummy);
			}
			gs_app_set_install_date (app, GS_APP_INSTALL_DATE_UNKNOWN);
			continue;
		}

		/* add history for application */
		g_variant_iter_init (&iter, value);
		while ((value = g_variant_iter_next_value (&iter))) {
			gs_plugin_packagekit_refine_add_history (app, value);
			g_variant_unref (value);
		}
	}
	return TRUE;
}

/**
 * gs_plugin_refine:
 */
gboolean
gs_plugin_refine (GsPlugin *plugin,
		  GList **list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	gboolean ret;
	GList *l;
	GsApp *app;
	GPtrArray *sources;
	g_autoptr(GList) packages = NULL;

	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_HISTORY) == 0)
		return TRUE;

	/* add any missing history data */
	for (l = *list; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		sources = gs_app_get_sources (app);
		if (sources->len == 0)
			continue;
		if (gs_app_get_install_date (app) != 0)
			continue;
		packages = g_list_prepend (packages, app);
	}
	if (packages != NULL) {
		ret = gs_plugin_packagekit_refine (plugin,
						   packages,
						   cancellable,
						   error);
		if (!ret)
			return FALSE;
	}
	return TRUE;
}
