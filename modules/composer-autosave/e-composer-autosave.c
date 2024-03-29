/*
 * e-composer-autosave.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <libebackend/e-extension.h>

#include <e-util/e-alert-dialog.h>
#include <composer/e-msg-composer.h>

#include "e-autosave-utils.h"

/* Standard GObject macros */
#define E_TYPE_COMPOSER_AUTOSAVE \
	(e_composer_autosave_get_type ())
#define E_COMPOSER_AUTOSAVE(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_COMPOSER_AUTOSAVE, EComposerAutosave))

#define AUTOSAVE_INTERVAL	60 /* seconds */

typedef struct _EComposerAutosave EComposerAutosave;
typedef struct _EComposerAutosaveClass EComposerAutosaveClass;

struct _EComposerAutosave {
	EExtension parent;

	GCancellable *cancellable;
	guint timeout_id;

	/* Composer contents have changed since
	 * the last auto-save or explicit save. */
	gboolean changed;

	/* Prevent error dialogs from piling up. */
	gboolean error_shown;
};

struct _EComposerAutosaveClass {
	EExtensionClass parent_class;
};

/* Forward Declarations */
GType e_composer_autosave_get_type (void);
void e_composer_autosave_type_register (GTypeModule *type_module);

G_DEFINE_DYNAMIC_TYPE (
	EComposerAutosave,
	e_composer_autosave,
	E_TYPE_EXTENSION)

static void
composer_autosave_finished_cb (EMsgComposer *composer,
                               GAsyncResult *result,
                               EComposerAutosave *autosave)
{
	GFile *snapshot_file;
	GError *error = NULL;

	snapshot_file = e_composer_get_snapshot_file (composer);
	e_composer_save_snapshot_finish (composer, result, &error);

	/* Return silently if we were cancelled. */
	if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_error_free (error);

	else if (error != NULL) {
		gchar *basename;

		if (G_IS_FILE (snapshot_file))
			basename = g_file_get_basename (snapshot_file);
		else
			basename = g_strdup (" ");

		/* Only show one error dialog at a time. */
		if (!autosave->error_shown) {
			autosave->error_shown = TRUE;
			e_alert_run_dialog_for_args (
				GTK_WINDOW (composer),
				"mail-composer:no-autosave",
				basename, error->message, NULL);
			autosave->error_shown = FALSE;
		} else
			g_warning ("%s: %s", basename, error->message);

		g_free (basename);
		g_error_free (error);
	}

	g_object_unref (autosave);
}

static gboolean
composer_autosave_timeout_cb (EComposerAutosave *autosave)
{
	EExtensible *extensible;

	extensible = e_extension_get_extensible (E_EXTENSION (autosave));

	/* User may have reverted or explicitly saved
	 * the changes since the timeout was scheduled. */
	if (autosave->changed) {

		/* Cancel the previous snapshot if it's still in
		 * progress and start a new snapshot operation. */
		g_cancellable_cancel (autosave->cancellable);
		g_object_unref (autosave->cancellable);
		autosave->cancellable = g_cancellable_new ();

		e_composer_save_snapshot (
			E_MSG_COMPOSER (extensible),
			autosave->cancellable,
			(GAsyncReadyCallback)
			composer_autosave_finished_cb,
			g_object_ref (autosave));
	}

	autosave->timeout_id = 0;
	autosave->changed = FALSE;

	return FALSE;
}

static void
composer_autosave_changed_cb (EComposerAutosave *autosave)
{
	GtkhtmlEditor *editor;
	EExtensible *extensible;

	extensible = e_extension_get_extensible (E_EXTENSION (autosave));

	editor = GTKHTML_EDITOR (extensible);
	autosave->changed = gtkhtml_editor_get_changed (editor);

	if (autosave->changed && autosave->timeout_id == 0)
		autosave->timeout_id = g_timeout_add_seconds (
			AUTOSAVE_INTERVAL, (GSourceFunc)
			composer_autosave_timeout_cb, autosave);
}

static void
composer_autosave_dispose (GObject *object)
{
	EComposerAutosave *autosave;
	GObjectClass *parent_class;

	autosave = E_COMPOSER_AUTOSAVE (object);

	/* Cancel any snapshots in progress. */
	if (autosave->cancellable != NULL) {
		g_cancellable_cancel (autosave->cancellable);
		g_object_unref (autosave->cancellable);
		autosave->cancellable = NULL;
	}

	if (autosave->timeout_id > 0) {
		g_source_remove (autosave->timeout_id);
		autosave->timeout_id = 0;
	}

	/* Chain up to parent's dispose() method. */
	parent_class = G_OBJECT_CLASS (e_composer_autosave_parent_class);
	parent_class->dispose (object);
}

static void
composer_autosave_constructed (GObject *object)
{
	EExtensible *extensible;
	GObjectClass *parent_class;

	/* Chain up to parent's constructed() method. */
	parent_class = G_OBJECT_CLASS (e_composer_autosave_parent_class);
	parent_class->constructed (object);

	extensible = e_extension_get_extensible (E_EXTENSION (object));

	g_signal_connect_swapped (
		extensible, "notify::changed",
		G_CALLBACK (composer_autosave_changed_cb), object);
}

static void
e_composer_autosave_class_init (EComposerAutosaveClass *class)
{
	GObjectClass *object_class;
	EExtensionClass *extension_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = composer_autosave_dispose;
	object_class->constructed = composer_autosave_constructed;

	extension_class = E_EXTENSION_CLASS (class);
	extension_class->extensible_type = E_TYPE_MSG_COMPOSER;
}

static void
e_composer_autosave_class_finalize (EComposerAutosaveClass *class)
{
}

static void
e_composer_autosave_init (EComposerAutosave *autosave)
{
	autosave->cancellable = g_cancellable_new ();
}

void
e_composer_autosave_type_register (GTypeModule *type_module)
{
	/* XXX G_DEFINE_DYNAMIC_TYPE declares a static type registration
	 *     function, so we have to wrap it with a public function in
	 *     order to register types from a separate compilation unit. */
	e_composer_autosave_register_type (type_module);
}
