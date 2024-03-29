/*
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
 *
 * Authors:
 *		Michel Zucchi <notzed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef EM_CONFIG_H
#define EM_CONFIG_H

#include <camel/camel.h>
#include <gconf/gconf-client.h>
#include <libedataserver/e-account.h>

#include "e-util/e-config.h"

G_BEGIN_DECLS

typedef struct _EMConfig EMConfig;
typedef struct _EMConfigClass EMConfigClass;
typedef struct _EMConfigPrivate EMConfigPrivate;

/* Current target description */
/* Types of popup tagets */
enum _em_config_target_t {
	EM_CONFIG_TARGET_FOLDER,
	EM_CONFIG_TARGET_PREFS,
	EM_CONFIG_TARGET_ACCOUNT
};

typedef struct _EMConfigTargetFolder EMConfigTargetFolder;
typedef struct _EMConfigTargetPrefs EMConfigTargetPrefs;
typedef struct _EMConfigTargetAccount EMConfigTargetAccount;

struct _EMConfigTargetFolder {
	EConfigTarget target;

	CamelFolder *folder;
};

struct _EMConfigTargetPrefs {
	EConfigTarget target;

	/* preferences are global from gconf */
	GConfClient *gconf;
};

struct _EMConfigTargetAccount {
	EConfigTarget target;

	EAccount *original_account;
	EAccount *modified_account;
	CamelSettings *settings;
};

typedef struct _EConfigItem EMConfigItem;

struct _EMConfig {
	EConfig config;
	EMConfigPrivate *priv;
};

struct _EMConfigClass {
	EConfigClass config_class;
};

GType		em_config_get_type		(void);
EMConfig *	em_config_new			(gint type,
						 const gchar *menuid);
EMConfigTargetFolder *
		em_config_target_new_folder	(EMConfig *emp,
						 CamelFolder *folder);
EMConfigTargetPrefs *
		em_config_target_new_prefs	(EMConfig *emp,
						 GConfClient *gconf);
EMConfigTargetAccount *
		em_config_target_new_account	(EMConfig *emp,
						 EAccount *original_account,
						 EAccount *modified_account,
						 CamelSettings *settings);
void		em_config_target_new_account_update_settings
						(EConfig *ep,
						 EMConfigTargetAccount *target,
						 CamelSettings *settings);

G_END_DECLS

#endif /* EM_CONFIG_H */
