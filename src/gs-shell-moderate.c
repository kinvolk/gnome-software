/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
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

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>

#include "gs-app.h"
#include "gs-app-row.h"
#include "gs-review-row.h"
#include "gs-shell.h"
#include "gs-shell-moderate.h"
#include "gs-utils.h"

struct _GsShellModerate
{
	GsPage			 parent_instance;

	GsPluginLoader		*plugin_loader;
	GtkBuilder		*builder;
	GCancellable		*cancellable;
	GtkSizeGroup		*sizegroup_image;
	GtkSizeGroup		*sizegroup_name;
	GsShell			*shell;

	GtkWidget		*list_box_install;
	GtkWidget		*scrolledwindow_install;
	GtkWidget		*spinner_install;
	GtkWidget		*stack_install;
};

G_DEFINE_TYPE (GsShellModerate, gs_shell_moderate, GS_TYPE_PAGE)

/**
 * gs_shell_moderate_app_set_review_cb:
 **/
static void
gs_shell_moderate_app_set_review_cb (GObject *source,
				     GAsyncResult *res,
				     gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source);
	g_autoptr(GError) error = NULL;

	if (!gs_plugin_loader_app_action_finish (plugin_loader, res, &error)) {
		g_warning ("failed to set review: %s", error->message);
		return;
	}
}

static void
gs_shell_moderate_review_clicked_cb (GsReviewRow *row,
				     GsReviewAction action,
				     GsShellModerate *self)
{
	GsApp *app = g_object_get_data (G_OBJECT (row), "GsApp");
	gs_plugin_loader_review_action_async (self->plugin_loader,
					      app,
					      gs_review_row_get_review (row),
					      action,
					      self->cancellable,
					      gs_shell_moderate_app_set_review_cb,
					      self);
	gtk_widget_set_visible (GTK_WIDGET (row), FALSE);
}


static void
gs_shell_moderate_selection_changed_cb (GtkListBox *listbox,
					GsAppRow *app_row,
					GsShellModerate *self)
{
	g_autofree gchar *tmp = NULL;
	tmp = gs_app_to_string (gs_app_row_get_app (app_row));
	g_print ("%s", tmp);
}

static void
gs_shell_moderate_add_app (GsShellModerate *self, GsApp *app)
{
	GPtrArray *reviews;
	GtkWidget *app_row;
	guint i;

	/* this hides the action button */
	gs_app_add_quirk (app, AS_APP_QUIRK_COMPULSORY);

	/* add top level app */
	app_row = gs_app_row_new (app);
	gs_app_row_set_colorful (GS_APP_ROW (app_row), FALSE);
	gtk_container_add (GTK_CONTAINER (self->list_box_install), app_row);
	gs_app_row_set_size_groups (GS_APP_ROW (app_row),
				    self->sizegroup_image,
				    self->sizegroup_name);

	/* add reviews */
	reviews = gs_app_get_reviews (app);
	for (i = 0; i < reviews->len; i++) {
		GsReview *review = g_ptr_array_index (reviews, i);
		GtkWidget *row = gs_review_row_new (review);
		gtk_widget_set_margin_start (row, 250);
		gtk_widget_set_margin_end (row, 250);
		gs_review_row_set_actions (GS_REVIEW_ROW (row),
					   1 << GS_REVIEW_ACTION_UPVOTE |
					   1 << GS_REVIEW_ACTION_DOWNVOTE |
					   1 << GS_REVIEW_ACTION_DISMISS |
					   1 << GS_REVIEW_ACTION_REPORT);
		g_signal_connect (row, "button-clicked",
				  G_CALLBACK (gs_shell_moderate_review_clicked_cb), self);
		g_object_set_data_full (G_OBJECT (row), "GsApp",
					g_object_ref (app),
					(GDestroyNotify) g_object_unref);
		gtk_container_add (GTK_CONTAINER (self->list_box_install), row);
	}
	gtk_widget_show (app_row);
}

/**
 * gs_shell_moderate_get_unvoted_reviews_cb:
 **/
static void
gs_shell_moderate_get_unvoted_reviews_cb (GObject *source_object,
					  GAsyncResult *res,
					  gpointer user_data)
{
	GList *l;
	GsApp *app;
	GsShellModerate *self = GS_SHELL_MODERATE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	gs_stop_spinner (GTK_SPINNER (self->spinner_install));
	gtk_stack_set_visible_child_name (GTK_STACK (self->stack_install), "view");

	list = gs_plugin_loader_get_unvoted_reviews_finish (plugin_loader,
							    res,
							    &error);
	if (list == NULL) {
		if (g_error_matches (error,
				     GS_PLUGIN_LOADER_ERROR,
				     GS_PLUGIN_LOADER_ERROR_NO_RESULTS)) {
			gtk_stack_set_visible_child_name (GTK_STACK (self->stack_install),
							  "uptodate");
			return;
		}
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("failed to get moderate apps: %s", error->message);
		return;
	}
	for (l = list; l != NULL; l = l->next) {
		app = GS_APP (l->data);
		gs_shell_moderate_add_app (self, app);
	}
}

