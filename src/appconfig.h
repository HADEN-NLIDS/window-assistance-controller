/*
 * appconfig.h - saving the sections the main window owns.
 *
 * WHY THIS IS ITS OWN FILE
 *
 * The configuration file is shared: [meta], [settings] and [workspace:*]
 * belong to the main window, while [rule:*] belongs to the rule module.
 * "Save the main window's configuration" is therefore one operation that must
 * load the existing file first and then write only its own sections.
 *
 * It has to be TESTABLE, because getting it wrong loses the user's data
 * silently. Measured 2026-09-23: implemented as "build a fresh document and
 * save it", this exact operation deleted every [rule:*] section - and since
 * it runs unconditionally on exit, a window preset anchored in the UI was
 * gone the next time the program started, with nothing said about it.
 *
 * The module sits at the top of src/ (beside main.c) because it coordinates
 * ui/ (Settings) and model/ (WorkspaceSet, store) - it belongs to neither.
 */

#ifndef WAC_APPCONFIG_H
#define WAC_APPCONFIG_H

#include <windows.h>

#include "model/workspace.h"
#include "ui/page_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Write the main window's sections to `path`, keeping every section it does
 * not own - in particular [rule:*].
 *
 * The existing file is loaded first; see store_open_for_write() in
 * model/store.h for why that is mandatory rather than an optimisation.
 * A missing file is the normal first-run case, and a damaged one has already
 * been reported to the user by the time any save happens.
 *
 * Returns FALSE only when the save failed (out of memory, or the file could
 * not be written). The caller logs that; there is nothing else to be done.
 */
BOOL appconfig_save(const wchar_t *path,
                    const Settings *settings,
                    const WorkspaceSet *workspaces);

#ifdef __cplusplus
}
#endif

#endif /* WAC_APPCONFIG_H */
