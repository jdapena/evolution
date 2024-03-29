/*
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
 *
 * Authors:
 *		Not Zed <notzed@lostzed.mmc.com.au>
 *      Jeffrey Stedfast <fejj@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#ifndef EM_FILTER_RULE_H
#define EM_FILTER_RULE_H

#include "filter/e-filter-rule.h"

#define EM_TYPE_FILTER_RULE \
	(em_filter_rule_get_type ())
#define EM_FILTER_RULE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), EM_TYPE_FILTER_RULE, EMFilterRule))
#define EM_FILTER_RULE_CLASS(cls)    (G_TYPE_CHECK_CLASS_CAST ((cls), EM_TYPE_FILTER_RULE, EMFilterRuleClass))
#define EM_IS_FILTER_RULE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), EM_TYPE_FILTER_RULE))
#define EM_IS_FILTER_RULE_CLASS(cls) (G_TYPE_CHECK_CLASS_TYPE ((cls), EM_TYPE_FILTER_RULE))
#define EM_FILTER_RULE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), EM_TYPE_FILTER_RULE, EMFilterRuleClass))

typedef struct _EMFilterRule EMFilterRule;
typedef struct _EMFilterRuleClass EMFilterRuleClass;

struct _EMFilterRule {
	EFilterRule parent_object;

	GList *actions;
};

struct _EMFilterRuleClass {
	EFilterRuleClass parent_class;
};

GType           em_filter_rule_get_type (void);
EFilterRule *	em_filter_rule_new      (void);

/* methods */
void            em_filter_rule_add_action     (EMFilterRule *fr, EFilterPart *fp);
void            em_filter_rule_remove_action  (EMFilterRule *fr, EFilterPart *fp);
void            em_filter_rule_replace_action (EMFilterRule *fr, EFilterPart *fp, EFilterPart *new);

void            em_filter_rule_build_action   (EMFilterRule *fr, GString *out);

#endif /* EM_FILTER_RULE_H */
