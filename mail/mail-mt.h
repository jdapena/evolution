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
 *		Michael Zucchi <notzed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef _MAIL_MT
#define _MAIL_MT

#include <camel/camel.h>
#include <e-util/e-activity.h>

typedef struct _MailMsg MailMsg;
typedef struct _MailMsgInfo MailMsgInfo;

typedef gchar *	(*MailMsgDescFunc)		(MailMsg *msg);
typedef void	(*MailMsgExecFunc)		(MailMsg *msg,
						 GCancellable *cancellable,
						 GError **error);
typedef void	(*MailMsgDoneFunc)		(MailMsg *msg);
typedef void	(*MailMsgFreeFunc)		(MailMsg *msg);
typedef void	(*MailMsgDispatchFunc)		(gpointer msg);

struct _MailMsg {
	MailMsgInfo *info;
	volatile gint ref_count;
	guint seq;			/* seq number for synchronisation */
	gint priority;			/* priority (default = 0) */
	EActivity *activity;
	GError *error;			/* up to the caller to use this */
};

struct _MailMsgInfo {
	gsize size;
	MailMsgDescFunc desc;
	MailMsgExecFunc exec;
	MailMsgDoneFunc done;
	MailMsgFreeFunc free;
};

/* setup ports */
void mail_msg_init (void);

gboolean mail_in_main_thread (void);

/* allocate a new message */
gpointer mail_msg_new (MailMsgInfo *info);
gpointer mail_msg_ref (gpointer msg);
void mail_msg_unref (gpointer msg);
void mail_msg_check_error (gpointer msg);
void mail_msg_cancel (guint msgid);
gboolean mail_msg_active (void);

/* dispatch a message */
void mail_msg_main_loop_push (gpointer msg);
void mail_msg_unordered_push (gpointer msg);
void mail_msg_fast_ordered_push (gpointer msg);
void mail_msg_slow_ordered_push (gpointer msg);

/* To implement the stop button */
GHook * mail_cancel_hook_add (GHookFunc func, gpointer data);
void mail_cancel_hook_remove (GHook *hook);
void mail_cancel_all (void);

/* request a string/password */
gchar *mail_get_password (CamelService *service, const gchar *prompt,
			 gboolean secret, gboolean *cache);

void mail_mt_set_backend (gchar *backend);

/* Call a function in the GUI thread, wait for it to return, type is
 * the marshaller to use.  FIXME This thing is horrible, please put
 * it out of its misery. */
typedef enum {
	MAIL_CALL_p_p,
	MAIL_CALL_p_pp,
	MAIL_CALL_p_ppp,
	MAIL_CALL_p_pppp,
	MAIL_CALL_p_ppppp,
	MAIL_CALL_p_ppippp
} mail_call_t;

typedef gpointer (*MailMainFunc)();

gpointer mail_call_main (mail_call_t type, MailMainFunc func, ...);

#endif /* _MAIL_MT */
