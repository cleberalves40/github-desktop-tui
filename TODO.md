# Backlog

Baseline: `v0.7.0` (`main` at the start of the code freeze).
Code freeze: 2026-10-28 through 2026-11-04. Collect and refine requests during the freeze; do not implement them until it ends.

## Usability

- [ ] **Parent-directory navigation:** Show a clickable `..` entry in the remote repository browser. It must navigate to the parent directory, matching the existing Backspace behavior. Keep a keyboard shortcut for the same action; confirm whether Backspace remains the chosen shortcut. At the repository root, return to the repository list/Home.
- [ ] **Cheat Codes overlay:** Add a floating help window listing the most useful commands and shortcuts, grouped by screen or workflow. Open it with a keyboard shortcut and close it with Esc. Decide the opening shortcut during backlog review.
- [ ] **Double-click to browse:** Double-clicking a repository in Home opens its remote file browser without cloning. A single click continues to select the repository.
- [ ] **Browse with a key chord:** A keyboard combination ending in Enter opens the selected repository's remote file browser, with the same effect as double-click. Keep Enter alone for cloning. Decide the exact key combination during backlog review.

## Release Quality

- [ ] **Sanitizer and memory checks:** Run AddressSanitizer (ASan) and Valgrind before every version. Record the command, platform, and result for each run; decide which checks can be automated in CI and on which platforms.

## Daily Log

- **2026-10-28:** Created this backlog during the `v0.7.0` code freeze. No code changes are included.