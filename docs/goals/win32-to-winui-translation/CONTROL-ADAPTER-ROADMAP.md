# Win32 Control Adapter Roadmap

## Product Contract

FluentShell projects explicitly supported, interrogable Win32 control contracts
into real WinUI 3 controls. Native HWND state remains authoritative. It does
not claim that arbitrary GDI pixels or private window procedures can be
reconstructed as equivalent XAML.

`FluentShell.Injector` establishing a renderer connection is process-level
readiness only. Every top-level window has a separate capture, capability,
commit, UIA, and rollback result in `%TEMP%\FluentShell.log`.

## Why Universal Pixel Translation Is Impossible

- GDI is immediate-mode output and does not retain a semantic scene graph.
- `WM_DRAWITEM` and `NM_CUSTOMDRAW` execute application-owned drawing logic;
  the resulting pixels do not reveal commands, hit targets, data models, or
  accessibility behavior.
- A custom HWND can implement rendering, input, validation, and state entirely
  in its private window procedure.
- UI Automation can corroborate semantic roles and patterns, but it does not
  prove visual identity, keyboard routing, context menus, drag/drop, IME, or
  application-private behavior.

Owner-draw/custom controls therefore require an application-specific adapter,
a complete trusted provider contract, or native fallback. A screenshot hosted
inside XAML is not considered translation.

## Adapter Contract

The current class/style classifier will evolve into a registry of versioned
adapters. Each adapter must own the complete contract:

```text
Match(class, realClass, styles, process identity) -> candidate | no match
Probe(HWND on owning thread) -> supported capabilities | rejection reason
Capture(HWND) -> canonical typed state
Observe(message/notification) -> dirty state facets
Invoke(semantic action) -> documented native operation
Verify(HWND) -> recaptured canonical state
Present(state) -> real WinUI control and UIA contract
```

Class names select possible adapters but never establish support by themselves.
Every adapter must reject owner-draw, callback, virtual, or custom behavior it
cannot prove equivalent.

## Delivery Tranches

### Tranche A: Diagnostics And Simple Controls

- Distinguish renderer connection from successful window projection.
- Record concrete root/child class, style, HWND, and rejection stage.
- Remove repeated discovery noise from the Bridge log.
- Add noninteractive `BS_GROUPBOX` projection with UIA Group semantics.
- Add determinate horizontal `msctls_progress32` projection with UIA RangeValue.
- Marquee progress is represented explicitly as indeterminate WinUI ProgressBar
  state and exposes no RangeValue pattern. Keep vertical, custom-draw, and
  invalid-range progress controls native.

### Tranche B: Window Graph And Static Assets

- Model owned top-level/modal graphs rather than rejecting every owned HWND.
- Bounded `SS_ICON` capture is implemented as owned, size-capped premultiplied
  BGRA data; native handles never cross the process boundary. `SS_BITMAP` and
  `SS_ENHMETAFILE` remain unsupported.
- Add standard `SysLink` text/link parsing and `NM_CLICK`/`NM_RETURN` action
  routing with canonical recapture.
- Add tooltip and accessible label relationships.

This tranche targets classic About dialogs such as `winver.exe`.

### Tranche C: Menus And Command Surfaces

- Capture HMENU hierarchy, state, IDs, accelerators, radio/check state, and
  default items before cutover.
- Present a WinUI MenuBar/MenuFlyout command model.
- Route semantic invocation to the owning thread and verify native state.
- Add toolbar and status-bar adapters only after menu command ownership works.

This tranche is required before classic Notepad-like main windows can project.

The bounded textual HMENU portion is now implemented: nested popup/command/
separator capture, enabled/check/radio/default state, WinUI MenuBar projection,
semantic `WM_COMMAND`, canonical recapture, persistent top-level UIA validation,
and owner-draw/bitmap/callback/MDI fallback. Bounded one-row push/separator
`ToolbarWindow32` and textual status-bar subsets are also implemented; toolbar
dropdown/check/group, callback, custom-draw, and multi-row shapes remain native.

### Tranche D: Text Documents

- Define a bounded plain-text RichEdit adapter first.
- Reject OLE, protected ranges, rich formatting, custom word-break procedures,
  callback text, unsupported IME behavior, and private text services.
- Preserve selection, scrolling, modified state, text limits, undo semantics,
  and native notifications before enabling editing.

