using System.ComponentModel;
using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Runtime;
using FluentShell.Renderer.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Windows.Graphics;

namespace FluentShell.Renderer.Windows;

public sealed class TranslatedWindow : Window
{
    private readonly Grid _root = new();
    private readonly SemanticContentViewport _canvas = new();
    private MenuBar? _menuBar;
    private readonly Func<ActionInvokeMessage, ulong, Task> _sendAction;
    private readonly Func<string> _nextEventId;
    private readonly Dictionary<string, (ControlNodeViewModel? Node, string Property)> _pending = new(StringComparer.Ordinal);
    private readonly TaskCompletionSource _loaded = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly AppWindow _appWindow;
    private readonly Dictionary<string, FrameworkElement> _controls = new(StringComparer.Ordinal);
    private readonly HashSet<string> _placementEventIds = new(StringComparer.Ordinal);
    private readonly PresenterActionCoordinator _presenterActions = new();
    private readonly BoundsActionCoordinator _boundsActions = new();
    private readonly NativeWindowInterop.SubclassProc _subclassProc;
    private bool _inSizeMove;
    private PixelRect? _sizeMoveStart;
    private bool _applyingCanonical;
    private bool _allowClose;
    private bool _committed;
    private bool _interactive;
    private PixelRect? _gateBounds;
    private bool _closePending;
    private bool _retired;
    private bool _interactionBlocked;
    private uint _renderDpi;

    public WindowViewModel ViewModel { get; }
    public nint Hwnd { get; }
    public bool IsCommitted => _committed;
    internal bool InteractionBlocked => _interactionBlocked;

    public TranslatedWindow(WindowSnapshot snapshot, Func<ActionInvokeMessage, ulong, Task> sendAction, Func<string> nextEventId)
    {
        ViewModel = WindowViewModel.FromSnapshot(snapshot);
        _sendAction = sendAction;
        _nextEventId = nextEventId;
        Title = ViewModel.Title;
        _root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        _root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        Grid.SetRow(_canvas, 1);
        _root.Children.Add(_canvas);
        Content = _root;
        _canvas.KeyDown += OnCanvasKeyDown;
        if (MicaController.IsSupported()) SystemBackdrop = new MicaBackdrop();
        ExtendsContentIntoTitleBar = false;
        Hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        _renderDpi = ReadWindowDpi();
        _appWindow = AppWindow.GetFromWindowId(Win32Interop.GetWindowIdFromWindow(Hwnd));
        // WinUI requires an owner before an overlapped presenter can be marked
        // modal.  The owner proxy is resolved by WindowRegistry after this
        // window is constructed, so defer IsModal until SetOwner.
        _root.Loaded += (_, _) => _loaded.TrySetResult();
        if (!SetCloaked(true)) throw new InvalidOperationException("Unable to cloak renderer proxy.");
        _appWindow.Title = ViewModel.Title;
        _appWindow.IsShownInSwitchers = ViewModel.ShowInTaskbar;
        _appWindow.Closing += OnClosing;
        _appWindow.Changed += OnAppWindowChanged;
        // AppWindow raises no enter/exit size-move signal, so observe the modal
        // move/size loop directly.  The delegate is held in a field because the
        // subclass keeps an unmanaged pointer to it.
        _subclassProc = OnSubclassMessage;
        if (!NativeWindowInterop.SetWindowSubclass(Hwnd, _subclassProc, SizeMoveSubclassId, 0))
            throw new InvalidOperationException("Unable to observe the renderer proxy move/size loop.");
        Closed += (_, _) => RendererDiagnostics.Log($"window closed hwnd=0x{(ulong)(nuint)Hwnd:X} surface={ViewModel.SurfaceId}");
        RebuildControls();
        ApplyWindowState();
        RefreshRenderDpi();
    }

    public void SetOwner(nint ownerHwnd)
    {
        if (ViewModel.Modal && ownerHwnd != 0)
        {
            NativeWindowInterop.SetOwner(Hwnd, ownerHwnd);
            if (_appWindow.Presenter is OverlappedPresenter presenter) presenter.IsModal = true;
        }
    }

    public void SetInteractionEnabled(bool enabled)
    {
        _interactionBlocked = !enabled;
        ApplyInteractionState();
    }

    public void Reactivate()
    {
        if (_committed && ViewModel.Visible)
        {
            Activate();
            FocusDefaultButton();
        }
    }

