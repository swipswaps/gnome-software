/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
 * Copyright (C) 2014-2018 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>

#include "gs-app-list-private.h"
#include "gs-common.h"
#include "gs-summary-tile.h"
#include "gs-popular-tile.h"
#include "gs-background-tile.h"
#include "gs-category-page.h"

typedef enum {
	SUBCATEGORY_SORT_TYPE_RATING,
	SUBCATEGORY_SORT_TYPE_NAME
} SubcategorySortType;

#define MAX_PLACEHOLDER_TILES 30

struct _GsCategoryPage
{
	GsPage		 parent_instance;

	GsPluginLoader	*plugin_loader;
	GtkBuilder	*builder;
	GCancellable	*cancellable;
	GsShell		*shell;
	GPtrArray	*copy_dests;  /* (element-type GFile) (owned) */
	gboolean	 copying;
	GsCategory	*category;
	GsCategory	*subcategory;
	guint		sort_rating_handler_id;
	guint		sort_name_handler_id;
	SubcategorySortType sort_type;
	GHashTable	*category_apps; /* (element-type GsCategory GsAppList) */
	gint		 num_placeholders_to_show;

	GtkWidget	*infobar_category_shell_extensions;
	GtkWidget	*button_category_shell_extensions;
	GtkWidget	*category_detail_box;
	GtkWidget	*scrolledwindow_category;
	GtkWidget	*subcats_filter_label;
	GtkWidget	*subcats_filter_button_label;
	GtkWidget	*subcats_filter_button;
	GtkWidget	*popover_filter_box;
	GtkWidget	*subcats_sort_label;
	GtkWidget	*subcats_sort_button;
	GtkWidget	*subcats_sort_button_label;
	GtkWidget	*sort_rating_button;
	GtkWidget	*sort_name_button;
	GtkWidget	*featured_grid;
	GtkWidget	*featured_heading;
	GtkWidget	*header_filter_box;
	GtkWidget	*no_apps_box;
	GtkWidget	*usb_action_box;
	GtkWidget	*copy_os_to_usb_button;
	GtkWidget	*cancel_os_copy_button;
	GtkWidget	*os_copy_spinner;
};

G_DEFINE_TYPE (GsCategoryPage, gs_category_page, GS_TYPE_PAGE)

static void
gs_category_page_switch_to (GsPage *page, gboolean scroll_up)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	GtkWidget *widget;

	widget = GTK_WIDGET (gtk_builder_get_object (self->builder, "buttonbox_main"));
	gtk_widget_show (widget);
}

static void
app_tile_clicked (GsAppTile *tile, gpointer data)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (data);
	GsApp *app;

	app = gs_app_tile_get_app (tile);
	gs_shell_show_app (self->shell, app);
}

static gboolean
gs_category_page_has_app (GsCategoryPage *self,
			  GsApp *app)
{
	GHashTableIter iter;
	gpointer value;
	const gchar *id = gs_app_get_unique_id (app);

	g_hash_table_iter_init (&iter, self->category_apps);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		GsAppList *list = value;
		if (gs_app_list_lookup (list, id) != NULL)
			return TRUE;
	}
	return FALSE;
}

static void
gs_category_page_sort_by_type (GsCategoryPage *self,
			       SubcategorySortType sort_type)
{
	g_autofree gchar *button_label;

	if (sort_type == SUBCATEGORY_SORT_TYPE_NAME)
		g_object_get (self->sort_name_button, "text", &button_label, NULL);
	else
		g_object_get (self->sort_rating_button, "text", &button_label, NULL);

	gtk_label_set_text (GTK_LABEL (self->subcats_sort_button_label), button_label);

	/* only sort again if the sort type is different */
	if (self->sort_type == sort_type)
		return;

	self->sort_type = sort_type;
	gtk_flow_box_invalidate_sort (GTK_FLOW_BOX (self->category_detail_box));
}

static void
sort_button_clicked (GtkButton *button, gpointer data)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (data);

	if (button == GTK_BUTTON (self->sort_rating_button))
		gs_category_page_sort_by_type (self, SUBCATEGORY_SORT_TYPE_RATING);
	else
		gs_category_page_sort_by_type (self, SUBCATEGORY_SORT_TYPE_NAME);
}

