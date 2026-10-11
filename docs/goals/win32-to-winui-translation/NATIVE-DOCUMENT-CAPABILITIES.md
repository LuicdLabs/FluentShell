# Native document capability contract

`NativeDocument.cpp` reads a bounded `Internet Explorer_Server` document on its
owning GUI thread. The production entry requires a System32 resource URL. DOM
elements supply text, heading, link, image, and scrollport semantics; a read-only
System Monitor report supplies its visible counter paths and current samples
through COM Automation. The renderer creates WinUI controls for those facts.

The contract includes:

- A bridge-owned document lifecycle token tied to canonical COM `IUnknown`
  identity. MSHTML's document `uniqueID` generates IDs and is not used as a
  document identifier. Link tokens remain stable while the DOM element, handler,
  and destination remain unchanged.
- A complete text census. Hidden content is omitted, links retain their action,
  and text mixed into an unsupported layout causes the whole surface to remain
  native. Arbitrary script input handlers, editable elements, custom tab order,
  and an HWND host with `WS_TABSTOP` are rejected with a specific reason.
- Flat HTML scrollports with canonical content extents and absolute offsets.
  System Monitor reports retain their native scroll ranges; local WinUI overflow
  keeps every captured row reachable when its native axis has no range.
- An exact visible report window census: the System32 `sysmon.ocx` `SysmonCtrl`
  root, one immediate `SysmonReport`, and the inert system `Hatchwin` ancestor.
  Other visible descendants are not absorbed. The report must be read-only, show
  current values, and have no toolbar, legend, or value bar.
- Owned image pixels captured by explicit `WM_PRINT` over two backgrounds.
  A host that supplies no print pixels is rejected. `PrintWindow` is not used for
  this alpha contract because its compositor path can discard the backgrounds.
- At most 256 DOM elements, 16 scrollports, 256 aggregate report rows, 65,536 text
  characters, and 1 MiB of decoded image pixels. Images retain the 96 px icon cap.

Actions carry the document ID, stable DOM key, and link token or absolute scroll
offset. The Bridge revalidates them before dispatch and checks cancellation before
each native write. COM calls use copied bindings, so a nested message pump cannot
invalidate an iterator retained by the outer operation. Navigation and HWND
destruction retire cached bindings.

Live sample and label patches update existing WinUI elements. Unchanged document
structure keeps the focused link or scroll viewer, and an unchanged native scroll
offset does not reset an in-progress gesture.

`NativeDocumentTests.h` uses real STA MSHTML documents to exercise capture, scroll,
link invocation, handler replacement, cancellation, and cache retirement. The
shared `window.open.native-document.json` and action fixtures cover the wire
boundary; `NativeDocumentTests.cs` covers required facts, range/ownership rules,
lossless action identity, reconciliation, and local overflow.

The installed `wdc.dll` `home.htm` resource has complete description text separated
by `BR` elements, two clickable links (one graphical), and the supported read-only
System Monitor report. Resource inspection establishes the markup shape; actual
MMC projection and rollback remain separate acceptance checks.