    public async Task<(PixelRect Bounds, int NodeCount, bool UiaReady)> PrepareSurfaceAsync()
    {
        _applyingCanonical = true;
        try
        {
            Activate();
            var readiness = await ReadSurfaceReadinessAsync();
            // Keep the HWND materialized for UIA while DWM cloak keeps it out of
            // the user's surface until the Bridge commits the cutover.
            if (!SetCloaked(true)) throw new InvalidOperationException("Unable to retain ready proxy cloak.");
            return readiness;
        }
        finally
        {
            _applyingCanonical = false;
        }
    }

    public async Task<(PixelRect Bounds, int NodeCount, bool UiaReady)> ReadSurfaceReadinessAsync()
    {
        var keepHidden = !_committed;
        if (keepHidden) _appWindow.Show(false);
        try
        {
        await _loaded.Task.WaitAsync(TimeSpan.FromSeconds(2));
        _root.UpdateLayout();
        await WaitForRenderAsync();
        _root.UpdateLayout();
        RefreshRenderDpi();
        var bounds = CurrentBounds();
        return (bounds, ViewModel.Nodes.Count, Content.XamlRoot is not null && Hwnd != 0);
        }
        finally
        {
            if (keepHidden && !SetCloaked(true))
                RendererDiagnostics.Log("ready proxy cloak failed after layout");
        }
    }

    private static async Task WaitForRenderAsync()
    {
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        EventHandler<object>? handler = null;
        handler = (_, _) =>
        {
            if (handler is not null) CompositionTarget.Rendering -= handler;
            completion.TrySetResult();
        };
        CompositionTarget.Rendering += handler;
        try
        {
            await completion.Task.WaitAsync(TimeSpan.FromSeconds(2));
        }
        finally
        {
            if (handler is not null) CompositionTarget.Rendering -= handler;
        }
    }

    public void ApplyPatch(WindowPatchMessage patch)
    {
        PresenterActionIntent? presenterReplay = null;
        BoundsActionIntent? boundsReplay = null;
        RendererDiagnostics.Log($"window.patch event={patch.EventId ?? "-"} rev={patch.Revision} stateBefore={ViewModel.State} inFlight={_presenterActions.InFlightEventId ?? "-"}");
        _applyingCanonical = true;
        try
        {
            var placementBefore = (
                ViewModel.State, ViewModel.Visible,
                ViewModel.Bounds.X, ViewModel.Bounds.Y,
                ViewModel.Bounds.Width, ViewModel.Bounds.Height);
            var placementActionPatch = patch.EventId is not null &&
                _placementEventIds.Remove(patch.EventId);
            var structural = patch.Operations.Any(operation => operation.Op is "add" or "remove") ||
                (patch.Snapshot is not null && !ViewModel.CanMergeSnapshot(patch.Snapshot));
            ViewModel.ApplyPatch(patch);
            var placementChanged = placementBefore != (
                ViewModel.State, ViewModel.Visible,
                ViewModel.Bounds.X, ViewModel.Bounds.Y,
                ViewModel.Bounds.Width, ViewModel.Bounds.Height);
            if (patch.EventId is not null && _pending.Remove(patch.EventId, out var committed))
            {
                committed.Node?.AcceptPending(committed.Property, patch.EventId);
            }
            presenterReplay = _presenterActions.CompletePatch(patch.EventId, ViewModel.State);
            // Clear the bounds action before reading LocalPlacementPending so the
            // patch that answers a move is free to apply the canonical result.
            boundsReplay = _boundsActions.CompletePatch(patch.EventId, ViewModel.Bounds);
            if (structural) RebuildControls();
            ApplyWindowState(ShouldApplyCanonicalPlacement(
                _committed, placementChanged, placementActionPatch, LocalPlacementPending));
        }
        finally
        {
            _applyingCanonical = false;
        }
        RendererDiagnostics.Log($"window.patch applied rev={ViewModel.Revision} state={ViewModel.State} replay={(presenterReplay?.Action ?? "-")} inFlight={_presenterActions.InFlightEventId ?? "-"}");
        if (presenterReplay is { } replay) QueueOrEmitPresenterAction(replay);
        if (boundsReplay is { } boundsIntent) QueueOrEmitBoundsAction(boundsIntent);
    }