static void
gs_category_page_remove_apps_for_category (GsCategoryPage *self,
					   GsCategory *category,
					   GsAppList *apps_to_remove)
{
	GsAppList *list = g_hash_table_lookup (self->category_apps, category);
	if (list == NULL)
		return;

	for (guint i = 0; i < gs_app_list_length (apps_to_remove); ++i) {
		GsApp *app = gs_app_list_index (apps_to_remove, i);
		gs_app_list_remove (list, app);
	}
}

static void
gs_category_page_get_apps_cb (GObject *source_object,
                              GAsyncResult *res,
                              gpointer user_data)
{
	GtkWidget *tile;
	GsCategoryPage *self = GS_CATEGORY_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;
	GsAppList *category_app_list = NULL;
	g_autoptr(GsAppList) new_app_list = NULL;

	list = gs_plugin_loader_job_process_finish (plugin_loader,
						    res,
						    &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get apps for category apps: %s", error->message);
		return;
	}

	self->sort_rating_handler_id = g_signal_connect (self->sort_rating_button,
							 "clicked",
							 G_CALLBACK (sort_button_clicked),
							 self);
	self->sort_name_handler_id = g_signal_connect (self->sort_name_button,
						       "clicked",
						       G_CALLBACK (sort_button_clicked),
						       self);

	category_app_list = g_hash_table_lookup (self->category_apps, self->subcategory);
	if (category_app_list == NULL) {
		category_app_list = gs_app_list_new ();
		g_hash_table_insert (self->category_apps, self->subcategory,
				     category_app_list);
	}

	/* gather any new apps that may not be already in the category view */
	new_app_list = gs_app_list_new ();
	for (guint i = 0; i < gs_app_list_length (list); ++i) {
		GsApp *app = gs_app_list_index (list, i);
		if (gs_app_list_lookup (category_app_list, gs_app_get_unique_id (app)) != NULL)
			continue;

		gs_app_list_add (new_app_list, app);
	}

	/* add the new apps to the category */
	for (guint i = 0; i < gs_app_list_length (new_app_list); i++) {
		GsApp *app = gs_app_list_index (new_app_list, i);
		/* new tiles are only created if they don't exist yet
		 * (they may have been added from another category since an
		 * app may be in several categories) */
		if (!gs_category_page_has_app (self, app)) {
			tile = gs_background_tile_new (app);
			g_signal_connect (tile, "clicked",
					  G_CALLBACK (app_tile_clicked), self);
			gtk_container_add (GTK_CONTAINER (self->category_detail_box), tile);
			gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
		}
		gs_app_list_add (category_app_list, app);
	}

	/* if an app is no longer part of a category, remove it from there */
	if (gs_app_list_length (category_app_list) != gs_app_list_length (list)) {
		g_autoptr(GsAppList) apps_to_remove = gs_app_list_new ();
		for (guint i = 0; i < gs_app_list_length (category_app_list); ++i) {
			GsApp *app = gs_app_list_index (category_app_list, i);
			if (gs_app_list_lookup (list, gs_app_get_unique_id (app)) == NULL) {
				g_debug ("App %s is no longer in category %s::%s",
					 gs_app_get_unique_id (app),
					 gs_category_get_id (self->category),
					 gs_category_get_id (self->subcategory));
				gs_app_list_add (apps_to_remove, app);
			}
		}
		gs_category_page_remove_apps_for_category (self,
							   self->subcategory,
							   apps_to_remove);
	}

	/* ensure the filter will show the apps, not the placeholders */
	self->num_placeholders_to_show = -1;
	gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (self->category_detail_box));

	if (g_strcmp0 (gs_category_get_id (self->category), "usb") == 0) {
		gtk_widget_set_visible (self->no_apps_box, gs_category_get_size (self->subcategory) == 0);
		gtk_widget_set_visible (self->scrolledwindow_category, gs_category_get_size (self->subcategory) != 0);
	}
}

static gboolean
_max_results_sort_cb (GsApp *app1, GsApp *app2, gpointer user_data)
{
	return gs_app_get_rating (app1) < gs_app_get_rating (app2);
}