/**
 * gs_shell_moderate_load:
 */
static void
gs_shell_moderate_load (GsShellModerate *self)
{
	/* remove old entries */
	gs_container_remove_all (GTK_CONTAINER (self->list_box_install));

	/* get unvoted reviews as apps */
	gs_plugin_loader_get_unvoted_reviews_async (self->plugin_loader,
						    GS_PLUGIN_REFINE_FLAGS_DEFAULT |
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION |
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION |
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE |
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_DESCRIPTION |
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENSE |
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS,
						    self->cancellable,
						    gs_shell_moderate_get_unvoted_reviews_cb,
						    self);
	gs_start_spinner (GTK_SPINNER (self->spinner_install));
	gtk_stack_set_visible_child_name (GTK_STACK (self->stack_install), "spinner");
}

/**
 * gs_shell_moderate_reload:
 */
void
gs_shell_moderate_reload (GsShellModerate *self)
{
	gs_shell_moderate_load (self);
}

/**
 * gs_shell_moderate_switch_to:
 **/
static void
gs_shell_moderate_switch_to (GsPage *page, gboolean scroll_up)
{
	GsShellModerate *self = GS_SHELL_MODERATE (page);

	if (gs_shell_get_mode (self->shell) != GS_SHELL_MODE_MODERATE) {
		g_warning ("Called switch_to(moderate) when in mode %s",
			   gs_shell_get_mode_string (self->shell));
		return;
	}
	if (gs_shell_get_mode (self->shell) == GS_SHELL_MODE_MODERATE)
		gs_grab_focus_when_mapped (self->scrolledwindow_install);
	gs_shell_moderate_load (self);
}

/**
 * gs_shell_moderate_list_header_func
 **/
static void
gs_shell_moderate_list_header_func (GtkListBoxRow *row,
				     GtkListBoxRow *before,
				     gpointer user_data)
{
	GtkWidget *header;
	gtk_list_box_row_set_header (row, NULL);
	if (before == NULL)
		return;
	if (GS_IS_REVIEW_ROW (before) && GS_IS_APP_ROW (row)) {
		header = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
		gtk_list_box_row_set_header (row, header);
	}
}

/**
 * gs_shell_moderate_setup:
 */
void
gs_shell_moderate_setup (GsShellModerate *self,
			  GsShell *shell,
			  GsPluginLoader *plugin_loader,
			  GtkBuilder *builder,
			  GCancellable *cancellable)
{
	g_return_if_fail (GS_IS_SHELL_MODERATE (self));

	self->shell = shell;
	self->plugin_loader = g_object_ref (plugin_loader);
	self->builder = g_object_ref (builder);
	self->cancellable = g_object_ref (cancellable);

	gtk_list_box_set_header_func (GTK_LIST_BOX (self->list_box_install),
				      gs_shell_moderate_list_header_func,
				      self, NULL);

	/* chain up */
	gs_page_setup (GS_PAGE (self), shell, plugin_loader, cancellable);
}

/**
 * gs_shell_moderate_dispose:
 **/
static void
gs_shell_moderate_dispose (GObject *object)
{
	GsShellModerate *self = GS_SHELL_MODERATE (object);

	g_clear_object (&self->sizegroup_image);
	g_clear_object (&self->sizegroup_name);

	g_clear_object (&self->builder);
	g_clear_object (&self->plugin_loader);
	g_clear_object (&self->cancellable);

	G_OBJECT_CLASS (gs_shell_moderate_parent_class)->dispose (object);
}

/**
 * gs_shell_moderate_class_init:
 **/
static void
gs_shell_moderate_class_init (GsShellModerateClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_shell_moderate_dispose;
	page_class->switch_to = gs_shell_moderate_switch_to;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-shell-moderate.ui");

	gtk_widget_class_bind_template_child (widget_class, GsShellModerate, list_box_install);
	gtk_widget_class_bind_template_child (widget_class, GsShellModerate, scrolledwindow_install);
	gtk_widget_class_bind_template_child (widget_class, GsShellModerate, spinner_install);
	gtk_widget_class_bind_template_child (widget_class, GsShellModerate, stack_install);
}

/**
 * gs_shell_moderate_init:
 **/
static void
gs_shell_moderate_init (GsShellModerate *self)
{
	gtk_widget_init_template (GTK_WIDGET (self));

	g_signal_connect (self->list_box_install, "row-activated",
			  G_CALLBACK (gs_shell_moderate_selection_changed_cb), self);

	self->sizegroup_image = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
	self->sizegroup_name = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
}

/**
 * gs_shell_moderate_new:
 **/
GsShellModerate *
gs_shell_moderate_new (void)
{
	GsShellModerate *self;
	self = g_object_new (GS_TYPE_SHELL_MODERATE, NULL);
	return GS_SHELL_MODERATE (self);
}

/* vim: set noexpandtab: */