    public void ApplyActionResult(ActionResultMessage result)
    {
        RendererDiagnostics.Log($"action.result event={result.EventId} status={result.Status} rev={result.Revision} inFlight={_presenterActions.InFlightEventId ?? "-"}");
        var resultRevision = ProtocolSerializer.ParseCanonicalUInt64(result.Revision, "action.result.revision");
        if (resultRevision < ViewModel.Revision)
            throw new ProtocolException("action.result revision precedes canonical renderer state.");
        _applyingCanonical = true;
        try
        {
            if (result.Snapshot is not null)
            {
                if (resultRevision <= ViewModel.Revision)
                    throw new ProtocolException("action.result snapshot does not advance canonical renderer state.");
                ViewModel.ApplySnapshot(result.Snapshot);
                RebuildControls();
                ApplyWindowState();
            }
            if (_pending.TryGetValue(result.EventId, out var pending))
            {
                if (result.Status is "rejected" or "stale" or "closeRejected")
                {
                    _pending.Remove(result.EventId);
                    pending.Node?.RejectPending(pending.Property, result.EventId);
                    if (pending.Property == "close") _closePending = false;
                }
                else if (result.Snapshot is not null ||
                         (result.Status == "accepted" && pending.Property == "invoke"))
                {
                    _pending.Remove(result.EventId);
                    pending.Node?.AcceptPending(pending.Property, result.EventId);
                }
            }
            if (result.Status == "stale")
            {
                _presenterActions.MarkStale(result.EventId);
                _boundsActions.MarkStale(result.EventId);
            }
            if (result.Status == "rejected")
            {
                _placementEventIds.Remove(result.EventId);
                _presenterActions.CancelIfMatches(result.EventId);
                _boundsActions.CancelIfMatches(result.EventId);
            }
        }
        finally
        {
            _applyingCanonical = false;
        }
    }

    public void Commit(bool show, ulong revision, bool interactive)
    {
        if (revision != ViewModel.Revision) throw new ProtocolException("surface.commit revision does not match renderer state.");
        RendererDiagnostics.Log(
            $"surface.commit surface={ViewModel.SurfaceId} show={show} interactive={interactive} revision={revision}");
        _applyingCanonical = true;
        try
        {
            if (show)
            {
                _committed = true;
                _interactive = interactive;
                _gateBounds = interactive ? null : ViewModel.Bounds;
                ApplyWindowState();
                if (ViewModel.Visible)
                {
                    Activate();
                    FocusDefaultButton();
                }
            }
            else
            {
                _committed = false;
                _interactive = false;
                _gateBounds = null;
                _appWindow.Hide();
                if (!SetCloaked(true)) RendererDiagnostics.Log("proxy cloak failed while hiding surface");
            }
        }
        finally { _applyingCanonical = false; }
    }

    public void RetireFromBridge()
    {
        _applyingCanonical = true;
        try
        {
            _committed = false;
            _interactive = false;
            _gateBounds = null;
            _retired = true;
            _closePending = false;
            _pending.Clear();
            _placementEventIds.Clear();
            _presenterActions.Reset();
            ResetSizeMove();
            _appWindow.IsShownInSwitchers = false;
            SetInteractionEnabled(false);
            _appWindow.Hide();
            if (!SetCloaked(true)) RendererDiagnostics.Log("retired proxy cloak failed after hide");
            _canvas.Children.Clear();
            if (_menuBar is not null) _root.Children.Remove(_menuBar);
            _menuBar = null;
            _controls.Clear();
        }
        finally
        {
            _applyingCanonical = false;
        }
    }

    public void CloseFromBridge()
    {
        _applyingCanonical = true;
        try
        {
            _committed = false;
            _interactive = false;
            _gateBounds = null;
            _retired = true;
            _closePending = false;
            _pending.Clear();
            _placementEventIds.Clear();
            _presenterActions.Reset();
            ResetSizeMove();
            _appWindow.Hide();
            if (!SetCloaked(true)) RendererDiagnostics.Log("closing proxy cloak failed after hide");
            _allowClose = true;
            Close();
        }
        finally
        {
            _applyingCanonical = false;
        }
    }