static gint
gs_category_page_sort_flow_box_sort_func (GtkFlowBoxChild *child1,
					  GtkFlowBoxChild *child2,
					  gpointer data)
{
	GsApp *app1 = gs_app_tile_get_app (GS_APP_TILE (gtk_bin_get_child (GTK_BIN (child1))));
	GsApp *app2 = gs_app_tile_get_app (GS_APP_TILE (gtk_bin_get_child (GTK_BIN (child2))));
	SubcategorySortType sort_type;
	g_autofree gchar *casefolded_name1 = NULL;
	g_autofree gchar *casefolded_name2 = NULL;

	if (!GS_IS_APP (app1) || !GS_IS_APP (app2))
		return 0;

	sort_type = GS_CATEGORY_PAGE (data)->sort_type;

	if (sort_type == SUBCATEGORY_SORT_TYPE_RATING) {
		gint rating_app1 = gs_app_get_rating (app1);
		gint rating_app2 = gs_app_get_rating (app2);
		if (rating_app1 > rating_app2)
			return -1;
		if (rating_app1 < rating_app2)
			return 1;
	}

	if (gs_app_get_name (app1) != NULL)
		casefolded_name1 = g_utf8_casefold (gs_app_get_name (app1), -1);
	if (gs_app_get_name (app2) != NULL)
		casefolded_name2 = g_utf8_casefold (gs_app_get_name (app2), -1);
	return g_strcmp0 (casefolded_name1, casefolded_name2);
}

static void
gs_category_page_set_featured_placeholders (GsCategoryPage *self)
{
	gs_container_remove_all (GTK_CONTAINER (self->featured_grid));
	for (guint i = 0; i < 3; ++i) {
		GtkWidget *tile = gs_summary_tile_new (NULL);
		g_signal_connect (tile, "clicked",
				  G_CALLBACK (app_tile_clicked), self);
		gtk_grid_attach (GTK_GRID (self->featured_grid), tile, i, 0, 1, 1);
		gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
	}
	gtk_widget_show (self->featured_grid);
}

static void
gs_category_page_get_featured_apps_cb (GObject *source_object,
				       GAsyncResult *res,
				       gpointer user_data)
{
	GsApp *app;
	GtkWidget *tile;
	GsCategoryPage *self = GS_CATEGORY_PAGE (user_data);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	gs_container_remove_all (GTK_CONTAINER (self->featured_grid));
	gtk_widget_hide (self->featured_grid);
	gtk_widget_hide (self->featured_heading);

	list = gs_plugin_loader_job_process_finish (plugin_loader,
						    res,
						    &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get featured apps for category apps: %s", error->message);
		return;
	}
	if (gs_app_list_length (list) < 3) {
		g_debug ("not enough featured apps for category %s; not showing featured apps!",
			 gs_category_get_id (self->category));
		return;
	}

	/* randomize so we show different featured apps every time */
	gs_app_list_randomize (list);

	for (guint i = 0; i < 3; ++i) {
		app = gs_app_list_index (list, i);
		tile = gs_summary_tile_new (app);
		g_signal_connect (tile, "clicked",
				  G_CALLBACK (app_tile_clicked), self);
		gtk_grid_attach (GTK_GRID (self->featured_grid), tile, i, 0, 1, 1);
		gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
	}

	gtk_widget_show (self->featured_grid);
	gtk_widget_show (self->featured_heading);
}

static void
gs_category_page_set_featured_apps (GsCategoryPage *self)
{
	GsCategory *featured_subcat = NULL;
	GPtrArray *children = gs_category_get_children (self->category);
	g_autoptr(GsPluginJob) plugin_job = NULL;

	for (guint i = 0; i < children->len; ++i) {
		GsCategory *sub = GS_CATEGORY (g_ptr_array_index (children, i));
		if (g_strcmp0 (gs_category_get_id (sub), "featured") == 0) {
			featured_subcat = sub;
			break;
		}
	}

	if (featured_subcat == NULL)
		return;

	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_CATEGORY_APPS,
					 "interactive", TRUE,
					 "category", featured_subcat,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING,
					 "dedupe-flags", GS_APP_LIST_FILTER_FLAG_PREFER_INSTALLED |
							 GS_APP_LIST_FILTER_FLAG_KEY_ID_PROVIDES,
					 NULL);
	gs_plugin_loader_job_process_async (self->plugin_loader,
					    plugin_job,
					    self->cancellable,
					    gs_category_page_get_featured_apps_cb,
					    self);
}

static void
set_os_copying_state (GsCategoryPage *self,
                      gboolean        copying)
{
	gtk_widget_set_visible (self->copy_os_to_usb_button, !copying);
	gtk_widget_set_visible (self->os_copy_spinner, copying);
	gtk_widget_set_visible (self->cancel_os_copy_button, copying);

	if (copying)
		gs_start_spinner (GTK_SPINNER (self->os_copy_spinner));
	else
		gs_stop_spinner (GTK_SPINNER (self->os_copy_spinner));

	self->copying = copying;
}

