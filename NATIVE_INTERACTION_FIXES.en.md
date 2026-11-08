# Native editor interaction fixes

[简体中文](NATIVE_INTERACTION_FIXES.md)

## Verdict

Implement the user's five requested fixes. Each environment variable owns its
pending split-item operations. Navigation only changes the view. Commit applies
the current variable's candidates to the working model; Save collects all
candidates and asks for confirmation before writing to the registry.

## Grounded facts

Before the fix, `applyLayout` cleared the Up/Down rectangles, the footer used
fixed narrow widths and a 16 px button height, item J/K directions were reversed,
and the item list omitted dd/gg/G dispatch. Backup counts initialized only when
opening Backups and the scope exclusion predicates were reversed. Item edits
modified the model immediately; navigation then discarded their pending flags.
These findings came from the affected source and isolated real-window checks.

## Architecture and interaction

All changes remain in the existing native GDI editor. No protocol, thread,
dependency or permission changes are needed. Per-variable sessions retain
values, added/deleted markers, delimiter, selection and undo/redo history.
Commit creates one model undo step. Reload explicitly discards candidates;
drafts serialize them separately from model rows.

Up/Down buttons show Alt+Up/Down in the footer Items group. J/Ctrl+J move down and
K/Ctrl+K move up; Alt+J/K reorder down/up. dd stages deletion, gg selects the
first item and G the last. Text fields retain literal typing. Footer labels
use actual font measurements and wrap if necessary. The backup tab keeps a
total independent of the scoped list, refreshed at startup, Save and deletion.

## Failure matrix and tests

| Scenario | Required result |
|---|---|
| Default/narrow width, Chinese/English | Visible buttons, complete text, no child overlap |
| List/text focus | Correct commands; literal text stays literal |
| Navigate after add/edit/delete/reorder | Retain candidates; model unchanged |
| Undo/redo/Commit | Local candidate history; Commit is one model step |
| User/System/empty backup scope | Correct scoped list and stable total |
| Save/reload/draft | Explicit application or discard; no hidden writes |

L0 verifies the test archive and executable. L1 runs logic/model/layout CTest.
L2 uses isolated registry and hosts protocol fixtures. L3 opens the native editor
from a real isolated WinShun host. L4 covers focused and existing real-window
tests with synthetic screenshots. L5 does not publish a new release in this turn.

## Permissions, delivery and limits

Preserve real environment/hosts data and running applications. Update both
READMEs, the bilingual changelog and validation evidence. Physical monitor DPI
switching and actual elevated writes remain untested. All write tests use
isolated fixture data.

## Delivery report — 2026-11-08

All five fixes are implemented. Candidate-only drafts prompt for recovery;
shared validation checks the variable identity, source and delimiter. Restored
candidates retain markers and can be undone together for that variable, then
redone. Footer measurement reserves every delete/restore label variant.

Passed: CTest 3/3, protocol 37/37, original GUI 145/145, focused interaction
51/51, split state 19/19 and both dialog locales 8/8. The original GUI run
preceded the final candidate-draft fix; focused and dialog suites cover those
final changes. A real isolated WinShun host opened the matching executable with
the expected query filter, verified by readback and SHA256, and exited with 0.

Evidence: `WinShun/docs/validation/envhosts-interaction-20261108/`.
The separate test archive retains manifest 0.7.3; it is not a new release/tag.

## Approved follow-up: row status and footer actions (implemented)

The follow-up screenshot shows Path without a pending indicator. Painting reads
`m_env.rowState()` but does not combine it with `partPending()` from the
per-variable sessions. Fix the presentation without applying candidates early.

Combine candidate and model status by `scope/name`. Candidate-only rows show
an amber Pending label with the explanation “Uncommitted split-item changes”.
Keep the table's model value and the detail candidate preview. After Commit,
show Unsaved; retain the meaning of added/deleted variables. When both states
exist, show Pending · Unsaved. Undo restores the remaining model state; a
successful Save clears the indicator when no candidates remain. Measure the
status column for the full locale labels and shorten the value preview first.
Keep errors/overlength notices visible and retain indicators on selected rows.
Repaint for add/edit/delete/reorder, navigation, filters and draft recovery.

Move Up Alt+Up / Down Alt+Down into an Items group in the existing footer, spaced
apart from window-wide Save/Close. Show this group only in environment split-item
mode. It acts on the current variable's current item and restores list focus
after a click. Disable Up at the first item, Down at the last, and both for empty
or read-only lists. Keep keyboard bindings unchanged. Remove the old dedicated
row to give its height back to the list; shorten its hint without leaving a
blank band. Keep Commit near the candidate preview. Measure footer labels and
wrap groups, reserving height before laying out the panes.

Verify row status through candidate operations, navigation, undo/redo, Commit
and Save, including combined model/candidate changes and equal variable names
in different scopes. Check action targets, boundaries, focus, keys and Chinese/
English layouts at 660/980 px. Candidate operations must leave the model and
registry unchanged. The user approved this implementation. Focused interaction
94/94, original GUI 145/145 and CTest 3/3 passed. Synthetic screenshots confirm
combined Pending/Unsaved status and footer groups in Chinese/English at 660/980 px.
Use the existing authorization to commit and create 0.7.4 / v0.7.4 and its local
test archive, without public publishing or online index updates. Final clean
source build, archive and host checks are recorded in
`WinShun/docs/validation/envhosts-v0.7.4-20261108/`.

[简体中文](NATIVE_INTERACTION_FIXES.md)
