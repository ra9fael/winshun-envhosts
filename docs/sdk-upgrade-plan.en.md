# envhosts: complete actions on search results

[中文](sdk-upgrade-plan.md)

## Why change

The plugin already declares actions on environment-variable and hosts items. The missing outcome is reliable, discoverable operation on the selected item, including opening the independent editor at that exact target.

Current action paths contain concrete correctness problems:

- Environment-variable Edit rewrites the query as `env:set NAME=value`, losing scope. The generated edit item uses user scope, and saving calls the user-variable writer. Editing a system result can therefore create or change a same-named user variable instead.
- Inline save derives the registry type from the new value rather than preserving the selected variable's original type.
- Hosts actions check write permission before branching to Copy line or Open file, so these read-only actions are blocked when the file is not writable.
- Query results suffix identical hosts IDs with `-2`, but the action lookup only compares unsuffixed IDs. Delete confirmation uses the first matching raw line, losing which duplicate was selected.
- The expanded-value copy action is present even when there is no distinct expanded value.

## Result actions

| Result | Default action | Additional menu actions |
|---|---|---|
| Environment variable | Edit in the independent editor, selecting the exact scope and name | Copy raw value; copy expanded value when distinct; copy name; copy NAME=value; inline edit for supported simple values; delete |
| Hosts record | Enable or Disable, named for the current state | Edit in the independent editor; copy IP; copy hostname; copy whole line; open hosts file; delete |
| Section/comment-only row | No record mutation | Copy original text where useful |

Keep the familiar Ctrl+Enter copy shortcut. Additional actions remain available in the result menu without forcing every action onto a visible button. Labels and available actions are determined per item.

## Implementation

1. Reuse `editorwin::OpenArgs.select`, which already accepts an environment scope/name or a hosts line. Factor the existing editor-opening setup so both the generic editor entry and item-level Edit share it.
2. When editing an existing environment item, carry its scope, name and original type through the complete edit/save path. Use the appropriate registry scope and corresponding backup. Never silently fall back from system to user scope.
3. Keep inline editing for values supported by the current single-line grammar. PATH split editing, multiline/special registry values and explicit type changes use the existing editor. New-variable commands retain their explicit default scope/type semantics.
4. Handle read-only copy and file-opening actions before write permission checks. Write actions check the actual target at execution and give a clear permission result. Opening a file does not promise an elevated external editor.
5. Use a shared hosts-record resolver for queries, editor targeting, toggles and delete confirmations. Carry the original line and its position/duplicate occurrence. Re-read before mutation, verify the intended record, and refresh with an actionable changed-record result when it no longer matches. Do not add hashes for line comparison.
6. Retain the exact target in the delete confirmation rather than reducing it to raw text or hostname. Confirm and Cancel are explicit actions; unrelated action IDs never commit a delete.
7. After a successful hosts toggle, refresh the visible list and keep it open, retaining the filter and selecting the updated record where the host supports it. Delete remains confirmed. Existing editor external-change handling must preserve unsaved work.
8. Generate meaningful item actions: omit expanded copy when identical, keep copy/view actions available for read-only targets, and show state-specific Enable/Disable labels.

## Plugin preferences

The five current preferences are small enough to remain on the host card. Add appropriate numeric bounds and units for result count and backup retention after confirming existing retention semantics. Result count is currently validated as 1–200. Invalid numeric input must not escape from `std::stoi` and fail a query.

Keep `hostsPath` as text while `%VAR%` expansion remains supported. Converting it to a file-picker-only type would change that contract. A dedicated plugin settings window is not necessary for these five preferences; the existing editor remains the complex data-editing interface.

## Acceptance

- Item JSON carries the intended action set for variables, active/disabled hosts records and sections.
- Editing system and user variables with the same name preserves scope. Editing a value preserves its registry type unless the user explicitly changes type in the editor.
- Read-only hosts supports copying and opening while mutation returns the correct permission result.
- The second of two identical hosts records can be toggled, edited and deleted without affecting the first.
- If the target changes between query and action, the plugin does not mutate another record.
- Editor actions select the correct item when the editor is new or already open, without dropping unsaved edits.
- Querying alone performs no registry writes, hosts writes, clipboard operations or window opens.
- Host result-action dispatch is checked in addition to protocol construction, so declared actions are actually reachable from the result menu.

Use existing test registry subkeys and temporary hosts files. Do not edit the machine's real variables or hosts for automated acceptance.

## Documentation

Update the action table, editing scope/type guarantees, duplicate-record behavior and read-only actions. Align keyboard instructions with actual action keys. Remove claims that the default file-opening action guarantees an administrator editor or that result actions cannot receive foreground permission on the current host.
