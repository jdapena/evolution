/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
#ifndef _E_ADDRESSBOOK_REFLOW_ADAPTER_H_
#define _E_ADDRESSBOOK_REFLOW_ADAPTER_H_

#include <gal/widgets/e-reflow-model.h>
#include <gal/widgets/e-selection-model.h>
#include "e-addressbook-model.h"
#include "addressbook/backend/ebook/e-book.h"
#include "addressbook/backend/ebook/e-book-view.h"
#include "addressbook/backend/ebook/e-card.h"

#define E_TYPE_ADDRESSBOOK_REFLOW_ADAPTER        (e_addressbook_reflow_adapter_get_type ())
#define E_ADDRESSBOOK_REFLOW_ADAPTER(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_ADDRESSBOOK_REFLOW_ADAPTER, EAddressbookReflowAdapter))
#define E_ADDRESSBOOK_REFLOW_ADAPTER_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), E_TYPE_ADDRESSBOOK_REFLOW_ADAPTER, EAddressbookReflowAdapterClass))
#define E_IS_ADDRESSBOOK_REFLOW_ADAPTER(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_ADDRESSBOOK_REFLOW_ADAPTER))
#define E_IS_ADDRESSBOOK_REFLOW_ADAPTER_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_ADDRESSBOOK_REFLOW_ADAPTER))

typedef struct _EAddressbookReflowAdapter EAddressbookReflowAdapter;
typedef struct _EAddressbookReflowAdapterPrivate EAddressbookReflowAdapterPrivate;
typedef struct _EAddressbookReflowAdapterClass EAddressbookReflowAdapterClass;

struct _EAddressbookReflowAdapter {
	EReflowModel parent;

	EAddressbookReflowAdapterPrivate *priv;
};


struct _EAddressbookReflowAdapterClass {
	EReflowModelClass parent_class;

	/*
	 * Signals
	 */
	gint (* drag_begin) (EAddressbookReflowAdapter *adapter, GdkEvent *event);
};


GType         e_addressbook_reflow_adapter_get_type          (void);
void          e_addressbook_reflow_adapter_construct         (EAddressbookReflowAdapter *adapter,
							      EAddressbookModel         *model);
EReflowModel *e_addressbook_reflow_adapter_new               (EAddressbookModel         *model);

/* Returns object with ref count of 1. */
ECard        *e_addressbook_reflow_adapter_get_card          (EAddressbookReflowAdapter *adapter,
							      int                        index);
#endif /* _E_ADDRESSBOOK_REFLOW_ADAPTER_H_ */
