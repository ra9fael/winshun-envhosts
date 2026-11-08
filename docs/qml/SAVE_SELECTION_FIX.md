# QML env selection after save — 0.8.0-qml.8

## Cause

Successful writes read the target back and call `EditorSession::load(..., true)`.
This kept environment row IDs, but reset the selection to the first row and
discarded split-item IDs, selection and delimiter. The UI therefore switched
away from the variable being edited even though the write succeeded.

## Behavior

- Adopt the verified snapshot while restoring the current variable, selected
  rows and Shift-selection anchor by stable ID in the active scope/filter/order.
- If the current variable was deleted, focus a surviving selected variable, or
  the next surviving visible neighbor, falling backward at the end of the list.
  An empty scope has no current item. Explicitly cleared selections stay clear.
- Keep the existing scope, sort, manual insertion order and view state. Renaming
  or saving a newly inserted variable does not lose its ID.
- Preserve split-item IDs, selection and custom delimiter when the readback
  matches their effective value. Remove saved deletions and clear added markers.
  Duplicate values remain distinct because matching uses IDs, not item text.
- Reload and external refresh also preserve the current variable. External
  changes to its value rebuild split items from actual data using the delimiter;
  stale split selections are not applied to new values.

The verified target still becomes the new baseline and clears undo/redo history.
The memory and deferred-open fixes from qml.7 are retained.

## Verification

Built `envhosts` and `qml_editor_tests`. Nine targeted tests passed, including
four new regressions for scope/multiselection/rename/new-row saves, split-item
readback and external edits, deletion fallback, and saving through real QML
buttons while retaining the current item and scroll position.

Evidence: `build-qml/evidence/save-selection-qml8.txt`.
Storage in these save tests uses `FakeStore` with temporary data directories.
The running WinShun and installed plugin were not replaced.

Package: `dist/qml-preview-11/ra9fael.envhosts-qml.wsplugin`.

Manual acceptance: select a variable away from the first row, change its value
or PATH split items, save, and confirm the same variable/items remain selected.
Repeat with multiselection, a renamed or newly inserted variable, and the system
scope. Deleting the current variable should choose a neighbor in that scope.