static void
gs_category_page_os_copied (GsPage *page,
			    const GError *error)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	set_os_copying_state (self, FALSE);
}

static void
cancel_os_copy_button_cb (GtkButton *button, GsCategoryPage *self)
{
	g_cancellable_cancel (self->cancellable);
	set_os_copying_state (self, FALSE);
}

static void
copy_os_to_usb_button_cb (GtkButton *button, GsCategoryPage *self)
{
	g_autoptr(GPtrArray) copy_dests = gs_plugin_loader_dup_copy_dests (self->plugin_loader);
	g_return_if_fail (copy_dests->len > 0);

	set_os_copying_state (self, TRUE);
	gs_page_copy_os (GS_PAGE (self), copy_dests->pdata[0], GS_SHELL_INTERACTION_FULL,
			 self->cancellable);
}

static void
gs_category_page_os_get_copyable_cb (GObject *source_object,
				     GAsyncResult *res,
				     gpointer user_data)
{
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	GsCategoryPage *self = GS_CATEGORY_PAGE (user_data);
	g_autoptr(GError) error = NULL;
	gboolean copyable;

	copyable = gs_plugin_loader_job_os_get_copyable_finish (plugin_loader,
								res, &error);
	gtk_button_set_label (GTK_BUTTON (self->copy_os_to_usb_button),
			      _("Copy OS to USB"));
	gtk_widget_set_sensitive (self->copy_os_to_usb_button, copyable);
	gtk_widget_set_visible (self->copy_os_to_usb_button, copyable);
	gtk_widget_set_visible (self->usb_action_box, copyable);
}

static void
gs_category_page_update_os_copy_button (GsCategoryPage *self)
{
	g_autoptr(GsPluginJob) plugin_job = NULL;

	gtk_widget_set_visible (self->usb_action_box, FALSE);
	gtk_widget_set_sensitive (self->copy_os_to_usb_button, FALSE);
	gtk_widget_set_visible (self->copy_os_to_usb_button, FALSE);

	if (self->category == NULL ||
	    g_strcmp0 (gs_category_get_id (self->category), "usb") != 0) {
		return;
	}

	if (self->copying)
		return;

	if (self->copy_dests != NULL && self->copy_dests->len > 0) {
		/* check (asynchronously) whether the OS will certainly fail a
		 * copy so we can update the UI accordingly. Cases where the
		 * plugin would know a copy would fail include an OSTree remote
		 * which lacks a collection ID or broader issues like the user
		 * lacking write access to the destination. */
		plugin_job = gs_plugin_job_newv (
			GS_PLUGIN_ACTION_OS_GET_COPYABLE,
			"copy-dest", self->copy_dests->pdata[0],
			 NULL);
		gs_plugin_loader_job_os_get_copyable_async (
			self->plugin_loader,
			plugin_job,
			self->cancellable,
			gs_category_page_os_get_copyable_cb,
			self);
	}
}

static void
gs_category_page_copy_dests_notify_cb (GsPluginLoader *plugin_loader,
				       GParamSpec *pspec,
				       GsCategoryPage *self)
{
	g_clear_pointer (&self->copy_dests, g_ptr_array_unref);
	self->copy_dests = gs_plugin_loader_dup_copy_dests (plugin_loader);
	gs_category_page_update_os_copy_button (self);
}

