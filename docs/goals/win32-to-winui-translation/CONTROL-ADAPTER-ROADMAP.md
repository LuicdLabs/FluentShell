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
  One level is now implemented: a dialog the application runs modally against a
  projected owner (the owner is on screen and disabled for the dialog's lifetime)
  becomes a surface of its own, and its proxy inherits the real owner and blocks
  it -- the same contract a translated MessageBox already met. Discovery classifies
  every root before the windows it owns, and a visible *modal* dialog does not hold
  its owner native, so a console that opens a dialog during startup (`rsop.msc`)
  projects the owner first and the dialog on top in the same pass. A window whose
  whole owner chain is hidden (the `StubWindow32` behind a control-panel property
  sheet) or has no area (odbcad32's zero-size `ODBCAdmClass64` popup) counts as
  unowned. A modeless owned top-level, and a dialog nested deeper than
  one level, still resolve the whole graph back to native together: a nested dialog
  inherits DWM cloaking from its cloaked owner and the proxy cannot take the
  foreground slot the committed gate requires.
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
  columns/rows, visible or hidden headers, independent selection, optional
  `LVS_EX_CHECKBOXES` state/actions, per-item icons, and in-place label editing
  where `LVS_EDITLABELS` admits it. Owner/custom-draw, grouped,
  activation-tracking, and header-drag semantics remain native.
- A bounded textual top-tab `SysTabControl32` subset is implemented with native
  multiline header rectangles, semantic vetoable selection, and Tab/TabItem UIA.
  The strip is a real WinUI `TabView`. Its template reserves an inset above the tab
  row and applies `TabViewItemHeaderPadding` inside the item, and `UpdateTabWidths`
  clears any `Width` a `SizeToContent` pill is given, so none of the native band's
  geometry can be asserted through public properties. The row is therefore
  calibrated by measurement after layout: it is shifted so the realized pills land
  on the native band, clipped to that band so the reserved content area cannot cover
  or swallow clicks over the projected page, and each pill's content is sized to the
  native item rectangle minus the measured template chrome. Opaque native `TCITEM.lParam` identity is retained only by the source control.
  Owner-draw, button, vertical/bottom, fixed-width, image, tooltip,
  callback-text, excessive, and malformed geometry shapes remain native.
- A bounded `SysTreeView32` subset is implemented: the whole inserted hierarchy is
  flattened into one canonical depth-first order carrying per-item label, nesting
  depth, expansion, child evidence, and normal/selected image indexes, with single
  selection, a real expansion route so a lazily populated tree still fills in
  through its own `TVN_ITEMEXPANDING`, and in-place label editing where
  `TVS_EDITLABELS` admits it. An empty tree is a valid snapshot, because a console
  clears its scope tree while a snap-in reloads. Overlay images are composited into
  the item icon the control paints. A state image list is a second bounded list
  drawn beside the icon; state-image bits with no list are the control's private
  user mask and are not a refusal. Checkboxes, hover selection, auto-collapse,
  callback children, application infotips, and multi-select extended styles remain
  native.
- A bounded `msctls_trackbar32` subset is implemented: range, position, line and
  page size, orientation, and the native reversed hint, driven through the
  control's own `WM_HSCROLL`/`WM_VSCROLL` notification and validated as UIA
  RangeValue against canonical native state. `TBS_TOOLTIPS` (the position shown
  beside the thumb while dragging) projects as the Slider's thumb tooltip. Selection
  ranges, thumbless bars, other attached tooltips, and pre-move veto snapping remain
  native.
- Add nonvirtual, non-owner/custom-draw Header, DateTimePicker, MonthCalendar,
  UpDown, and tooltip subsets.
- Model selection, expansion, grouping, sorting, and notifications as typed
  capabilities.
- Bounded `LVS_OWNERDATA` ListViews are supported in report, large-icon,
  small-icon, and list modes. `LVM_GETITEMW` obtains text and image indexes
  through the application's own `LVN_GETDISPINFO` callback on the owning GUI
  thread; `LVM_GETITEMTEXT` is unsupported for these controls. The same 4,096-item,
  64-column, and 256 Ki-character aggregate text limits apply. Columns, native
  item geometry, selection, focus, scrolling, and the application's in-place
  label-edit acceptance or veto retain their existing contracts. Virtual row
  indexes are never treated as stable item IDs, so deferred default activation
  remains disabled. Indexed mutations reject an unreconciled native snapshot
  change before touching the application. Owner-data checkboxes and callback
  state images, overlays, cut state, or drop highlighting remain native until
  their application-owned state contracts can be projected faithfully.
- Plain report lists inherit WinUI's virtualizing `ItemsStackPanel`; assigning
  a null panel before theme attachment previously forced full realization.
  Checkable reports remain fully realized while the UIA checkbox gate maps
  enumeration order to canonical row indexes.
- Stable-ID ListView activation first invokes the current MSAA default action.
  A missing method (`DISP_E_MEMBERNOTFOUND`) may use native double-click only
  when current metadata matches `CreateStdAccessibleObject`, native selection
  and focus still identify the original item, and the control hit-tests a
  visible first-column label/icon point. Native handlers generate their own
  notifications. No Enter or fabricated `WM_NOTIFY` replacement is used.
  Provider callbacks, item replacement, cancellation, restore and capture
  changes are guarded before dispatch and before the trailing button-up.
  Computer Management result-list navigation has live evidence; other snap-in
  actions and dialogs remain tracked in the validation matrix.
- Reject unmodeled callback semantics, private data, custom draw, and missing
  accessibility/state evidence.

### Tranche H: Per-Item Imagery And In-Place Renaming

Item icons and in-place renaming are the two capabilities that decided whether a
projected tree or list was a faithful translation or a downgrade, so both are
implemented rather than refused.

- **Imagery travels as a bounded shared image list, not per-item pixels.** A
  `treeView`/`listView` node carries at most `kMaxImageListImages` (64) entries of
  at most 64x64 premultiplied BGRA, and items carry only indexes into it
  (`-1` for no icon), which is how Win32 already models them: one image list,
  many referring items. A control whose image list exceeds those bounds, or whose
  items resolve to an index outside it, is refused instead of projected with
  missing icons. Trees additionally carry the selected-state index, so the
  projection shows the open-folder icon for the current selection the way the
  native control does. An overlay is not a second list: capture composites it
  into the icon the control draws, keyed by image index plus overlay index, so
  two items can share a base image and still differ where one carries a badge.
  State images travel as `stateImageList` and `itemStateImages` and share the
  icon count and decoded-pixel budget. Checkbox styles stay refused, because
  those glyphs are a toggle the control owns rather than a decoration.
- **A rename runs the native control's own label session.** The renderer hosts the
  typing experience locally (F2 or the UIA Value pattern opens a TextBox over the
  item), and the commit is one source-thread command that opens
  `TVM_EDITLABELW`/`LVM_EDITLABELW`, writes the text into the control's own edit
  child, ends the session, and reads the item text back to decide the outcome.
  The application's `TVN_BEGINLABELEDIT`/`LVN_BEGINLABELEDIT` veto, its
  `ENDLABELEDIT` veto, and any text normalization it performs therefore all still
  apply. The session is never left open across user typing, because the native
  window is cloaked and the renderer owns the desktop focus; the command moves
  only the source thread's focus, which is what the two messages check.
- **A refused action is not a failed projection.** An application that runs the
  operation and declines it (an invalid or duplicate name) reports `refused`, which
  the Bridge turns into a `rejected` action result plus a patch from canonical
  state. Only an operation that could not be performed at all still rolls the
  whole window back. Without that distinction a routine rename veto would tear
  down the surface.
- The label-edit child window is absorbed as a composite implementation child, so
  a control caught mid-session by a reconcile does not read as an unsupported
  extra HWND.

### Translated Dialog Lane Regression Fixed Alongside Tranche H

The MessageBox/TaskDialog lane had stopped projecting: both surfaces are virtual by
construction (the Bridge answers the API call itself, so no native dialog HWND is
ever created), but the renderer's application-adapter rule required every node on a
non-adapter surface to be HWND-backed, and the TaskDialog builder never assigned a
z-index while the protocol requires it to be unique per node. Either one faulted the
surface the moment a dialog opened, which rolled the whole owner window back to
native.

- The lane rule is now stated per surface kind: `window` surfaces are HWND-backed
  and `messageBox`/`taskDialog` surfaces are fully virtual, refused in both
  directions, in `ProtocolValidator` and in the schema.
- The dialog snapshot builders moved out of `RendererSession` into
  `src/Bridge/Translation/DialogSnapshots.cpp` so the native suite can hold them to
  the same cross-node invariants the renderer enforces on admission
  (`TestVirtualDialogSnapshots`). The regression was invisible to both suites before
  that, because nothing built a dialog snapshot outside the live hook path.
- Admission failures now name the offending node and field instead of only the rule.

### Translated Dialogs Completed (protocol minor 38, 2026-10-10)

The lane used to lay every MessageBox out at a fixed 540x300 with English labels, and
virtualized only a static TaskDialog subset (anything with a callback, radio buttons,
command links, hyperlinks, a progress bar or expandable details went native, where its
DirectUI content handed the owner graph back to native).

- Layout: dialogs are sized to their text, measured with DirectWrite (Segoe UI and the
  system fallback, the font XAML draws; GDI's font linking measured CJK too narrow and
  clipped it), with native-like margins, the icon beside the text, and the buttons
  right-aligned in a shaded command band. Bounds include the proxy's frame.
- Labels: user32's own (`MB_GetString`): `確定`, `是(&Y)`. A lone OK closes on Escape.
- `MessageBoxIndirectW` is hooked (resource text, `MB_USERICON` as a picture node), and
  `TaskDialog` alongside `TaskDialogIndirect` (in the comctl32 that exports it).
- TaskDialog: every `TASKDIALOGCONFIG` feature (custom and common buttons, command links
  with notes and shields, radio buttons, verification, expandable information in the
  content or footer area, footer and footer icon, application icons, hyperlinks,
  normal and marquee progress, `cxWidth`, positioning) and the callback contract
  (`TDN_*` on the caller's thread with a real HWND; `TDM_*` including navigation,
  progress, element text, enable/elevation state and icon updates; `S_FALSE` vetoes).
- Protocol minor 38: a task dialog's nodes carry presentation variants
  (`mainInstruction`, `commandLink*`, `elevated`, `expando*`) and a command link's note
  as `helpText`; its text is a `sysLink` with one label per link, named by index through
  `activateItem`. A window surface still admits a single-link SysLink only.
- Evidence: `TestVirtualDialogSnapshots` and `TestVirtualTaskDialogCallbacks` (native),
  `TaskDialogProtocolTests` (renderer), and LegacyDialogHost's "Rich TaskDialog" and
  "Information message" commands driven live, including a MessageBox raised from the
  task dialog's callback and a navigation; Device Manager's "no driver files" message
  box over a device's Properties.

### Tranche J: Accessible Islands And The Full Toolbar Surface

Some private container classes host their content as objects that own no HWND at
all. DirectUI is the common one, and MMC's Actions pane is the case that forced this
lane: the geometric container rule correctly reports that `DirectUIHWND` has no
visible children to frame, because its elements are not windows.

- **The elements are read through MSAA, not UI Automation.** UIA normalizes
  DirectUI's "More Actions" row into a `Menu` that exposes **no actionable pattern at
  all** -- not Invoke, not ExpandCollapse, not LegacyIAccessible -- so a UIA-only
  projection would have to refuse it. The same element answers `IAccessible` with
  `accRole = popup menu` and a real `accDefaultAction`. MSAA is also answered inline
  by the provider on the window's own thread, so an island is captured inside the
  ordinary source-thread pass instead of needing an out-of-band UIA worker with its
  own deadline, poisoning guard, and revision coupling.
- **The elements travel as typed items on the island's own HWND-backed node**, which
  is the shape a Toolbar's buttons and an HMENU's items already use. No node on a
  generic surface becomes virtual, so the "every projected node is HWND-backed" rule
  is untouched and the DirectUI application lane is not disturbed.
- **Admission is fail-closed on evidence, not on class alone.** The host class must be
  in a closed set (`DirectUIHWND` -- a Windows class, not an application's), the
  window must own no visible child windows, its accessible root must have a container
  role, and every visible element must have a name, a role in the admitted set
  (static text, push button, dropdown/menu button, link), no accessible subtree of its
  own, and -- if the projection would draw it as actionable -- a nonempty
  `accDefaultAction`. An element with an action may not be drawn as inert text, and an
  element without one may not be drawn as actionable.
- **An action runs the element's own default action.** `islandInvoke` names an item
  index; the source thread re-reads the island, refuses if the name or action string at
  that index changed, and then calls `accDoDefaultAction`. It is *posted* rather than
  performed inside the command, because a provider's default action may open a menu
  and spin a modal loop -- the same reason `WM_COMMAND` and `BM_CLICK` are posted.
- A dropdown element draws the chevron the native element draws, so the projection
  never promises an in-place result for something that opens a menu.

#### The toolbar surface, completed

Reaching MMC's real toolbars turned eight successive fail-closed refusals into
capabilities. Each was a genuine gap, and the accessible-object route unlocked the
first one:

- **Icon-only buttons.** A button with no text still publishes the name a screen
  reader reads -- normally its tooltip -- through the control's own accessible object.
  `AccessibleChildName` reads it, so an icon-only toolbar projects with a real name
  instead of being refused for having no label.
- **Latched buttons.** `TBSTATE_CHECKED` is carried, whether the latch belongs to a
  `BTNS_CHECK` button or to an application that manages it on an ordinary button.
  The projection posts the same `WM_COMMAND` and shows whatever the next capture
  reports, so the control keeps ownership of the state.
- **Dropdown buttons.** `BTNS_DROPDOWN` and `BTNS_WHOLEDROPDOWN` are admitted and
  draw their arrow.
- **Per-button image lists.** A toolbar may own several; a button's `iBitmap` then
  carries the list id in its high word. A toolbar that owns no list for that id draws
  no icon at all no matter what `iBitmap` says, which is what makes a text-only
  toolbar projectable.
- **Callback images.** `I_IMAGECALLBACK` asks the owner for an index at draw time, so
  the projection asks the same question through `TBN_GETDISPINFO` -- immediate parent
  and frame, wide and ANSI.
- **Custom-drawn faces.** An owner that answers no index draws the face itself, so the
  face is reproduced from the control's own paint: one `PrintWindow` of the toolbar
  cropped per button rectangle, bounded by the same image cap and part of the snapshot
  fingerprint. `PaintedClientSurface` is shared with container chrome. A button that
  carries its own label is the exception -- its label *is* its face, and a cropped
  bitmap of text would only be a worse copy of it. That is what makes MMC's menu bar
  project as real Fluent buttons rather than as strips of pixels.
- **Transient and cosmetic button states.** `TBSTATE_PRESSED` is the look of a button
  under the pointer right now, `TBSTATE_MARKED` an application highlight the control
  paints, and `TBSTATE_ELLIPSES` only says the control trimmed the label it already
  reported. None of them changes what the button does, so none is a reason to refuse
  the window.
- **`dwData` is no longer a rejection.** It is application-private storage the control
  never interprets; the projection posts the same `WM_COMMAND` a real click produces
  and the application looks up its own data exactly as it always does. Owner-draw is
  still refused where it is actually declared, in the control's style bits.
- A status bar's empty child window (a placeholder for text the application sets
  later, or a progress bar it keeps hidden) is absorbed, because a status bar's text
  lives in its own parts. One carrying text is still refused: that would be content.

#### Where `mmc.exe` stands

**An empty console projects.** Every node of `mmc.exe` with no snap-in passes capture,
renderer admission, and both UIA gates, and the surface stays projected: the menu bar's
toolbar as real Fluent buttons, the icon toolbar with its own icons, the tree with its
folder icon, the report list with its header, both splitters, the Actions pane island,
and the status bar. Toggling *Show/Hide Console Tree* on the projected toolbar hides the
native tree pane and the projection follows on the next reconcile; invoking an Actions
pane element runs the provider's own default action.

Four defects had to be fixed on the way there, each of which had been making the whole
window fall back to native:

- **The renderer's parent rule listed only dialog and MDI containers.** A private
  container pane could be captured but never admitted, so MMC's view window faulted the
  surface. Both sides now agree on one list, and the Bridge refuses a node nested inside
  a non-container at capture time -- with the parent's kind in the evidence -- instead of
  emitting a snapshot the renderer must reject. A `Static` used as a panel, a common
  Win32 idiom, is reclassified geometrically so the children it frames have a container
  to live in.
- **`GetBoundingRectangleCore` overrides inferred their scale from the visual.** A
  ContentControl whose visual and layout slot disagree made the peer report DIPs where
  the gate compares physical pixels (1262x23 against 1896x35 at 150%). The scale now
  comes from `XamlRoot.RasterizationScale`, which is what the platform actually uses to
  convert.
- **`AppBarSeparator` publishes no automation peer.** Outside a CommandBar a bare one
  simply vanishes from the accessibility tree, so a toolbar's separators stopped
  existing for a screen reader and the gate counted six children where the control had
  nine. Separators are now hosted in a control that owns a Separator peer.
- **Escape closed windows that never close on Escape.** `canCancel` was hard-coded true
  for every top-level surface; it is a dialog-manager behaviour, so it is now reported
  only for a real dialog class.

#### A revision race is not a refusal

A node property action carries the revision it was composed against, and the Bridge
answers `stale` when reconcile has moved on in between. That is a statement about the
snapshot, not about the request, and treating it as a refusal silently dropped
interactions -- a tree expansion that landed within the roughly one-per-second reconcile
window simply did nothing.

Such an action is now re-sent once. The rule is not which control it belongs to but what
the action means: `itemExpanded`, `checked`, `selectedIndex`, `selectedIndices`, and
`checkedIndices` each carry the absolute state the user asked for, so re-sending one
against a newer revision converges on that state no matter what the reconcile in between
observed. A one-shot command would be performed twice and `text` would overwrite the
normalization the application owns, so neither is ever replayed; geometry is absent for a
different reason, since request-semantic actions are rebased and never reported stale.
The replay waits for the patch that carries the revision which made it stale, because the
Bridge sends the rejection first and the revision second -- emitting it earlier would earn
the same refusal. `NodeActionReplayPolicy` holds both the list and the single-retry
ceiling.

#### What the first projected console got wrong

Projecting is not the same as projecting *well*. The first console that passed both gates
still looked and behaved wrong, and each defect had a distinct cause:

- **The splitters were invisible and unhittable.** A `ContentControl` whose only visual
  is a `Background` brush renders nothing WinUI considers on screen, so XAML reported an
  empty bounding rectangle and there was nothing to grab. The divider is now a child
  element the width of the native gap, inside an invisible grab strip, and it reports
  real bounds (`3x823` and `5x823` on MMC's two seams).
- **A splitter was offered where the projection draws no panes.** The pane surface was
  computed from *visible* child windows, but the projection absorbs some of them -- MMC's
  relocated MDI caption cluster in the rebar band is one -- so the container advertised a
  seam between a toolbar band and a window the renderer never draws. The pane surface now
  excludes exactly what capture skips, so the pane layout and the node tree agree.
- **Unpainted areas of a chrome band travelled as opaque black.** GDI carries no alpha,
  so one `PrintWindow` cannot say whether a pixel is black because the window painted it
  black or because the window painted nothing. `PaintedClientSurface` now renders the
  same paint twice, over black and over white: identical pixels are opaque, and the
  difference between the two is how much background showed through. The black render is
  already the premultiplied colour, which is the format the projection publishes, so a
  region the window never painted is simply transparent. MMC's "Actions" caption no
  longer drags a black rectangle across the rest of the pane.
- **Toolbar labels were sliced mid-glyph.** The native control sized each button by
  measuring its label with GDI, and WinUI's text metrics are wider for the same string at
  the same point size -- 5% to 20% on MMC's menu bar. The projection may not move the
  button, so the label is fitted into the width the control committed to, down to a
  floor, and only then trimmed.
- **Toolbar separators did not exist.** `AppBarSeparator`'s theme margin collapses its
  rule to nothing inside a strip as narrow as a native separator gap, and it publishes no
  automation peer of its own. The line is drawn directly and the wrapper supplies the
  Separator peer, so three separators now report three distinct rectangles instead of one
  shared bogus one.

#### Still open on `mmc.exe`

**The menu bar as a real WinUI menu is in progress.** Neither leaving a native popup on
screen nor refusing the bar is acceptable, so the menu itself is projected. Three facts
were established by measurement, and they fix the shape of the work:

- **The control declares them as menus, not buttons.** MMC's menu-bar `ToolbarWindow32`
  publishes every button with the accessibility role of a menu item and an Invoke action,
  and invoking one really does make MMC open its menu. `IsMenuBarToolbar` therefore asks
  the control what it is instead of matching a class name or a style bit, and the click
  is performed through the control's own accessible default action rather than a posted
  `WM_COMMAND` -- which MMC ignores, and which is why the projected bar did nothing.
- **The popup is a real HMENU.** The menu MMC opens is a `#32768` window over a live
  HMENU with readable items, so it can be captured with the same code the projection
  already uses for a window's own menu bar (`CaptureMenuHandle`) and rendered by the
  renderer's existing real XAML menu. `TrackPopupMenu` and `TrackPopupMenuEx` are now
  intercepted: while interception is armed for a thread, the hook records the HMENU and
  answers as if the user had dismissed the menu, so the application's popup never reaches
  the screen.
- **The read cannot happen inside the capture pass.** MMC opens the popup from its own
  message loop rather than from inside the accessible default action, so a bracket that
  returns before the loop runs records nothing. Driving it while the native window still
  owns the foreground also costs the proxy the foreground slot the committed gate
  requires -- measured as `proxy did not take the native foreground slot`. The read is
  therefore a staged source-thread sequence (`SourceThreadAgent::ReadMenuBarToolbar`,
  commands `kCommandMenuBarDrive`/`kCommandMenuBarRead`) that runs once per surface from
  reconcile, after the commit and cloak: arm interception and drive one button, let the
  application's loop run, then collect. `RendererSession::ReadMenuBarToolbarOnce` drives
  it and recaptures so the menu reaches the renderer in the same tick.
- **The popup must be captured inside the hook, not collected as a handle.** The
  application frees the menu as soon as the tracking call answers, so a handle collected
  160 ms later is no longer a menu -- measured as `menu handle is not a menu`. The hook
  now captures the items while the popup is still alive and the staged read collects the
  snapshot.
- **A menu bar's leading document-icon button opens the window's system menu.** Its items
  are `SC_*` commands, which need `WM_SYSCOMMAND` and which the projection already draws
  on the MDI child's own Fluent caption, so that button is skipped rather than refused --
  the same reasoning as the relocated caption cluster and the MDI bitmap menu items.
- Menu-item admission was too strict for a real application menu: an icon beside a label
  is decoration on top of the label, and `dwItemData` is application-private storage the
  menu never interprets. A string item that also carries a bitmap or item data is now
  projected from its string. What is still refused is an item with no readable label at
  all -- owner-draw, or a bitmap standing in place of text -- and a break that changes the
  menu's shape. Both refusals now name the item and its type or command id.
- Two regressions this lane introduced, both found from screenshots and both fixed. The
  first: interception was armed per button, so a popup that arrived after a button was
  given up on reached the screen -- a native menu over the projection, which is the one
  thing this lane exists to prevent. Suppression is now process-wide and spans the whole
  read. The second was worse: `CaptureWindow` assigned the toolbar-derived menu into
  `snapshot.menu` unconditionally, which wiped the HMENU menu bar of *every* ordinary
  window -- Phone Dialer projected with no File/Edit/Tools/Help at all. A menu read from a
  toolbar may only fill that field when the window owns no menu of its own.
- A `Static`'s label carries the same `&` mnemonic markup a Button's does. Every other
  kind already ran it through `Win32MnemonicTextConverter`; Static did not, so labels
  projected as `&Number to dial:` and were wider than the slot the control sized for them.

**Where it stands: MMC's menu bar projects as a real WinUI menu.** File / Action / View /
Favorites / SnapinMenu / Help appear as a real XAML `MenuBar`, and opening one shows a real
dark Fluent `MenuFlyout` -- Help lists "Help on Snap-in", "Help Topics", "TechCenter Web
Site", "About Microsoft Management Console...", "About Snap-in". No native popup reaches the
screen at any point (verified as 0 visible `#32768` windows), and the toolbar node is gone
from the projection because the bar is now the surface's menu.

Three more measurements were needed to get there, each of which had a different cause than
the one before:

- **The wait had to be a pump, not a delay.** The application's own message loop is what
  opens the popup, so the read drives the button and then pumps that queue until the hook
  reports a popup. Bridge commands arriving during the pump are requeued rather than
  dispatched re-entrantly (`SourceThreadAgent::DeferringCommands`).
- **comctl32 will not enter menu mode for a window that is not active.** With the
  projection committed the proxy owns the foreground, so the read makes the cloaked native
  window active for the length of one popup and hands the foreground straight back. Nothing
  of that is visible: the window is cloaked and every popup is swallowed.
- **A top-level menu can legitimately open nothing.** MMC's Window slot on a console with
  no snap-in windows opens no popup at all. That is a menu with nothing in it right now,
  not a failed read, so it is projected with its title and left disabled -- which is what
  the native bar shows. `ProtocolValidator` admits an empty popup only while it is
  disabled: an enabled menu the user can open onto nothing would mean the projection lost
  the application's content.

- The "Actions" band MMC paints includes the pane title the accessible island also
  projects as its first item, so that one string appears twice.
- The menu bar's leading System button is 16 native pixels wide and carries the label
  "System", so the fitted label still trims to a stub where the native control draws a
  document icon.

#### MMC menu lifecycle and interaction follow-up

The toolbar-derived menu is now refreshed after semantic changes instead of being
frozen at injection. Menu and toolbar commands, selection changes, and MDI changes
invalidate it. Capture observes the toolbar's HWND generation, labels and enabled
state without opening a popup; removal, recreation or relabelling discards stale
identities. A quiet console reuses its menu. Refresh is debounced and failed reads
have at most three attempts per invalidation.

`SourceThreadAgent::RefreshMenuBarToolbar` queues the complete operation onto the
owning GUI thread, including discovery, MSAA reads, popup capture, cleanup and
publication. The pump checks a total deadline while draining messages, defers Bridge
commands and preserves `WM_QUIT`. Intercepted HMENUs are copied before the application
destroys them. Late popup callbacks from a previously read owner remain suppressed
while that native root is cloaked; restoring it restores native popup behavior.

A pending or failed refresh cannot dispatch old commands. Disabled menu ancestors
also disable their commands, and a stale menu click is rejected without restoring
the whole surface. Nested WinUI menus now track name, access key, enabled and default
state changes. Icon toolbar buttons expose the native accessible name as a tooltip.

The committed isolation gate checks the exact native process/HWND among desktop
top-level UIA children, after confirming the HWND is top-level and application-cloaked.
The former desktop descendant scan also walked unrelated applications' content and
took about 12 seconds on the acceptance desktop, exceeding the 5-second gate budget.
Gate timeouts now report the stage and node being checked.

Native regression coverage uses a real toolbar HWND with a test MSAA provider that
posts and destroys popup menus. It covers menu state refresh, toolbar identity,
disabled ancestors, cancellation with a queued popup, preserved `WM_QUIT`, bounded
retry, and a message stream that cannot extend the total deadline.

The same MMC soak exposed a chrome capture bug with channel-dependent GDI text:
the black/white reconstruction used the least covered RGB channel as scalar alpha,
which could put another color channel above alpha and fault the renderer. It now
uses the greatest coverage, preserving black-pass color and transparent unpainted
pixels while guaranteeing premultiplied BGRA. Regression cases exercise different
coverage per channel, changing paint, and transparent/opaque pixels; the real HWND
capture test also checks every captured chrome pixel against the wire invariant.

Validated on 2026-09-26 with Release builds: the 252 renderer tests, native suite,
and production gates pass. Empty MMC reaches interactive commit, both pane toolbar
buttons toggle their native panes, menu refresh follows each toggle, and native-name
tooltips appear. After the alpha fix it remained projected for more than two minutes;
`LegacyDialogHost` also reached interactive commit. Terminating MMC's renderer
restored the complete native console. This validates the empty-console
path, not arbitrary snap-in controls or every Actions-pane popup.

#### Actions-pane popup projection

Accessible-island dropdown actions now have a separate, live popup route. An
`islandInvoke` records its HWND generation, item index, name, and default action on
the source GUI thread. The matching `TrackPopupMenu`/`TrackPopupMenuEx` call keeps
its native HMENU alive while its items travel in optional `window.popupMenu`
state. The renderer opens a real WinUI `MenuFlyout` at the projected island item.
Ordinary snapshot updates preserve the flyout and its native tracking token.

Protocol minor 20 adds the popup token, anchor, items, and `popupCommand`. A choice
names the token and item path; a null path dismisses the menu. The source thread
revalidates the selected command against the still-live menu, including enabled
ancestors, before returning its ID for `TPM_RETURNCMD` or posting `WM_COMMAND` to
the actual tracking owner. Stale tokens are refused. WinUI sends one terminal
reply even when Click and Closed both occur. A leaf's empty child array does not
consume an extra level of the menu depth bound.

The native modal pump continues servicing capture, selection, and restore
commands. Toolbar-menu refresh pauses during tracking. Temporarily disabling the
opener does not discard its pending request; changing the anchor identity,
moving/minimizing/closing the frame, or restoring the native surface cancels it.
An unsupported popup capture faults the surface through the normal supervisor
rollback path. Owner-draw, modeless, drag/drop, and position-notification menus
retain the existing capture boundary.

Validation on 2026-09-27 includes 296 renderer tests and real HWND/source-thread
integration tests for asynchronous opening, disabled openers, nested selection,
actual-owner command routing, HMENU lifetime, dismissal, geometry changes, and
whole-surface restore. These tests cover the implementation used by MMC's More
Actions route; a manual click-through of this new route in MMC remains pending.

### Tranche I: Private Containers, Splitters, And Reorderable Headers

The blockers that kept `mmc.exe` native were mostly *containers*, not controls: MMC's
view window, its Actions-pane host, and its rebar band are all private MFC classes.
A class-name whitelist would have been a dead end, so the boundary for a container is
geometric.

- **A container is admitted by what it can still be hiding, not by its name.**
  `ProbePaneContainer` runs for any class the registry does not know: the window must
  be a child, must not scroll, must not composite or clip itself with a region, must
  own no menu, and must have at least one visible child. Its visible children are
  subtracted from its client area, and every rectangle left over is classified: a
  strip no thicker than `kMaxPaneGap` is a gap between panes, and anything thicker is
  a band the container paints itself. Window text is not a rejection: a container
  whose children tile it has nowhere to paint text, so the text becomes the pane's
  automation name.
- **A band the container paints itself is reproduced, not dropped.** Each such
  rectangle travels as a chrome region: the rectangle plus the premultiplied BGRA
  pixels the native window drew there, captured with one `PrintWindow` of the client
  area and cropped per band. The pixels are part of the snapshot fingerprint, so a
  repaint reaches the renderer as an ordinary patch. A container with more than
  `kMaxChromeRegions` bands, a band wider or taller than
  `kMaxChromeRegionDimension`, or more than `kMaxChromeRegionBytes` of them in total
  is refused with the offending rectangle as evidence -- that cap is the line between
  a frame and a custom-drawn control wearing a container's shape. UIA exposes no
  element for such a band natively, so the projected image loses nothing.
- **A strip that divides the container becomes a real splitter.** A gap qualifies
  only when the two children touching it cover the same extent across the other axis
  as the gap itself, which is what makes "drag this strip" mean "resize exactly these
  two panes". The seam between two toolbar bands of different widths fails that test
  and is never offered; a splitter sitting under a caption band the container paints
  still passes, because the band is chrome rather than a pane. Each split travels
  with its position, thickness, and the range bounded by the two panes' far edges.
- **Dragging a projected splitter resizes the two native panes.** `setSplit` is a
  request-semantic action: the source thread resolves the split against the
  container's live geometry and moves exactly the two neighbours with `SetWindowPos`,
  so the native layout stays canonical and the next capture reports what the
  application settled on. Posting a synthetic mouse drag was tried first and
  rejected: MMC's splitter tracking answered one drag and then ignored the rest,
  because a tracking loop reads the real cursor and the projection must not move it.

  A container also keeps its own proportion in private data that no message writes, so
  the next `WM_SIZE` re-layout would put the split back where the application last knew
  it. The split the user asked for is therefore remembered against the extent it was
  measured on, and asserted again -- through the same `SetWindowPos` contract -- when
  reconcile observes the container at a different extent. The guards are what keep this
  from overriding the application rather than compensating for a write it cannot make:
  nothing happens while the extent is unchanged, so a split the application or the user
  moved at a fixed size stands; nothing happens when the re-layout already lands on the
  remembered proportion; and a container that refuses the proportion twice keeps its own
  layout for good. Measured on MMC: dragging the console tree to 29% of the view window
  and then resizing four times holds 0.289-0.291, with exactly one re-assertion per
  resize and none at a steady size.
- **An MDI frame's relocated caption cluster is absorbed.** When a child is
  maximized the frame hosts that child's minimize/restore/close buttons itself; the
  projection already draws them on the child's own Fluent caption, so a textless
  leaf no larger than three caption buttons in the frame band, with a maximized
  active MDI child to belong to, is skipped like the MDI chrome menu items.
- **`LVS_EX_HEADERDRAGDROP` is projected instead of refused.** The list carries its
  column display order as a permutation of the logical indexes, so columns, widths,
  cells, and every index on the wire keep the application's own meaning while the
  projection presents them in header order. Dragging a projected header cell emits
  `setColumnOrder`, which the Bridge applies through `LVM_SETCOLUMNORDERARRAY` -- the
  same state the native header writes.
- Toolbars may now carry `CCS_NODIVIDER`, `CCS_NORESIZE`, `TBSTYLE_LIST`,
  `WS_EX_TOOLWINDOW`, and `WS_EX_NOPARENTNOTIFY`: none of them changes what the
  control paints or how its buttons behave.
- Capture now logs *every* rejection in the tree rather than only the first, because
  enumerating a target's blockers one rebuild at a time is what made this tranche
  slow to plan.

#### Where Tranche I left `mmc.exe`

With the container, splitter, chrome, and header work in place, the remaining blocker
was `DirectUIHWND`: it hosts the Actions pane's content as HWND-less elements, so the
geometric container rule correctly reported it had no visible children to frame.
Tranche J admits it.

### Tranche K: Windows Forms Result Panes And Large Virtual Lists (2026-10-08)

MMC 3.0 snap-ins (Event Viewer, Task Scheduler, Windows Firewall, TPM) host Windows
Forms views, and Event Viewer's logs are owner-data lists of thousands of rows. Each
adapter below is admitted by evidence the control publishes, never by class name alone.

- **A form-like panel is a frame when its own paint is only the system surface.**
  `ProbePaneContainer` also admits a container whose visible client area, minus its
  children, is one opaque colour equal to `COLOR_3DFACE` or `COLOR_WINDOW` -- the
  surface the projection already draws behind every control -- and one whose children
  are all hidden. The check renders the container twice (over black and over white, as
  the chrome capture does) and compares only what is on screen. Such a frame carries no
  chrome and offers no splits, because a gap between two fields is nothing the user can
  drag natively. An empty tab stop is not a frame: it is a control that has not painted.
- **Windows Forms controls are painted through `WM_PRINT`.** They draw in `OnPrint`, so
  their pixels come straight from the window in milliseconds instead of a compositor
  round trip per render (Event Viewer's overview capture fell from about 2 s to about
  190 ms). Any window that answers `WM_PRINT` with nothing is read from the compositor.
- **Read-only RichEdit projects as a read-only text box** when the text object model
  reports one size and colour, no emphasis, hidden, raised or lowered run, no link and
  no embedded object. CJK/Latin font binding is not formatting. An editable RichEdit
  would lose its formatting through a plain-text edit and keeps its refusal.
- **A LinkLabel is a one-link SysLink.** Its accessible object publishes the link as a
  child (`ROLE_SYSTEM_LINK`) with a default action; the label is told from a plain
  Label by those children. Activation is the link's own `accDoDefaultAction`, which
  raises `LinkClicked`, on the deferred native-action path because the handler may
  open a dialog.
- **Symbol-font text is glyphs.** A Windows Forms label or button in Marlett, Webdings,
  Wingdings or a Segoe icon font, and a textless picture button, carry their painted
  face as an image with the face background keyed out, instead of projecting the
  letters (`5` for a chevron, `r` for a close cross).
- **An actionless focusable strip is its pixels.** A tab-stop `WindowsForms10.Window.*`
  at most 32 px tall that has no children and publishes no accessible action (Windows
  Firewall's overview section headers) is an inert painted surface and leaves keyboard
  traversal.
- **A window another covers but for its edges is not on screen.** MMC lays Print
  Management's result ListView over the HTML host that positions it. The host and its
  content are left out of the capture, and unsupported content inside one drops the
  host rather than refusing the frame.
- **MMC's busy shield is input semantics, not content.** The transparent
  `MMCWaitCursorWindow` (mmcndmgr.dll) is absorbed and the session declines everything
  but the window frame's commands while it is visible.
- **A `res://` image of a System32 module is decoded from its resource.** The image is
  placed at its own layout box inside the item's box, so it is exact whatever the scroll
  position, the cloak or the host's print path; the document's own paint is read back
  only for an image that is not such a resource.
- **Large virtual report lists are windowed.** An owner-data report ListView of more
  than 512 rows without checkboxes or editable labels is carried as 192 rows around its
  native top row (`itemCount`, `itemWindowStart`; protocol minor 33). Indices stay
  absolute on the wire and are bounded by the full count; the projection draws the
  window between two spacers that keep the whole list's extent and moves it with
  `setTopIndex`, which scrolls the native list so the requested row is its top row. One
  request is in flight at a time, the viewport is checked again after each window
  arrives, the scroll offset is restored across the rebuild, and a request that does not
  land never rolls the window back.

### Tranche L: MMC Menus, Selected-Row Views, And Item Activation (2026-10-08)

The console's own chrome and result panes: what MMC hides, what its HTML views mix, and
how an item is opened from the keyboard or by double-click.

- **The MSAA menu bar skips what MMC hides.** MMC's menu bar is a `ToolbarWindow32` that
  publishes its buttons through MSAA; a menu that does not apply to the current view keeps
  its button with `STATE_SYSTEM_INVISIBLE`. Capture reads that state, leaves hidden buttons
  out and refuses to select one, so the projected bar shows what the native bar shows.
- **System commands are `SC_*` only.** MFC numbers an MDI Window menu's items from 0xFF00,
  which an "above 0xF000" test took for system commands and refused as not `WM_COMMAND`.
  Only 0xF000-0xF1FF are system commands (`IsSystemCommandId`, shared by the menu-bar and
  popup-level captures); every other id is an ordinary command.
- **A selected row's extended view mixes elements and loose text.** The Services
  `views.htm` view puts a script link ("Start") beside bare text nodes. Text-node runs are
  measured through `IHTMLTxtRange` (`getBoundingClientRect`), bounded by their neighbouring
  elements, and the document is compared as a multiset whenever any exist. The
  `FolderIcon` and `MMCEvents` objects MMC injects into these views are inert and skipped.
- **A counter collection is read through `Item(i)`, then `_NewEnum`.** sysmon's counter
  items refuse `QueryInterface(IID_IDispatch)`, so the VARIANT's own `IDispatch` is used,
  and a failed read names the step that failed. Performance Monitor's root and Computer
  Management's Performance node project.
- **A context menu may need the Menu key.** Device Manager's result tree is an MFC OCX
  that ignores a synthesized `WM_CONTEXTMENU(-1,-1)` but opens its menu from `VK_APPS`.
  `ContextMenuTrigger` names how a request is delivered (context-menu message, Menu key,
  Enter key); a TreeView hosted in an OCX view uses the Menu key.
- **A popup another thread creates is claimed, not left native.** Event Viewer's list runs
  on its own GUI thread and the frame thread creates the popup asynchronously. The frame's
  agent arms a pending island menu for the request and the remote popup is marked claimed,
  so the popup is tracked and projected instead of retired as a stray.
- **A default activation is a request, not a guess (protocol minor 34).** A virtual MMC
  result list publishes no native activation, and a snap-in tree's leaf opens on Enter.
  Inside an MMC frame such a node offers `itemDefaultActivation` (never the scope tree,
  whose double-click expands). The projection sends `openItem { index, text }` with the
  absolute index and the label as witness; the Bridge selects that item and posts its
  control Enter (`WM_KEYDOWN`/`WM_KEYUP`, `VK_RETURN`) on the deferred native-action path,
  because the handler may enter a modal loop. `openItem` is request-semantic (rebased onto
  the current revision like `invoke`), and a refused one is recoverable.
- **The projection recognizes the gesture itself.** A double-click is counted from raw
  presses at window level (`ItemPressTracker`): the first press can rebuild the item
  presenter and even replace the node's view model, which cancels a double-tap the
  presenter had recognized. Enter is heard among handled events, because the row
  container consumes it as its own invoke before it reaches the list or the tree. The
  list's listener serves native `activateItem` on Enter as well.

### Tranche M: Snap-in Dialogs, Windows Forms Forms And Rich Text (2026-10-09)

What a survey of the dialogs MMC opens turned up (each fixed row names the evidence).

- **A Windows Forms list opens its item from whichever input its application listens to.**
  Task Scheduler's task list answers the accessible default action with success and opens
  nothing, and ignores Enter; its application listens to `DoubleClick`. Event Viewer's event
  list is the reverse: it opens on Enter and ignores a synthesized click (the application
  reads the pointer, which is not where the projection's request is). A Windows Forms
  ListView inside an MMC frame therefore offers `itemDefaultActivation` instead of
  `itemActivationSupported`, and `openItem` sends Enter first (it names the selected item
  exactly) and, only when that opened no captioned window within a modal call's duration
  (300 ms) plus a short grace (250 ms, messages dispatched), gives the control
  `WM_LBUTTONDBLCLK` and `WM_LBUTTONUP` at the item's label. Every other control keeps the
  Enter key. A refusal or a request that never reaches the list's thread is logged
  (`Remote island action refused / running / did not run`), which used to be silent.
- **Focus that already landed is not moved back.** The tab control and the tree each pulled
  keyboard focus to their selected header or item on every `GotFocus`, and that event
  bubbles from everything inside. Clicking an edit on a property page left focus on the
  selected tab header, so no edit on any tab page took typing; clicking a tree item
  focused the previous selection and scrolled the tree to it, so a double-click's second
  press met another row. Only keyboard focus that lands on a header (or the tab control)
  and focus that lands on the tree itself are redirected.
- **Opening a tree node does not rebuild the tree.** An `itemExpanded` patch rebuilt every row
  and reset the scroll offset to the top; a change of icons did the same. When labels, depths
  and child flags are unchanged the nodes' open state and icons are applied in place, and a
  rebuild that remains restores the vertical offset once layout has made the extent tall
  enough again.
- **A default minimum never exceeds the native extent.** WinUI's `CheckBox` is 120 px wide at
  least, so a 65 px "Hidden" box reported bounds the native control did not have and the
  committed UIA gate refused the whole dialog. `ApplyBounds` clears a `MinWidth`/`MinHeight`
  larger than the extent it has just assigned.
- **A split runs across its container.** The strip between a form's label and its field has a
  neighbour on both sides and was offered as a draggable splitter. A split must reach from
  one edge (or a painted caption band) to the other and cover at least half the container's
  extent; capture and `SetPaneSplit` read the same `ResolveSplits` list, so indices agree.
- **A band that is only the surface is nothing to carry.** A painted band that is one uniform
  system surface colour -- the dialog face, the window background, or a themed tab body's
  neutral near-white (#F9F9F9 under the Windows 11 theme) -- is the background the projection
  already draws behind every control. It is not captured as chrome (it showed as a slightly
  different patch on the projection's own surface), and one wider than the chrome caps no
  longer refuses a frame that has nothing custom in it.
- **A glyph button shows its face.** An owner-draw Windows Forms button answers `WM_GETFONT`
  with nothing, so a symbol font ("67" for a drop-down arrow, Marlett "5"/"6" for up and down)
  is invisible to the font test. A button no larger than 36 px with at most three characters,
  and a standard push button in a symbol font, carry their painted face as a picture; a face
  that does not render leaves a standard button as a button with its characters.
- **A snap-in popup's owner is replaced, not remembered.** Event Viewer's Action and View
  menus are owned by a hidden `ATL:` top-level window MMC creates for each tracking call and
  destroys after it, so the owner recorded at read time is gone when a command is replayed.
  A recorded owner outside the frame is told by its class, and the replayed popup is accepted
  with a new owner of that class on the same thread. A stale refusal names the check that
  failed.
- **An unowned modal dialog is modal to the window disabled for it.** `Form.ShowDialog()`
  without an owner is parented to the desktop and disables the thread's other top-level
  windows. The desktop is no owner; a dialog-framed window resolves the disabled top-level
  nearest below it on its thread as its owner, so the proxy blocks it like a real owner.
- **Formatted read-only text is carried as runs (protocol minor 35).** A read-only RichEdit
  with more than one character format (a certificate's bold labels and bullets) is the
  `richText` kind: paragraphs of runs read through the text object model, each run one
  constant format (bold, italic, underline, strike-through, size in points, colour), with
  paragraph alignment, a bullet, indents and spacing. The projection is a scroller around a
  text block and reports as a pane. Links, embedded objects, hidden, raised or lowered
  text, background colours and numbered lists refuse the control.
- **A Windows Forms NumericUpDown is an UpDown beside its edit.** `UpDownBase.UpDownButtons`
  is a small leaf that paints two arrows: an accessible spin box with an up and a down push
  button that publish no action, stepping when its upper or lower half is pressed. It
  projects as an `upDown` whose position is the integer in the sibling edit and whose range
  is open; a step sends the control the press a pointer gives. A number that is not an
  integer refuses the control.
- **A Panel's 1-pixel rules are inert pictures.** A Panel answers as an accessible grouping,
  and a rule a few pixels thick may run the whole height of a page (the divider a wizard draws
  beside its page), which the box cap would have refused.

### Tranche N: Performance Monitor's Chart (2026-10-09)

The chart view of Performance Monitor (perfmon.msc, Monitoring Tools) was the last node of the
navigation census that stayed native. It hosts the System Monitor control (sysmon.ocx): a
toolbar, the graph (`ChartGraphClass`, around an ATL window of wvc.dll that draws it), a value
strip (`PerfStatus`) and an owner-draw legend (`PerfLegend` around a `SysListView32`). Pixels of
the graph say nothing about the counters behind them, so the contract is the control's own
state: three kinds, `performanceGraph`, `performanceLegend` and `performanceValueBar` (protocol
minor 36), each probed by class *and* by identity (the class is registered by System32's
sysmon.ocx, the part sits directly in a `SysmonCtrl` whose object answers `IPersist` with the
System Monitor CLSID and whose methods are code in sysmon.ocx).

- **The control is read through its vtable, never its IDispatch.** Its automation dispatch
  resolves names through one process-wide `ITypeLib` pointer that sysmon.ocx clears when *any*
  instance is destroyed, and MMC destroys the previous view's instance (the Performance page's
  embedded counter report) moments after it creates the chart's: from then on
  `GetIDsOfNames` answers `E_UNEXPECTED` and `Invoke` `E_FAIL` for every instance in the
  process while the control itself keeps working. The vtable interfaces (`ISystemMonitor`,
  `ISystemMonitor2`, `ICounterItem`, `ICounterItem2`) never touch that pointer, so
  `SysmonInterfaces.h` declares them by hand in the type library's slot order
  (`tools\Dump-TypeLib.ps1 C:\Windows\System32\sysmon.ocx` prints it) and `Sysmon.cpp` calls
  them under a structured-exception guard. The control's interface pointer is the OLE drop
  target it registers on its window (`OleDropTargetInterface`), queried for `ISystemMonitor2`.
- **A live chart's history is recorded from the control's own event.** `GetDataAt` answers only
  for log and SQL sources (it returns `E_UNEXPECTED` when the data source type is not 2 or 3,
  checked in the disassembly), so there is no history to read. The control does publish
  `DISystemMonitorEvents::OnSampleCollected` once per sample; the Bridge advises a sink on that
  connection point (the control owns the subscription and drops it when destroyed) and, for each
  event, keeps every counter's `GetValue` -- the values the control plots -- with the time. The
  history starts when the part is first probed, a moment after the view appears, which is when
  the native chart starts too. A sink that fires after its window is gone does nothing.
- **Slots are laid out as the control lays them out (compared against a native chart).** A
  wrapping chart (the default) writes sample n into slot n mod N with a red time bar at the
  newest sample and leaves the previous pass right of the bar until it is overwritten; the slot
  just after the bar is blank (the control clears the sample the next one overwrites), so the
  line has a one-slot gap there. A scrolling chart keeps the newest sample last. The graph
  carries one value (or null) per slot, the bar's slot, whether the chart has wrapped, and the
  times of the first and last slot's samples. Samples sit at the right end of their share of the
  plot's width, which is where the native chart draws them. The time axis labels the time of the
  sample each slot holds -- the first slot at the left edge, the last at the right edge, and a
  slot every nice number of seconds (a step at least 2.2 label widths wide) between -- so after a
  wrap the left label is newer than the interior and right ones, as it is natively. A histogram
  has no time axis and no time bar: its bars are the counters' latest values, each centred in its
  share of the plot (a lone counter's bar is the middle two fifths), and the legend's swatches
  are solid bars.
- **The value strip's labels come from the control.** Last, Average, Minimum, Maximum and
  Duration are string-table entries of sysmon.ocx (128 to 132, localized by its MUI file), the
  numbers are the selected counter's `GetValue` and `GetStatistics` in the user's number format
  (three decimals, digit grouping when the control groups), and the duration is the data point
  count times the interval. The legend's cells are the native list's own text (the control keeps
  the text of the columns it draws itself); its Show box and row selection are the list's own
  item state, set through the list so the control reacts as it does to a click and then checked
  against the counter.
- **Only what the projection can draw is admitted.** Current-activity data (not a log or SQL
  source), the line and histogram types (not report, area or stacked area), 2 to 1000 data
  points, at most 64 counters and an interval of 0.1 s to an hour. Anything else keeps the whole
  window native, so switching the chart to Report from the toolbar rolls the console back and
  switching to Line again projects it again.
- **Gaps between the control's children are margins, not splitters.** Its toolbar, graph, strip
  and legend are 10 px apart, which the container rule (strips of at most 14 px that span the
  container) took for three draggable splitters. `ResolveSplits` offers none for a System
  Monitor control.
- **A toolbar button's arrow is its own gesture.** `TBSTYLE_EX_DRAWDDARROWS` (the only extended
  style the control's toolbar sets) is accepted. A `BTNS_DROPDOWN` button projects as one button
  with two parts: its body is the button's command (here "change graph type", which cycles) and
  its arrow is the new `toolbarDropDown` action (the button's command id). The Bridge answers it
  the way the toolbar's own click does -- `WM_NOTIFY`/`TBN_DROPDOWN` with the button's id and
  rectangle sent to the toolbar's parent -- from a deferred source-thread request with popup
  interception armed, so the owner's `TrackPopupMenu` is carried as the same popup menu a
  context menu is, anchored under the button.

Not carried: log and SQL data sources, the report and area types, and the chart's own pointer
interactions (hover tooltips, double-click properties); a scrolling chart's layout follows the
documented behaviour and has not been compared against a native scrolling chart.

#### The Add Counters dialog (same day)

The chart's Add button opens the PDH counter browser: a dialog whose "Available counters" list is a
grouped report `SysListView32` (one collapsible group per counter set), whose instances list is a
multi-select ListBox, and whose "Added counters" list is a second grouped list. It crashed the
target at first (the whole of `mmc.exe` ended inside comctl32); what it needed:

- **A multi-select ListBox is an index list.** `LBS_EXTENDEDSEL` and `LBS_MULTIPLESEL` lists are
  admitted: the capture reads `LB_GETSELITEMS`, the renderer's ListBox takes the matching selection
  mode, and `setSelection` carries every selected index (a `LBN_SELCHANGE` is sent as a click would
  send it). Multi-column, owner-draw and virtual lists stay refused.
- **comctl32 6.10.26100 ends the application when a ListView group has no footer.** Its
  `LVM_GETGROUPINFO(BYINDEX)` copies the header, footer, subtitle and task text into the caller's
  buffer with an unchecked string copy, so a group inserted without a footer is a null read inside
  comctl32 (WER: `Comctl32.dll` offset 0x7D418, then `0xC000041D`). It looked like a hang
  because the faulting thread sat in WER. Group text is read one field per query through the
  class window procedure under a structured handler (an exception raised below `SendMessage`
  crosses a kernel callback and never reaches the sender's handler), and a null read is an
  absent text. `TestListViewGroupsWithoutText` pins it.
- **A positioned list builds only what its viewport shows.** A grouped (or icon) ListView is a
  Canvas holding every item of the native list wherever its scroll position puts it, and the XAML
  accessibility provider has to describe each element that exists: a couple of hundred group
  bands made one UIA transaction last longer than a gate waits (1.3 s a stage, 1 s allowed), and
  the dialog went back to native. Items within two viewports of the native viewport are built;
  a group's rows that the native list collapses (no extent) and bands far off are not (rows keep
  a collapsed placeholder in `control.Items`, which selection indexes; bands come after every
  row, so they are simply absent). Native scrolling recaptures the list, which builds the rest.
  A group band is one UIA element now (the button), not three. A list that indexes its realized
  rows (check boxes, in-place rename) keeps them all.
- **The UIA gate scales with the snapshot** (`BudgetFor`): 1 s per transaction and 5 s per pass for
  a small tree, more in proportion to controls, rows and group bands, up to 4 s and 14 s.
- **Escape and the close button reach dialogs that ignore `WM_CLOSE`.** The Bridge's `close`
  action sent `WM_CLOSE`, which this dialog neither handles nor turns into a cancel (posted or sent,
  it stays open). It now sends `WM_SYSCOMMAND`/`SC_CLOSE`, which is what the native close button
  sends: `DefWindowProc` turns it into the dialog's own cancel, and into `WM_CLOSE` for a window
  that handles that.

Verified live (disposable console, non-elevated): the dialog opens projected, a counter set expands
through UIA, "% Processor Time" is selected, `Add >>` puts it in the right list under its group,
`OK` closes the dialog and the legend gains the row; Cancel and Escape close it.

#### The chart's Properties dialog (same day)

The toolbar's Properties button (`Ctrl+Q`) opens a comctl32 property sheet: a `#32770` frame with a
tab control, OK / Cancel / Apply, and a DS_CONTROL host dialog holding the active page. The five
pages (General, Source, Data, Graph, Appearance) are sysmon.ocx dialog templates created as child
dialogs with `WS_CHILD | DS_SHELLFONT` and no `DS_CONTROL` (`tools\Dump-DialogResources.ps1
C:\Windows\System32\sysmon.ocx` lists every control of every page). What it needed, in the order the
log named it:

- **A child dialog that is no control parent is a navigation scope of its own.** Such a page runs a
  dialog manager of its own (Tab cycles inside it), so the frame's `GetNextDlgTabItem` walk never
  reaches its controls. `ProbeDialogContainer` admits any chrome-free child dialog (`DialogContainer`),
  `IsIndependentDialogPage` makes each visible one an extra tab container next to the MDI children, and
  the renderer's `ValidateDialogContainer` no longer demands the DS_CONTROL shape.
  `TestTabOrderScopes` pins it.
- **A container's tab order starts at its first tab stop.** The walk began after the container's first
  child, so a container whose first child was itself a tab stop numbered it last (a null start to
  `GetNextDlgTabItem` answers the first one). The MDI regression test had encoded the rotated order.
- **An owner-draw ComboBox whose owner is a dialog procedure.** A dialog procedure answers
  `WM_DRAWITEM` through `DWL_MSGRESULT`, which the sender reads as 0 even though the item was painted,
  so the capture treated every painted item as refused. Painting (a render that changed its fill) now
  counts as handling it. The Data page's colour, width and style combos paint their line previews this
  way.
- **An owner-draw button of a framework other than Windows Forms.** A push button whose owner paints
  its whole face is carried as that picture and clicks the control when pressed -- up to a button
  picture's size (96 px). A wider face that no key reaches (the Appearance page's colour swatch) is
  the inert picture it shows (`StaticBitmap`); a wider one that takes focus has no contract.
- **The Time Range bar** (`IntervalBar`, a window sysmon.ocx registered, on the Source page) is an
  inert painted picture like the Disk Management legend: the range of a log is not edited by
  dragging it in the projection.
- **A Static draws like DrawText.** Left, centred or right (`SS_LEFT/CENTER/RIGHT`), wrapped at its
  width unless `SS_SIMPLE`/`SS_LEFTNOWORDWRAP`, from the *top* of its rectangle unless
  `SS_CENTERIMAGE`, an ellipsis for `SS_*ELLIPSIS`, `&` shown as is under `SS_NOPREFIX`, and
  everything that does not fit clipped to the control's own rectangle (a label narrower than its text
  showed the middle of two lines). The text fills the control's slot rather than being centred in it,
  because the automation peer expands a centred text's rectangle symmetrically to report the
  control's bounds -- which moved a top-aligned message by half its height and failed the UIA gate.
- **Native-sized check boxes, radio buttons and combos keep their labels.** WinUI's 5 px of top
  padding and top-aligned label cut the text of a 13 to 16 px control (and a combo's text, 10 px of
  room in 22): a combo's padding is `8,0,0,0` and the label is centred.
- **Check boxes and radio buttons use compact templates** (`CompactControlStyles.cs`). WinUI's own
  template lays the glyph out in a 32 px cell and draws it 20 px wide, so a native control 13 to 16 px
  tall clipped it: a radio button showed as an arc. The compact `Style`s keep every state (pointer
  over, pressed, disabled, checked, indeterminate) and the Fluent look but draw a 16 px glyph centred
  on the label's line, with a 4 px gap. A label wraps only when the control is tall enough for two
  lines (`WrapWhenTall`): the capture does not carry `BS_MULTILINE`, and a one-line control clips at its
  edge as the native one does instead of dropping its last word onto a second line it cannot show. A
  style is built once, only if every theme resource it names exists in the application's dictionaries
  (a missing one fails later, inside layout); otherwise the control keeps WinUI's template and the
  renderer log says `compact ... style not used`.
- **Windows Forms dialogs close on Escape.** `canCancel` was true only for `#32770`, so Event
  Viewer's and Task Scheduler's dialogs ignored Escape; a WinForms form that is modal or has the dialog
  frame (`WS_EX_DLGMODALFRAME`, `FormBorderStyle.FixedDialog`) now cancels like its close button does.
  (MMC opens these unowned -- the owner chain is hidden -- so "modal" alone matched none of them.)
- **A check box or radio button answers a click anywhere in its rectangle.** WinUI's template has no
  background on its root, so a 567 px native radio with a short label answered only on its glyph and
  text; both now carry a transparent background.

Verified live: all five pages project with their controls (edits, check boxes, radio buttons, combos,
the counter list, the owner-draw previews) and every control answers real input; the Source page's
Database radio makes leaving the page raise the "no data source name" message box, which projects
too. Known gaps: a label a few pixels wider than its native control (a Chinese "(W)" accelerator on a
tight radio button) still loses the last pixel or two of its closing parenthesis. The Color chooser (the
Appearance page's Change button, nested over the sheet) stays native while open and the console and
sheet are projected again about three seconds after it closes (below). The Font chooser (three
`CBS_SIMPLE` combo boxes, which no adapter admits) and the log-file picker (the shell's item dialog) have
not been driven; by the registry's rules neither projects, and the same hand-back applies.

#### Dialogs opened from dialogs (owner chains)

The owner graph used to be one level deep: a dialog whose owner was itself owned (Add Counters or the
Color chooser opened from the chart's Properties sheet, which the console owns) was never projected, and
its appearance handed the whole graph back to native, where it stayed. `IsModalOwnedTopLevel` is now only
"the owner is disabled", so a chain of modal dialogs projects one dialog at a time, each proxy owned by the
proxy beneath it (the renderer already blocks whichever owner a modal window names). What the Bridge had to
learn to make a refusal survivable:

- **Order.** Discovery decides roots first, then every depth of owner in turn (a nested dialog is above
  its owner in the z-order it enumerates in). An owned window waits in `ownedWaiting_` without spending its
  one attempt until its direct owner is a projection, so a graph handed back to native is projected again
  from its root and its dialogs are offered after it (`ReleaseOwnedWindows`).
- **Hand-back.** A refused modal dialog (`refusedDialogs_`) hands every projected owner back to native
  together (`HandOwnerGraphToNative`; DWM cloaking is inherited down the owner chain). The roots wait in
  `ownerGraphDeferrals_`, a dialog owner gets its attempt back. Before this, the refusal of a *first-level*
  modal dialog left its owner native for good.
- **Hold.** A root stays native while anything in its owner subtree, at any depth, is a modeless window
  or a refused dialog; the timed retry that `WatchTopology` gives a refused window does not release it.
  The hold ends two seconds after the last such window has gone.
- **Custom-drawn statics.** The Color chooser projected with blank palettes (two `SS_SIMPLE` statics with
  `WS_TABSTOP` that comdlg32 subclasses and paints). A Static that takes the focus and has no text is now
  refused as a custom-drawn control, which keeps that dialog native and fully working.
- **Closing order.** `WindowRegistry.CloseAll` and the Bridge's `RestoreAll` take the deepest dialog of a
  chain first: an owner proxy destroyed before the proxy it owns takes it down from under its XAML window.

Two renderer defects surfaced by projecting a console while a dialog was open natively (the console
window is disabled, and the same state arises after any rollback): the chart's three custom elements
(graph, legend, value strip) reported themselves enabled whatever the native state (their peers did not
override `IsEnabledCore`), and the graph, applied at size zero before layout, was never drawn when the
control got no `SizeChanged` and no live sample patch followed; it now draws to the extent the factory
gave it.

#### Date and time pickers, and group boxes that own their controls (minor 37)

Task Scheduler's New Trigger dialog (nested over a task's Properties) was refused for its
`SysDateTimePick32` controls, and then for its Windows Forms group boxes.

- **`datePicker` / `timePicker`.** A picker's value travels in the range fields every node already
  carries, so the wire needed no new field and `setValue` no new shape: a date picker's `position` is
  its day number (days since 0001-01-01, `DateOnly.DayNumber`; `DateTimePickerValue.h` and the
  renderer count the same), its range the native `DTM_GETRANGE` clamped to 1900-2200 (what a calendar
  picker is built for); a time picker's is the second of the day (0 to 86399). The probe takes the stock
  formats (short, short with century, long date; time) and refuses `DTS_SHOWNONE` (a no-date check
  box), `DTS_APPCANPARSE` (the application parses the text) and a date picker whose text shows a time of
  day (a Windows Forms Custom format keeps the stock style bits and the format cannot be read back). The
  spin arrows a time picker owns are an implementation child. `setValue` sets `DTM_SETSYSTEMTIME`
  keeping the half the picker does not show, reads the value back (the control clamps to its range) and
  sends the parent the `DTN_DATETIMECHANGE` a user's change sends, which is the only way a Windows Forms
  `DateTimePicker` learns its new `Value` (`SetDateTimePickerValue`).
- **Renderer.** A date picker is WinUI's `CalendarDatePicker` (a subclass whose peer reports the
  native name, `Button`); a time picker is a button showing the native text that opens WinUI's
  `TimePickerFlyout`. Seconds have no column there, so a time chosen keeps the native seconds. UIA reports
  both as a button with Invoke.
- **Group boxes are containers.** A Windows Forms `GroupBox` is a window its controls are children of
  (a Win32 group box's controls are siblings), so a `groupBox` node may be a parent: the Bridge's
  `IsProjectedContainerKind`, the renderer validator's parent rule and a `SemanticGroupControl` with a
  child layer (frame and caption take no input, as before).
- **Clipped controls.** A native panel larger than the panel that clips it (the trigger dialog's
  scrolling-style pages) is reported by XAML as its own rectangle; the committed gate accepts the clipped
  or the whole rectangle for a node its parent clips.
- **Escape after a click that took the focus away.** Toggling controls of a dialog can leave keyboard
  focus on the XAML island's input site with no element to route a key to (the focused control was
  disabled or hidden by the native response), and the Escape handler sat on the content canvas, so the
  dialog ignored Escape from then on. It now sits on the window's root (the menu bar's own keys are
  excluded), which sees the key whatever holds the focus.

#### Two small admissions from the menu census

- **A group box with `WS_TABSTOP`** (MMC's Customize View) used to be refused. A group box never takes the
  focus, so the bit is a stop on nothing: the probe admits the box and capture drops it from the traversal
  (no tab stop, no tab index), like a progress bar's.
- **An animation control** (`SysAnimate32`: Find Certificates' flashlight) is an inert picture of the
  frame it shows at capture (`StaticBitmap`, the painted-surface path the Time Range bar uses); it is
  blank until a search starts, and a later frame arrives with a later capture. One that is a tab stop is
  refused.

### Tranche G: MDI Frames

- Project the MDI client area and each MDI child frame as real nested WinUI
  surfaces rather than refusing the frame. `MDIClient` is admitted as an inert
  container; an MDI child is admitted by the role its frame gave it
  (`WS_EX_MDICHILD`) rather than by its class name, because the caption, state,
  and system commands the projection replaces belong to `DefMDIChildProc` while
  the class belongs to the application.
- Each child carries its caption text, activation, one of three window states,
  and the client band its own controls live in, so the projected caption occupies
  exactly the native frame inset and nothing inside the child moves.
- The five caption verbs (`activate`, `close`, `minimize`, `maximize`,
  `restore`) are offered only when the child's own style bits and current state
  accept them, and each is routed as the `WM_SYSCOMMAND` its native caption
  button posts, or as `WM_MDIACTIVATE` through the client.
- Dialog-manager traversal is captured per container, because an MDI child runs
  its own `IsDialogMessage` loop and its controls are not reachable from the
  frame's walk.
- A maximized MDI child makes `DefFrameProc` insert window chrome into the
  frame's menu bar. Those bitmap items are skipped, because the projected child's
  caption already owns that contract; every textual menu item still goes through
  the ordinary menu rules.
- Still native: a scrolling MDI client area, an MDI child with scroll bars or
  layered/RTL composition, and any private container class inside a child that is
  not itself an admitted adapter. An MDI child's own client-area painting is not
  reproduced, which is the same compromise the projection already makes for a
  top-level window's client area.

### Tranche F: Application Adapters

- Match canonical executable identity and signer plus bounded HWND and UIA evidence.
- Package capture, actions, invalidation, presentation, and rollback tests as
  one versioned adapter.
- Never enable an application adapter globally by class name alone.

This is the only supported lane for private/custom HWND contracts.

The DirectUI translation engine in `DirectUiEngine.cpp` derives every surface's
contract from its current HWND and UIA evidence; see `DIRECTUI-APPLICATION-ADAPTERS.md`.
Protocol minor 29 removes the exact MdSched and RecoveryDrive profiles and their
adapter IDs. The remaining capability-derived contracts are restricted to
Microsoft-signed canonical System32 images, and every
projected slot takes its kind from the registered Win32 adapter that backs it, so
the DirectUI lane's boundary is this roadmap's boundary rather than a second list
maintained per application. Tranche A/B statics and buttons, Tranche C's one-row
toolbar and textual status bar, and Tranche E's report ListView and textual tab
control are all admissible on any built-in DirectUI page because their registry
rows exist. `SysTreeView32` and `msctls_trackbar32` now have registry rows too,
but the capability-derived lane classifies by UIA control type and has no Tree or
Slider case, so a DirectUI page containing one still stays native: admitting a
role there also means pinning its composite item census across the A/U/B bracket
and defining its in-place mutation, which is a step beyond the Win32 adapter
itself. Unsupported custom or
unexpectedly actionable roles keep the complete surface native.

## Target Findings

The current full MMC scope and its per-console validation status are tracked in
[MMC-VALIDATION-MATRIX.md](MMC-VALIDATION-MATRIX.md). Protocol minor 23 includes bounded
referenced-icon compaction, Static frame/fill projection, accessible page tabs,
positioned ListView modes and toolbar radio groups; MDI navigation and deferred
source-action cancellation have native regressions. These
shared fixes do not yet establish live support for every installed snap-in.

- `notepad.exe`: current builds are blocked by HMENU and may additionally use
  RichEdit, status bars, or existing XAML. Menu and text-document tranches are
  prerequisites; already-XAML processes remain excluded.
- `mmc.exe`: an empty console (`mmc.exe` with no snap-in) **projects end to end** as of
  Tranche J. The MDI frame, its client area, and its child frames are admitted since
  Tranche G, its icon-bearing label-editable tree and list views since Tranche H, its
  private containers, painted chrome bands, splitters, and reorderable list header since
  Tranche I, and its DirectUI Actions pane, menu bar, and both toolbar bands since
  Tranche J.
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

## System Tool Survey (2026-10-03)

Startup projection of classic System32 tools at ordinary integrity, injected after a
settled launch with no input. Projected: `winver`, `dxdiag`, `eudcedit`, `dccw`,
`cttune`, `rekeywiz`, `SystemPropertiesComputerName`, `odbcad32` (after zero-size
owners stopped holding the graph) and `msinfo32` (after fully clipped children stopped
being captured). Still native, with the blocker each one names first:

| Tool | First blocker |
| --- | --- |
| `charmap` | `CharGridWClass` custom grid; owner-draw font ComboBox; RichEdit |
| `mstsc` | Owner-draw ComboBox; toolbar hot/disabled image lists |
| `SndVol` | `Volume Flood` custom meter; `TBSTYLE_CUSTOMERASE` toolbar; nested non-control dialog |
| `colorcpl` | Grouped ListView; `Link Window` |
| `iexpress` | `SS_BITMAP` wizard art (larger than the generic 96 px image cap); `msctls_updown32` |
| `fsquirt` | DirectUI host that also owns `CtrlNotifySink` children with custom regions |
| `WFS` | `TVS_INFOTIP` tree; MFC scrolling preview view |

Follow-up the same day (protocol minor 28) added three adapter rows from that list:

- `staticBitmap`: an `SS_BITMAP` Static carries its `HBITMAP` pixels at the client
  size under the 1024 px bitmap cap, at the origin or centred over the documented
  top-left-pixel fill (`SS_CENTERIMAGE`); `SS_REALSIZECONTROL` scaling stays native.
- `upDown`: `msctls_updown32` carries its ordered range, a `reversed` flag, its
  position and its first acceleration step, projects as a UIA Spinner with
  RangeValue, and is driven by `setValue` through the control's own arrow sequence
  (`UDN_DELTAPOS`, which the parent may veto or rewrite, then `UDM_SETPOS32` and the
  `SB_THUMBPOSITION`/`SB_ENDSCROLL` pair). A veto is a rejected action, not a rollback.
- `BS_ICON`/`BS_BITMAP` push buttons carry their picture (icon cap) in the image
  fields and keep their text as the accessible name; picture check boxes and radio
  buttons stay native.

Capture also skips a wizard-mode property sheet's tab control: it lies under the page
(`WS_CLIPSIBLINGS`, fully covered by a sibling above it) and under comctl32 v6 lays out
no item rectangle at all. With these, `sigverif`, `cliconfg` and `iexpress`'s first page
project. `eudcedit` stays native because its visible MDI frame (a ComboBox inside its
status bar, MFC control bars) is unsupported, so its startup dialog stays native with it.

Follow-up on 2026-10-04 (protocol minor 29) adds owned hot, disabled, and pressed
toolbar image-list variants. Each referenced image must match its button's normal
icon dimensions, stay within the 96 px icon cap, and fit the existing aggregate
toolbar pixel budget together with all other states. The renderer chooses the state
image from the projected button's enabled, hover, pressed, and checked state;
reconciliation refreshes or removes those images when the native lists change.
Pressed buttons without a pressed image use the normal image; checked buttons use
the hot image even without hover, and hover uses hot only for flat/list toolbars.
Missing state images fall back to normal, while malformed or mismatched
lists keep the whole surface native. This extends the toolbar adapter, not the
owner-draw ComboBox or grouped ListView boundary, and does not establish projection
for `mstsc` or WinForms views on a separate GUI thread.

The remaining recurring Win32 gaps include owner-draw ComboBox and grouped ListView;
each needs a bounded adapter contract rather than a wider class allowlist.

## Completion Rule

A tranche is complete only when its native adapter, protocol model, WinUI
presenter, UIA contract, semantic actions, dynamic reconciliation, rollback,
and target-perspective tests pass together. Adding a visual approximation alone
does not expand the production support boundary.