RichEdit is a document model project, not an alias for the standard Edit
adapter.

### Tranche E: Structured Common Controls

- A bounded report-mode `SysListView32` subset is implemented with native
  columns/rows, visible or hidden headers, independent selection, and optional
  `LVS_EX_CHECKBOXES` state/actions. Virtual, owner/custom-draw, grouped,
  label-editable, activation-tracking, and header-drag semantics remain native.
- A bounded textual top-tab `SysTabControl32` subset is implemented with native
  multiline header rectangles, semantic vetoable selection, and Tab/TabItem UIA.
  Opaque native `TCITEM.lParam` identity is retained only by the source control.
  Owner-draw, button, vertical/bottom, fixed-width, image, tooltip,
  callback-text, excessive, and malformed geometry shapes remain native.
- Add nonvirtual, non-owner/custom-draw TreeView, Header,
  DateTimePicker, MonthCalendar, Trackbar, UpDown, and tooltip subsets.
- Model image lists, selection, expansion, grouping, editing, sorting, and
  notifications as typed capabilities.
- Reject `LVS_OWNERDATA`, callback items, private data, custom draw, and missing
  accessibility/state evidence.

### Tranche F: Application Adapters

- Match canonical executable identity/version plus HWND and UIA signatures.
- Package capture, actions, invalidation, presentation, and rollback tests as
  one versioned adapter.
- Never enable an application adapter globally by class name alone.

This is the only supported lane for private/custom HWND contracts.

The DirectUI translation engine is implemented in `DirectUiEngine.cpp` with
both declarative exact profiles and fail-closed, capability-derived per-surface
profiles; see `DIRECTUI-APPLICATION-ADAPTERS.md`. Exact profiles remain for
MdSched `10.0.26100.7309` and RecoveryDrive `10.0.26100.33296`. Generated
profiles are restricted to Microsoft-signed canonical System32 images, and every
projected slot takes its kind from the registered Win32 adapter that backs it, so
the DirectUI lane's boundary is this roadmap's boundary rather than a second list
maintained per application. Tranche A/B statics and buttons, Tranche C's one-row
toolbar and textual status bar, and Tranche E's report ListView and textual tab
control are all admissible on any built-in DirectUI page because their registry
rows exist; `SysTreeView32` and `msctls_trackbar32` have no row, so they are the
two roles that still keep a DirectUI page native. Unsupported custom or
unexpectedly actionable roles keep the complete surface native.

## Target Findings

- `notepad.exe`: current builds are blocked by HMENU and may additionally use
  RichEdit, status bars, or existing XAML. Menu and text-document tranches are
  prerequisites; already-XAML processes remain excluded.
- `dialer.exe`: observed windows include menus, owned top levels, and
  layered/child shapes. It needs the owner graph and command/common-control
  tranches before whole-window projection can be proven.
- `winver.exe`: the observed first unsupported visible class is `SysLink`.
  Static assets and SysLink support are the shortest path, followed by final
  whole-window UIA validation.

## WinUI Source Policy

Use stock Windows App SDK/WinUI controls and default templates. The
`microsoft-ui-xaml` source is useful for studying visual states, theme
resources, high contrast, focus, and automation peers, but copying templates or
forking WinUI would freeze internal implementation details and create a large
servicing burden. Any copied substantial source would also require preserving
the MIT notice and auditing applicable third-party notices.

Primary references:

- https://github.com/microsoft/microsoft-ui-xaml
- https://github.com/microsoft/microsoft-ui-xaml/blob/main/LICENSE
- https://github.com/microsoft/microsoft-ui-xaml/blob/main/NOTICE.md
- https://learn.microsoft.com/windows/apps/develop/ui/controls/
- https://learn.microsoft.com/windows/win32/controls/window-controls
- https://learn.microsoft.com/windows/win32/controls/common-controls-intro
- https://learn.microsoft.com/windows/win32/controls/wm-drawitem
- https://learn.microsoft.com/windows/win32/controls/about-custom-draw
- https://learn.microsoft.com/windows/win32/winauto/uiauto-controlpatternsoverview

## Completion Rule

A tranche is complete only when its native adapter, protocol model, WinUI
presenter, UIA contract, semantic actions, dynamic reconciliation, rollback,
and target-perspective tests pass together. Adding a visual approximation alone
does not expand the production support boundary.