static void
gs_category_page_reload (GsPage *page)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	g_autoptr(GsPluginJob) plugin_job = NULL;

	if (self->subcategory == NULL)
		return;

	g_cancellable_cancel (self->cancellable);
	g_clear_object (&self->cancellable);
	self->cancellable = g_cancellable_new ();

	g_debug ("search using %s/%s",
	         gs_category_get_id (self->category),
	         gs_category_get_id (self->subcategory));

	/* don't show the sort button on addons that cannot be rated */
	if (g_strcmp0 (gs_category_get_id (self->category), "addons") == 0) {
		gtk_widget_set_visible (self->subcats_sort_label, FALSE);
		gtk_widget_set_visible (self->subcats_sort_button, FALSE);

	} else {
		gtk_widget_set_visible (self->subcats_sort_label, TRUE);
		gtk_widget_set_visible (self->subcats_sort_button, TRUE);
	}

	/* show the shell extensions header */
	if (g_strcmp0 (gs_category_get_id (self->subcategory), "shell-extensions") == 0)
		gtk_widget_set_visible (self->infobar_category_shell_extensions, TRUE);
	else
		gtk_widget_set_visible (self->infobar_category_shell_extensions, FALSE);

	if (self->sort_rating_handler_id > 0) {
		g_signal_handler_disconnect (self->sort_rating_button,
					     self->sort_rating_handler_id);
		self->sort_rating_handler_id = 0;
	}

	if (self->sort_name_handler_id > 0) {
		g_signal_handler_disconnect (self->sort_name_button,
					     self->sort_name_handler_id);
		self->sort_name_handler_id = 0;
	}

	/* just ensure the sort button has the correct label */
	gs_category_page_sort_by_type (self, self->sort_type);

	/* ensure the placeholders are shown */
	self->num_placeholders_to_show = MIN (MAX_PLACEHOLDER_TILES,
					      (gint) gs_category_get_size (self->subcategory));
	gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (self->category_detail_box));

	gtk_widget_set_visible (self->cancel_os_copy_button, FALSE);
	gs_category_page_copy_dests_notify_cb (self->plugin_loader, NULL, self);

	gs_category_page_set_featured_apps (self);

	plugin_job = gs_plugin_job_newv (GS_PLUGIN_ACTION_GET_CATEGORY_APPS,
					 "category", self->subcategory,
					 "filter-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING,
					 "refine-flags", GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_ORIGIN_HOSTNAME |
							 GS_PLUGIN_REFINE_FLAGS_REQUIRE_KEY_COLORS,
					 "dedupe-flags", GS_APP_LIST_FILTER_FLAG_PREFER_INSTALLED |
							 GS_APP_LIST_FILTER_FLAG_KEY_ID_PROVIDES,
					 NULL);
	gs_plugin_job_set_sort_func (plugin_job, _max_results_sort_cb);
	gs_plugin_loader_job_process_async (self->plugin_loader,
					    plugin_job,
					    self->cancellable,
					    gs_category_page_get_apps_cb,
					    self);
}

static void
gs_category_page_populate_filtered (GsCategoryPage *self, GsCategory *subcategory)
{
	g_assert (subcategory != NULL);
	g_set_object (&self->subcategory, subcategory);
	gs_category_page_reload (GS_PAGE (self));
}

static void
filter_button_activated (GtkWidget *button,  gpointer data)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (data);
	GsCategory *category;

	category = g_object_get_data (G_OBJECT (button), "category");

	gtk_label_set_text (GTK_LABEL (self->subcats_filter_button_label),
			    gs_category_get_name (category));
	gs_category_page_populate_filtered (self, category);
}

static gboolean
gs_category_page_should_use_header_filter (GsCategory *category)
{
	return g_strcmp0 (gs_category_get_id (category), "addons") == 0;
}