    private void RebuildControls()
    {
        _canvas.Children.Clear();
        _controls.Clear();
        if (_menuBar is not null) _root.Children.Remove(_menuBar);
        _menuBar = null;
        if (ViewModel.Menu.Count != 0)
        {
            _menuBar = new MenuProjectionFactory(EmitMenuAction).Create(ViewModel.Menu);
            Grid.SetRow(_menuBar, 0);
            _root.Children.Insert(0, _menuBar);
        }
        var dpi = RenderDpi;
        _canvas.Width = ViewModel.ClientBounds.Width * 96.0 / dpi;
        _canvas.Height = ViewModel.ClientBounds.Height * 96.0 / dpi;
        var factory = new ControlFactory(
            Math.Max(1, (int)Math.Round(dpi)), ViewModel.Nodes, EmitNodeAction,
            () => !CanEmitActions,
            IsImeComposing);
        var icon = CreateDialogIcon();
        if (icon is not null) _canvas.Children.Add(icon);
        foreach (var node in ViewModel.Nodes)
        {
            var control = factory.Create(node);
            _controls[node.NodeId] = control;
            if (node.ParentNodeId is null)
            {
                _canvas.Children.Add(control);
            }
            else if (_controls[node.ParentNodeId] is SemanticDialogContainer container)
            {
                container.Children.Add(control);
            }
            else
            {
                throw new InvalidOperationException("Validated control parent is not a dialog container.");
            }
        }
    }

    private FontIcon? CreateDialogIcon()
    {
        var (glyph, name) = ViewModel.Icon switch
        {
            "warning" => ("\uE7BA", "Warning"),
            "error" => ("\uEA39", "Error"),
            "info" => ("\uE946", "Information"),
            "question" => ("\uE9CE", "Question"),
            "shield" => ("\uEA18", "Security"),
            _ => (string.Empty, string.Empty),
        };
        if (glyph.Length == 0) return null;
        var icon = new FontIcon { Glyph = glyph, FontSize = 32 };
        AutomationProperties.SetName(icon, name);
        Canvas.SetLeft(icon, 16);
        Canvas.SetTop(icon, 16);
        Canvas.SetZIndex(icon, 512);
        return icon;
    }

    internal static bool ShouldApplyCanonicalBounds(bool committed, string canonicalState) =>
        !committed || canonicalState == "normal";

    internal static bool ShouldEmitGeometry(
        string canonicalState,
        OverlappedPresenterState presenterState) =>
        canonicalState == "normal" && presenterState == OverlappedPresenterState.Restored;

    internal static bool ShouldApplyCanonicalPlacement(
        bool committed,
        bool placementChanged,
        bool placementActionPatch,
        bool localPlacementPending) =>
        !committed ||
        (!localPlacementPending && (placementChanged || placementActionPatch));

    /// <summary>
    /// True while this proxy, not the Bridge, owns the window's geometry: the user
    /// is inside a move/size gesture, or a bounds action they started has not been
    /// answered yet.  Re-applying canonical placement in either case yanks the
    /// window out from under the pointer.
    /// </summary>
    private bool LocalPlacementPending => _inSizeMove || _boundsActions.HasInFlight;

    private void ApplyWindowState(bool applyCanonicalPlacement = true)
    {
        Title = ViewModel.Title;
        _appWindow.Title = ViewModel.Title;
        _appWindow.IsShownInSwitchers = ViewModel.ShowInTaskbar;
        _root.FlowDirection = ViewModel.Rtl ? FlowDirection.RightToLeft : FlowDirection.LeftToRight;
        var dpi = RenderDpi;
        _canvas.Width = ViewModel.ClientBounds.Width * 96.0 / dpi;
        _canvas.Height = ViewModel.ClientBounds.Height * 96.0 / dpi;
        if (_appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsResizable = ViewModel.IsResizable;
            presenter.IsMinimizable = ViewModel.IsMinimizable;
            presenter.IsMaximizable = ViewModel.IsMaximizable;
            presenter.IsAlwaysOnTop = ViewModel.IsAlwaysOnTop;
            if (applyCanonicalPlacement &&
                ShouldApplyCanonicalBounds(_committed, ViewModel.State))
            {
                if (_committed && presenter.State != OverlappedPresenterState.Restored)
                    presenter.Restore();
                _appWindow.MoveAndResize(new RectInt32(
                    ViewModel.Bounds.X, ViewModel.Bounds.Y,
                    ViewModel.Bounds.Width, ViewModel.Bounds.Height));
            }
        }
        else if (applyCanonicalPlacement)
        {
            _appWindow.MoveAndResize(new RectInt32(
                ViewModel.Bounds.X, ViewModel.Bounds.Y,
                ViewModel.Bounds.Width, ViewModel.Bounds.Height));
        }
        RefreshRenderDpi();
        ApplyInteractionState();
        if (_committed)
        {
            if (ViewModel.Visible)
            {
                if (!SetCloaked(false)) throw new InvalidOperationException("Unable to uncloak renderer proxy.");
                _appWindow.Show(false);
                if (applyCanonicalPlacement) ApplyPresenterState();
            }
            else
            {
                _appWindow.Hide();
                if (!SetCloaked(true)) RendererDiagnostics.Log("hidden canonical proxy cloak failed after hide");
            }
        }
    }

