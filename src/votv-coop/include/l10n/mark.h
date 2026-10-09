// l10n/mark.h -- the markers that do nothing at run time: a literal in a static table (a config row's
// label, an F1 tree name) wrapped so the template extractor finds it, translated where it is drawn.
// MTA's `_td` (Client/sdk/core/CLocalizationInterface.h). Alone in its header so a table's owner, such
// as coop/config, includes nothing else of l10n.
#pragma once

#define L10N_MARK(s)        s
#define L10N_MARK_C(ctx, s) s
