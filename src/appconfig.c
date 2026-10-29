/*
 * appconfig.c - see appconfig.h for why this module exists.
 */

#include "appconfig.h"

#include "model/store.h"

BOOL appconfig_save(const wchar_t *path,
                    const Settings *settings,
                    const WorkspaceSet *workspaces)
{
    if (path == NULL || settings == NULL || workspaces == NULL) return FALSE;

    /*
     * store_open_for_write(), NOT store_create().
     *
     * This function owns [meta], [settings] and [workspace:*]. The same file
     * also holds the [rule:*] sections written by rules_save(), and a fresh
     * document would overwrite them. That was the measured failure: anchoring
     * a preset and then closing the program normally left the file with zero
     * rules, so the preset never came back.
     *
     * ws_to_store() additionally removes workspaces and slots that no longer
     * exist, and settings_to_store() rewrites its whole (fixed) key set, so
     * neither a stale section nor an unrelated one can survive or be lost.
     */
    StoreDocument *doc = store_open_for_write(path);
    if (doc == NULL) return FALSE;

    store_set(doc, "meta", "version", "1");
    settings_to_store(settings, doc);
    ws_to_store(workspaces, doc);

    const BOOL ok = store_save(path, doc);
    store_destroy(doc);
    return ok;
}