    private bool CanEmitActions =>
        !_applyingCanonical && _committed && !_retired &&
        _interactive && ViewModel.Visible && ViewModel.Enabled && !_interactionBlocked;

    private void ApplyInteractionState() =>
        NativeWindowInterop.SetEnabled(
            Hwnd, ViewModel.Enabled && !_interactionBlocked && !_retired);

    private void FocusDefaultButton()
    {
        var node = ViewModel.Nodes.FirstOrDefault(candidate =>
            candidate.Kind == "button" && candidate.IsDefault && candidate.Enabled && candidate.Visible);
        node ??= ViewModel.Nodes
            .Where(candidate => candidate.TabStop && candidate.Enabled && candidate.Visible)
            .OrderBy(candidate => candidate.TabIndex)
            .FirstOrDefault();
        node ??= ViewModel.Nodes.FirstOrDefault(candidate =>
            candidate.Enabled && candidate.Visible &&
            candidate.Kind is "edit" or "password" or "comboBox" or "listBox" or "listView");
        if (node is not null && _controls.TryGetValue(node.NodeId, out var control))
        {
            // Win32 WS_TABSTOP describes dialog traversal, not whether a main
            // document control can own keyboard focus. WinUI's IsTabStop also
            // gates programmatic focus, so enable it only for the focus call
            // and immediately restore the native traversal contract.
            var restoreTabStop = !control.IsTabStop;
            if (restoreTabStop) control.IsTabStop = true;
            try
            {
                control.Focus(FocusState.Programmatic);
            }
            finally
            {
                if (restoreTabStop) control.IsTabStop = false;
            }
        }
    }

    private void ApplyPresenterState()
    {
        if (_appWindow.Presenter is not OverlappedPresenter presenter) return;
        switch (ViewModel.State)
        {
            case "minimized": presenter.Minimize(); break;
            case "maximized": presenter.Maximize(); break;
            default: presenter.Restore(); break;
        }
    }

    private void EmitNodeAction(ControlNodeViewModel node, string action, object? value)
    {
        EmitAction(node, PropertyForNodeAction(action), action, value);
    }

    internal static string PropertyForNodeAction(string action) => action switch
        {
            "setText" => "text",
            "setCheck" => "checked",
            "select" => "selectedIndex",
            "setSelection" => "selectedIndices",
            "setItemCheck" => "checkedIndices",
            _ => action,
        };

    private void EmitMenuAction(MenuItemViewModel item) =>
        EmitAction(null, $"menu:{item.CommandId}", "menuCommand", item.CommandId);

    private string? EmitAction(ControlNodeViewModel? node, string property, string action, object? value)
    {
        if (!CanEmitActions) return null;
        if ((property == "invoke" || property == "close" || property.StartsWith("menu:", StringComparison.Ordinal)) &&
            _pending.Values.Any(pending =>
                ReferenceEquals(pending.Node, node) && pending.Property == property))
        {
            return null;
        }
        var eventId = _nextEventId();
        node?.RegisterPending(property, eventId);
        _pending[eventId] = (node, property);
        if (property is "state" or "bounds") _placementEventIds.Add(eventId);
        var message = new ActionInvokeMessage
        {
            SessionNonce = string.Empty, // WindowRegistry stamps the session nonce.
            SurfaceId = ViewModel.SurfaceId,
            NodeId = node?.NodeId,
            EventId = eventId,
            ExpectedRevision = ViewModel.Revision.ToString(System.Globalization.CultureInfo.InvariantCulture),
            Action = action,
            Value = JsonSerializer.SerializeToElement(value),
        };
        _ = _sendAction(message, ViewModel.Revision);
        return eventId;
    }