static void
gs_category_page_create_filter (GsCategoryPage *self,
				GsCategory *category)
{
	GtkWidget *button = NULL;
	GsCategory *s;
	guint i;
	GPtrArray *children;
	GtkWidget *first_subcat = NULL;
	gboolean featured_category_found = FALSE;
	gboolean use_header_filter = gs_category_page_should_use_header_filter (category);

	gs_container_remove_all (GTK_CONTAINER (self->header_filter_box));
	gs_container_remove_all (GTK_CONTAINER (self->popover_filter_box));

	children = gs_category_get_children (category);
	for (i = 0; i < children->len; i++) {
		s = GS_CATEGORY (g_ptr_array_index (children, i));
		/* don't include the featured subcategory (those will appear as banners) */
		if (g_strcmp0 (gs_category_get_id (s), "featured") == 0) {
			featured_category_found = TRUE;
			continue;
		}
		if (gs_category_get_size (s) < 1) {
			gboolean category_is_usb = FALSE;

			g_debug ("not showing %s/%s as no apps",
				 gs_category_get_id (category),
				 gs_category_get_id (s));

			/* re-filter USB category with no apps so the
			 * placeholder app tiles get cleared out, then set
			 * "empty state" message for an empty USB disk
			 */
			if (g_strcmp0 (gs_category_get_id (category), "usb") == 0) {
				gs_category_page_populate_filtered (self, s);
				category_is_usb = TRUE;
			}

			gtk_widget_set_visible (self->no_apps_box, category_is_usb);
			gtk_widget_set_visible (self->scrolledwindow_category, !category_is_usb);

			continue;
		}

		/* create the right button type depending on where it will be used */
		if (use_header_filter) {
			if (button == NULL)
				button = gtk_radio_button_new (NULL);
			else
				button = gtk_radio_button_new_from_widget (GTK_RADIO_BUTTON (button));
			g_object_set (button, "xalign", 0.5, "label", gs_category_get_name (s),
				      "draw-indicator", FALSE, "relief", GTK_RELIEF_NONE, NULL);
			gtk_container_add (GTK_CONTAINER (self->header_filter_box), button);
		} else {
			button = gtk_model_button_new ();
			g_object_set (button, "xalign", 0.0, "text", gs_category_get_name (s), NULL);
			gtk_container_add (GTK_CONTAINER (self->popover_filter_box), button);
		}

		g_object_set_data_full (G_OBJECT (button), "category", g_object_ref (s), g_object_unref);
		gtk_widget_show (button);
		g_signal_connect (button, "clicked", G_CALLBACK (filter_button_activated), self);

		/* make sure the first subcategory gets selected */
		if (first_subcat == NULL)
		        first_subcat = button;
	}
	if (first_subcat != NULL)
		filter_button_activated (first_subcat, self);

	/* show only the adequate filter */
	gtk_widget_set_visible (self->subcats_filter_label, !use_header_filter);
	gtk_widget_set_visible (self->subcats_filter_button, !use_header_filter);
	gtk_widget_set_visible (self->header_filter_box, use_header_filter);

	if (featured_category_found) {
		g_autofree gchar *featured_heading = NULL;

		/* set up the placeholders as having the featured category is a good
		 * indicator that there will be featured apps */
		gs_category_page_set_featured_placeholders (self);

		/* TRANSLATORS: This is a heading on the categories page. %s gets
		   replaced by the category name, e.g. 'Graphics & Photography' */
		featured_heading = g_strdup_printf (_("Featured %s"), gs_category_get_name (self->category));
		gtk_label_set_label (GTK_LABEL (self->featured_heading), featured_heading);
		gtk_widget_show (self->featured_heading);
	} else {
		gs_container_remove_all (GTK_CONTAINER (self->featured_grid));
		gtk_widget_hide (self->featured_grid);
		gtk_widget_hide (self->featured_heading);
	}
}

void
gs_category_page_set_category (GsCategoryPage *self, GsCategory *category)
{
	GtkAdjustment *adj = NULL;

	/* this means we've come from the app-view -> back */
	if (self->category == category)
		return;

	/* save this */
	g_clear_object (&self->category);
	self->category = g_object_ref (category);

	/* find apps in this group */
	gs_category_page_create_filter (self, category);

	/* scroll the list of apps to the beginning, otherwise it will show
	 * with the previous scroll value */
	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_category));
	gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj));
}

GsCategory *
gs_category_page_get_category (GsCategoryPage *self)
{
	return self->category;
}

static gboolean
gs_category_page_filter_apps_func (GtkFlowBoxChild *child,
				   gpointer user_data)
{
	GsCategoryPage *self = user_data;
	GsApp *app = gs_app_tile_get_app (GS_APP_TILE (gtk_bin_get_child (GTK_BIN (child))));
	GsAppList *category_apps = NULL;

	if (self->subcategory == NULL)
		return TRUE;

	/* let's show as many placeholders as desired */
	if (self->num_placeholders_to_show > -1) {
		/* don't show normal app tiles if we're supposed to show placeholders*/
		if (app != NULL)
			return FALSE;

		/* we cannot drop below 0 here, otherwise it will start showing
		 * the regular app tiles */
		if (self->num_placeholders_to_show > 0)
			--self->num_placeholders_to_show;
		return TRUE;
	}

	if (app == NULL)
		return FALSE;

	category_apps = g_hash_table_lookup (self->category_apps, self->subcategory);
	return category_apps != NULL &&
		gs_app_list_lookup (category_apps, gs_app_get_unique_id (app));
}

static void
gs_category_page_init (GsCategoryPage *self)
{
	gtk_widget_init_template (GTK_WIDGET (self));
}

