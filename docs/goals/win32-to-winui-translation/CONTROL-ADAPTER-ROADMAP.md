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
  columns/rows, visible or hidden headers, independent selection, optional
  `LVS_EX_CHECKBOXES` state/actions, per-item icons, and in-place label editing
  where `LVS_EDITLABELS` admits it. Virtual, owner/custom-draw, grouped,
  activation-tracking, and header-drag semantics remain native.
- A bounded textual top-tab `SysTabControl32` subset is implemented with native
  multiline header rectangles, semantic vetoable selection, and Tab/TabItem UIA.
  Opaque native `TCITEM.lParam` identity is retained only by the source control.
  Owner-draw, button, vertical/bottom, fixed-width, image, tooltip,
  callback-text, excessive, and malformed geometry shapes remain native.
- A bounded `SysTreeView32` subset is implemented: the whole inserted hierarchy is
  flattened into one canonical depth-first order carrying per-item label, nesting
  depth, expansion, child evidence, and normal/selected image indexes, with single
  selection, a real expansion route so a lazily populated tree still fills in
  through its own `TVN_ITEMEXPANDING`, and in-place label editing where
  `TVS_EDITLABELS` admits it. State images and checkboxes, hover selection,
  auto-collapse, callback children, application infotips, and multi-select
  extended styles remain native.
- A bounded `msctls_trackbar32` subset is implemented: range, position, line and
  page size, orientation, and the native reversed hint, driven through the
  control's own `WM_HSCROLL`/`WM_VSCROLL` notification and validated as UIA
  RangeValue against canonical native state. Selection ranges, thumbless bars,
  control-owned tooltips, and pre-move veto snapping remain native.
- Add nonvirtual, non-owner/custom-draw Header, DateTimePicker, MonthCalendar,
  UpDown, and tooltip subsets.
- Model selection, expansion, grouping, sorting, and notifications as typed
  capabilities.
- Reject `LVS_OWNERDATA`, callback items, private data, custom draw, and missing
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
  native control does. State and overlay image lists stay refused: they encode
  application state the projection has no contract for.
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
rows exist. `SysTreeView32` and `msctls_trackbar32` now have registry rows too,
but the capability-derived lane classifies by UIA control type and has no Tree or
Slider case, so a DirectUI page containing one still stays native: admitting a
role there also means pinning its composite item census across the A/U/B bracket
and defining its in-place mutation, which is a step beyond the Win32 adapter
itself. Unsupported custom or
unexpectedly actionable roles keep the complete surface native.

## Target Findings

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

## Completion Rule

A tranche is complete only when its native adapter, protocol model, WinUI
presenter, UIA contract, semantic actions, dynamic reconciliation, rollback,
and target-perspective tests pass together. Adding a visual approximation alone
does not expand the production support boundary.