    private void QueueOrEmitPresenterAction(string action) =>
        QueueOrEmitPresenterAction(new PresenterActionIntent(action, 0));

    private void QueueOrEmitPresenterAction(PresenterActionIntent intent)
    {
        if (_presenterActions.HasInFlight)
        {
            _presenterActions.QueueLatest(intent.Action);
            RendererDiagnostics.Log($"presenter queued action={intent.Action} retry={intent.RetryCount} inFlight={_presenterActions.InFlightEventId}");
            return;
        }

        var eventId = EmitAction(null, "state", intent.Action, null);
        if (eventId is not null)
        {
            _presenterActions.MarkSent(eventId, intent);
            RendererDiagnostics.Log($"presenter sent action={intent.Action} retry={intent.RetryCount} event={eventId} rev={ViewModel.Revision}");
        }
    }

    private void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_allowClose) return;
        args.Cancel = true;
        RequestClose();
    }

    private void RequestClose()
    {
        if (!ViewModel.CanCancel || _closePending) return;
        _closePending = true;
        EmitAction(null, "close", "close", null);
    }

    private void OnCanvasKeyDown(object sender, Microsoft.UI.Xaml.Input.KeyRoutedEventArgs args)
    {
        if (args.Key == global::Windows.System.VirtualKey.Enter)
        {
            var defaultButton = ViewModel.Nodes.FirstOrDefault(node => node.Kind == "button" && node.IsDefault && node.Enabled && node.Visible);
            if (defaultButton is not null)
            {
                EmitNodeAction(defaultButton, "invoke", null);
                args.Handled = true;
            }
        }
        else if (args.Key == global::Windows.System.VirtualKey.Escape && ViewModel.CanCancel)
        {
            RequestClose();
            args.Handled = true;
        }
    }

    private void OnAppWindowChanged(AppWindow sender, AppWindowChangedEventArgs args)
    {
        if (_applyingCanonical || !_committed || !_interactive) return;
        RefreshRenderDpi();
        if (args.DidPresenterChange && sender.Presenter is OverlappedPresenter presenter)
        {
            var (state, action) = presenter.State switch
            {
                OverlappedPresenterState.Minimized => ("minimized", "minimize"),
                OverlappedPresenterState.Maximized => ("maximized", "maximize"),
                _ => ("normal", "restore"),
            };
            RendererDiagnostics.Log($"presenter changed state={state} action={action} canonical={ViewModel.State} applying={_applyingCanonical} inFlight={_presenterActions.InFlightEventId ?? "-"}");
            var desiredState = _presenterActions.DesiredState(ViewModel.State);
            if (!string.Equals(state, desiredState, StringComparison.Ordinal))
            {
                QueueOrEmitPresenterAction(action);
                return;
            }
        }
        if (args.DidPositionChange || args.DidSizeChange)
        {
            // The OS owns geometry for the duration of a move/size gesture.  One
            // authoritative action follows on WM_EXITSIZEMOVE, so emitting per
            // frame here would only round-trip positions the user already left.
            if (_inSizeMove) return;
            if (sender.Presenter is OverlappedPresenter boundsPresenter &&
                !ShouldEmitGeometry(ViewModel.State, boundsPresenter.State))
            {
                return;
            }
            var action = args.DidSizeChange && !args.DidPositionChange ? "resize" : "move";
            QueueOrEmitBoundsAction(new BoundsActionIntent(action, CurrentBounds(), 0));
        }
    }

    private PixelRect CurrentBounds() => new()
    {
        X = _appWindow.Position.X,
        Y = _appWindow.Position.Y,
        Width = _appWindow.Size.Width,
        Height = _appWindow.Size.Height,
    };

    private nint OnSubclassMessage(
        nint hwnd, uint message, nint wParam, nint lParam, nuint subclassId, nuint refData)
    {
        // Committed but not interactive: the proxy is on screen so the committed
        // UIA gate can see it, yet it must behave as if it were not there.
        if (!_interactive && ApplyPreInteractiveGate(message, wParam, lParam) is { } gated)
        {
            return gated;
        }
        if (message == WindowMessages.EnterSizeMove)
        {
            _inSizeMove = true;
            _sizeMoveStart = CurrentBounds();
        }
        var result = NativeWindowInterop.DefSubclassProc(hwnd, message, wParam, lParam);
        switch (message)
        {
            case WindowMessages.ExitSizeMove:
                _inSizeMove = false;
                CommitSizeMove();
                break;
            case WindowMessages.NcDestroy:
                NativeWindowInterop.RemoveWindowSubclass(hwnd, _subclassProc, SizeMoveSubclassId);
                break;
        }
        return result;
    }

    /// <summary>
    /// Returns the result to answer a message with while input is still gated, or
    /// null to let it reach the default handler.
    /// </summary>
    private nint? ApplyPreInteractiveGate(uint message, nint wParam, nint lParam)
    {
        if (ShouldClampProvisionalBounds(_interactive, _applyingCanonical) &&
            message == WindowMessages.WindowPosChanging &&
            _gateBounds is { } gateBounds)
        {
            NativeWindowInterop.ClampWindowPosition(lParam, gateBounds);
        }
        if (message == WindowMessages.SysCommand &&
            WindowMessages.IsPlacementSystemCommand(wParam)) return 0;
        if (message == WindowMessages.MouseActivate) return WindowMessages.NoActivateAndEat;
        if (WindowMessages.IsInputMessage(message)) return 0;
        return null;
    }

    private void CommitSizeMove()
    {
        var start = _sizeMoveStart;
        _sizeMoveStart = null;
        if (_applyingCanonical || !_committed || !_interactive) return;
        var bounds = CurrentBounds();
        if (start is null || start == bounds) return;
        if (_appWindow.Presenter is OverlappedPresenter presenter &&
            !ShouldEmitGeometry(ViewModel.State, presenter.State))
        {
            return;
        }
        var sizeChanged = start.Width != bounds.Width || start.Height != bounds.Height;
        var positionChanged = start.X != bounds.X || start.Y != bounds.Y;
        var action = sizeChanged && !positionChanged ? "resize" : "move";
        RendererDiagnostics.Log(
            $"size-move committed action={action} bounds={bounds.X},{bounds.Y} " +
            $"{bounds.Width}x{bounds.Height}");
        QueueOrEmitBoundsAction(new BoundsActionIntent(action, bounds, 0));
    }

    private void QueueOrEmitBoundsAction(BoundsActionIntent intent)
    {
        if (_boundsActions.HasInFlight)
        {
            _boundsActions.QueueLatest(intent);
            RendererDiagnostics.Log($"bounds queued action={intent.Action} inFlight={_boundsActions.InFlightEventId}");
            return;
        }

        var eventId = EmitAction(null, "bounds", intent.Action, new
        {
            x = intent.Bounds.X,
            y = intent.Bounds.Y,
            width = intent.Bounds.Width,
            height = intent.Bounds.Height,
        });
        if (eventId is not null) _boundsActions.MarkSent(eventId, intent);
    }

    private void ResetSizeMove()
    {
        _inSizeMove = false;
        _sizeMoveStart = null;
        _boundsActions.Reset();
    }

    internal static bool ShouldClampProvisionalBounds(
        bool interactive, bool applyingCanonical) =>
        !interactive && !applyingCanonical;

    private const nuint SizeMoveSubclassId = 1;

    private bool SetCloaked(bool cloaked) => NativeWindowInterop.SetCloaked(Hwnd, cloaked);

    private bool IsImeComposing() => NativeWindowInterop.IsImeComposing(Hwnd);

    private double RenderDpi => _renderDpi == 0
        ? Math.Max(1u, ViewModel.Dpi)
        : _renderDpi;

    private uint ReadWindowDpi()
    {
        var dpi = NativeWindowInterop.GetWindowDpi(Hwnd);
        return dpi == 0 ? (uint)Math.Max(1, ViewModel.Dpi) : dpi;
    }

    private void RefreshRenderDpi()
    {
        var dpi = ReadWindowDpi();
        if (_renderDpi == dpi) return;
        _renderDpi = dpi;
        RebuildControls();
    }
}

internal sealed class SemanticContentViewport : Canvas
{
    internal const string AutomationId = "FluentShell.ContentViewport";
    internal static AutomationControlType ControlType => AutomationControlType.Pane;

    public SemanticContentViewport()
    {
        AutomationProperties.SetAutomationId(this, AutomationId);
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticContentViewportAutomationPeer(this);
}

internal sealed class SemanticContentViewportAutomationPeer(SemanticContentViewport owner) :
    FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        SemanticContentViewport.ControlType;
    protected override string GetClassNameCore() => "ContentViewport";
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
}
