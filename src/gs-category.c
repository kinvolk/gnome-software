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

#include <glib/gi18n.h>

#include "gs-category.h"

struct _GsCategory
{
	GObject		 parent_instance;

	gchar		*id;
	gchar		*name;
	GsCategory	*parent;
	guint		 size;
	GList		*subcategories;
};

G_DEFINE_TYPE (GsCategory, gs_category, G_TYPE_OBJECT)

/**
 * gs_category_get_size:
 *
 * Returns how many applications the category could contain.
 *
 * NOTE: This may over-estimate the number if duplicate applications are
 * filtered or core applications are not shown.
 **/
guint
gs_category_get_size (GsCategory *category)
{
	g_return_val_if_fail (GS_IS_CATEGORY (category), 0);
	return category->size;
}

/**
 * gs_category_set_size:
 **/
void
gs_category_set_size (GsCategory *category, guint size)
{
	g_return_if_fail (GS_IS_CATEGORY (category));
	category->size = size;
}

/**
 * gs_category_increment_size:
 *
 * Adds one to the size count if an application is available
 **/
void
gs_category_increment_size (GsCategory *category)
{
	g_return_if_fail (GS_IS_CATEGORY (category));
	category->size++;
}

const gchar *
gs_category_get_id (GsCategory *category)
{
	g_return_val_if_fail (GS_IS_CATEGORY (category), NULL);
	return category->id;
}

const gchar *
gs_category_get_name (GsCategory *category)
{
	g_return_val_if_fail (GS_IS_CATEGORY (category), NULL);
	return category->name;
}

GsCategory *
gs_category_find_child (GsCategory *category, const gchar *id)
{
	GList *l;
	GsCategory *tmp;

	/* find the subcategory */
	if (category->subcategories == NULL)
		return NULL;
	for (l = category->subcategories; l != NULL; l = l->next) {
		tmp = GS_CATEGORY (l->data);
		if (g_strcmp0 (id, gs_category_get_id (tmp)) == 0)
			return tmp;
	}
	return NULL;
}

GsCategory *
gs_category_get_parent (GsCategory *category)
{
	g_return_val_if_fail (GS_IS_CATEGORY (category), NULL);
	return category->parent;
}

/**
 * gs_category_get_subcategories:
 *
 * Return value: (element-type GsApp) (transfer container): A list of subcategories
 **/
GList *
gs_category_get_subcategories (GsCategory *category)
{
	g_return_val_if_fail (GS_IS_CATEGORY (category), NULL);
	return g_list_copy (category->subcategories);
}

/**
 * gs_category_add_subcategory:
 **/
void
gs_category_add_subcategory (GsCategory *category, GsCategory *subcategory)
{
	g_return_if_fail (GS_IS_CATEGORY (category));
	category->subcategories = g_list_prepend (category->subcategories,
							g_object_ref (subcategory));
}

/**
 * gs_category_get_sort_key:
 **/
static gchar *
gs_category_get_sort_key (GsCategory *category)
{
	guint sort_order = 5;
	if (g_strcmp0 (gs_category_get_id (category), "featured") == 0)
		sort_order = 0;
	else if (g_strcmp0 (gs_category_get_id (category), "all") == 0)
		sort_order = 2;
	else if (g_strcmp0 (gs_category_get_id (category), "other") == 0)
		sort_order = 9;
	return g_strdup_printf ("%i:%s",
				sort_order,
				gs_category_get_name (category));
}

/**
 * gs_category_sort_subcategories_cb:
 **/
static gint
gs_category_sort_subcategories_cb (gconstpointer a, gconstpointer b)
{
	GsCategory *ca = GS_CATEGORY ((gpointer) a);
	GsCategory *cb = GS_CATEGORY ((gpointer) b);
	g_autofree gchar *id_a = gs_category_get_sort_key (ca);
	g_autofree gchar *id_b = gs_category_get_sort_key (cb);
	return g_strcmp0 (id_a, id_b);
}

/**
 * gs_category_sort_subcategories:
 **/
void
gs_category_sort_subcategories (GsCategory *category)
{
	/* nothing here */
	if (category->subcategories == NULL)
		return;

	/* actually sort the data */
	category->subcategories = g_list_sort (category->subcategories,
					   gs_category_sort_subcategories_cb);
}

static void
gs_category_dispose (GObject *object)
{
	GsCategory *category = GS_CATEGORY (object);

	if (category->subcategories != NULL) {
		g_list_free_full (category->subcategories, g_object_unref);
		category->subcategories = NULL;
	}

	G_OBJECT_CLASS (gs_category_parent_class)->dispose (object);
}

static void
gs_category_finalize (GObject *object)
{
	GsCategory *category = GS_CATEGORY (object);

	if (category->parent != NULL)
		g_object_remove_weak_pointer (G_OBJECT (category->parent),
		                              (gpointer *) &category->parent);
	g_free (category->id);
	g_free (category->name);

	G_OBJECT_CLASS (gs_category_parent_class)->finalize (object);
}

static void
gs_category_class_init (GsCategoryClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->dispose = gs_category_dispose;
	object_class->finalize = gs_category_finalize;
}

static void
gs_category_init (GsCategory *category)
{
}

GsCategory *
gs_category_new (GsCategory *parent, const gchar *id, const gchar *name)
{
	GsCategory *category;

	/* special case, we don't want translations in the plugins */
	if (g_strcmp0 (id, "other") == 0) {
		/* TRANSLATORS: this is where all applications that don't
		 * fit in other groups are put */
		name =_("Other");
	} else if (g_strcmp0 (id, "all") == 0) {
		/* TRANSLATORS: this is a subcategory matching all the
		 * different apps in the parent category, e.g. "Games" */
		name =_("All");
	} else if (g_strcmp0 (id, "featured") == 0) {
		/* TRANSLATORS: this is a subcategory of featured apps */
		name =_("Featured");
	}

	category = g_object_new (GS_TYPE_CATEGORY, NULL);
	category->parent = parent;
	if (category->parent != NULL)
		g_object_add_weak_pointer (G_OBJECT (category->parent),
		                           (gpointer *) &category->parent);
	category->id = g_strdup (id);
	category->name = g_strdup (name);
	return GS_CATEGORY (category);
}

/* vim: set noexpandtab: */