static void
gs_category_page_dispose (GObject *object)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (object);

	g_cancellable_cancel (self->cancellable);
	g_clear_object (&self->cancellable);

	g_signal_handlers_disconnect_by_func (self->plugin_loader, gs_category_page_copy_dests_notify_cb, self);
	g_clear_pointer (&self->copy_dests, g_ptr_array_unref);

	g_clear_object (&self->builder);
	g_clear_object (&self->category);
	g_clear_object (&self->subcategory);
	g_clear_object (&self->plugin_loader);

	g_clear_pointer (&self->category_apps, g_hash_table_unref);

	G_OBJECT_CLASS (gs_category_page_parent_class)->dispose (object);
}

static void
button_shell_extensions_cb (GtkButton *button, GsCategoryPage *self)
{
	gboolean ret;
	g_autoptr(GError) error = NULL;
	const gchar *argv[] = { "gnome-shell-extension-prefs", NULL };
	ret = g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH,
			     NULL, NULL, NULL, &error);
	if (!ret)
		g_warning ("failed to exec %s: %s", argv[0], error->message);
}

static gboolean
gs_category_page_setup (GsPage *page,
                        GsShell *shell,
                        GsPluginLoader *plugin_loader,
                        GtkBuilder *builder,
                        GCancellable *cancellable,
                        GError **error)
{
	GsCategoryPage *self = GS_CATEGORY_PAGE (page);
	GtkAdjustment *adj;

	self->plugin_loader = g_object_ref (plugin_loader);
	self->builder = g_object_ref (builder);
	self->shell = shell;
	self->sort_type = SUBCATEGORY_SORT_TYPE_RATING;
	gtk_flow_box_set_sort_func (GTK_FLOW_BOX (self->category_detail_box),
				    gs_category_page_sort_flow_box_sort_func,
				    self, NULL);
	self->category_apps = g_hash_table_new_full (g_direct_hash,
						     g_direct_equal,
						     g_object_unref,
						     g_object_unref);

	self->copying = FALSE;
	self->copy_dests = NULL;
	g_signal_connect (self->copy_os_to_usb_button, "clicked",
			  G_CALLBACK (copy_os_to_usb_button_cb), self);
	g_signal_connect (self->cancel_os_copy_button, "clicked",
			  G_CALLBACK (cancel_os_copy_button_cb), self);
	g_signal_connect (plugin_loader, "notify::copy-dests",
			  G_CALLBACK (gs_category_page_copy_dests_notify_cb),
			  self);
	gs_category_page_copy_dests_notify_cb (plugin_loader, NULL, self);

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolledwindow_category));
	gtk_container_set_focus_vadjustment (GTK_CONTAINER (self->category_detail_box), adj);

	gtk_flow_box_set_filter_func (GTK_FLOW_BOX (self->category_detail_box),
				      gs_category_page_filter_apps_func,
				      page,
				      NULL);

	/* add the placeholder tiles already, to be shown when loading the category view */
	for (guint i = 0; i < MAX_PLACEHOLDER_TILES; ++i) {
		GtkWidget *tile = gs_background_tile_new (NULL);
		gtk_container_add (GTK_CONTAINER (self->category_detail_box), tile);
		gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
	}

	g_signal_connect (self->button_category_shell_extensions, "clicked",
			  G_CALLBACK (button_shell_extensions_cb), self);
	return TRUE;
}

static void
gs_category_page_class_init (GsCategoryPageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_category_page_dispose;
	page_class->switch_to = gs_category_page_switch_to;
	page_class->reload = gs_category_page_reload;
	page_class->setup = gs_category_page_setup;
	page_class->os_copied = gs_category_page_os_copied;

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-category-page.ui");

	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, category_detail_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, infobar_category_shell_extensions);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, button_category_shell_extensions);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, scrolledwindow_category);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, subcats_filter_label);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, subcats_filter_button_label);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, subcats_filter_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, popover_filter_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, subcats_sort_label);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, subcats_sort_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, subcats_sort_button_label);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, sort_rating_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, sort_name_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, featured_grid);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, featured_heading);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, header_filter_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, no_apps_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, usb_action_box);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, copy_os_to_usb_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, cancel_os_copy_button);
	gtk_widget_class_bind_template_child (widget_class, GsCategoryPage, os_copy_spinner);
}

GsCategoryPage *
gs_category_page_new (void)
{
	GsCategoryPage *self;
	self = g_object_new (GS_TYPE_CATEGORY_PAGE, NULL);
	return self;
}
