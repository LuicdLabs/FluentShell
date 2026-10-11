#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// Performance Monitor's chart (protocol minor 36): how the recorded samples fill the graph's slots
// in both chart styles, the text the legend and the value strip show, what crosses the wire, and
// what an action may name. The System Monitor control itself cannot be hosted here, so these cover
// the parts that do not need it; the live check is the projected perfmon console.

namespace {
using PerformanceHistoryValues = std::vector<std::vector<std::optional<double>>>;

Translation::SysmonSettings ChartSettings(bool scroll) {
    Translation::SysmonSettings settings;
    settings.dataPointCount = 5;
    settings.updateInterval = 1.0f;
    settings.chartScroll = scroll;
    Translation::SysmonCounter counter;
    counter.identity = 7;
    settings.counters.push_back(counter);
    return settings;
}

Translation::PerformanceGraphSnapshot EmptyGraph() {
    Translation::PerformanceGraphSnapshot graph;
    graph.intervalMs = 1000;
    graph.sampleCount = 5;
    graph.series.resize(1);
    return graph;
}

bool SlotsAre(const Translation::PerformanceGraphSnapshot& graph, const std::vector<std::optional<double>>& expected) {
    return graph.series.size() == 1 && graph.series[0].values == expected;
}
} // namespace

void TestPerformanceChartLayout() {
    const std::optional<double> none;
    {
        // The first pass of a wrapping chart: samples fill slots from the left, the time bar stands at
        // the newest, and a sample the counter had no value for leaves its slot empty.
        auto graph = EmptyGraph();
        Translation::SysmonHistory history;
        history.times = { 1'000'000, 1'001'000, 1'002'000 };
        history.values = PerformanceHistoryValues{ { 1.0, 2.0, std::nullopt } };
        history.collected = 3;
        Translation::LayOutPerformanceSamples(ChartSettings(false), history, graph);
        Check(SlotsAre(graph, { 1.0, 2.0, none, none, none }) && graph.barSlot == 2 && !graph.wrapped,
            "a wrapping chart's first pass did not fill slots from the left with the bar at the newest sample");
        Check(graph.startTimeMs == 1'000'000 && graph.endTimeMs == 1'004'000,
            "a wrapping chart's axis did not start at its first sample and span the slots");
    }
    {
        // After the chart wrapped: sample n lives in slot n mod 5, the older samples stay right of the bar.
        auto graph = EmptyGraph();
        Translation::SysmonHistory history;
        history.times = { 20'000, 21'000, 22'000, 23'000, 24'000 };
        history.values = PerformanceHistoryValues{ { 2.0, 3.0, 4.0, 5.0, 6.0 } };
        history.collected = 7;
        Translation::LayOutPerformanceSamples(ChartSettings(false), history, graph);
        Check(SlotsAre(graph, { 5.0, 6.0, 2.0, 3.0, 4.0 }) && graph.barSlot == 1 && graph.wrapped,
            "a wrapped chart did not keep the previous pass right of its time bar");
        // The pass in progress began with sample 5, the one in slot 0.
        Check(graph.startTimeMs == 23'000 && graph.endTimeMs == 27'000,
            "a wrapped chart's axis did not restart at the pass's first sample");
    }
    {
        // A chart that has just wrapped exactly: the newest sample is in the last slot.
        auto graph = EmptyGraph();
        Translation::SysmonHistory history;
        history.times = { 1000, 2000, 3000, 4000, 5000 };
        history.values = PerformanceHistoryValues{ { 1.0, 2.0, 3.0, 4.0, 5.0 } };
        history.collected = 5;
        Translation::LayOutPerformanceSamples(ChartSettings(false), history, graph);
        Check(SlotsAre(graph, { 1.0, 2.0, 3.0, 4.0, 5.0 }) && graph.barSlot == 4 && graph.startTimeMs == 1000 &&
              !graph.wrapped,
            "a chart that filled exactly did not put its bar on the last slot of its first pass");
    }
    {
        // A scrolling chart keeps the newest sample in the last slot and its axis ends there.
        auto graph = EmptyGraph();
        Translation::SysmonHistory history;
        history.times = { 1'000'000, 1'001'000, 1'002'000 };
        history.values = PerformanceHistoryValues{ { 1.0, 2.0, 3.0 } };
        history.collected = 3;
        Translation::LayOutPerformanceSamples(ChartSettings(true), history, graph);
        Check(SlotsAre(graph, { none, none, 1.0, 2.0, 3.0 }) && graph.barSlot == -1 && !graph.wrapped,
            "a scrolling chart did not right-align its samples without a time bar");
        Check(graph.endTimeMs == 1'002'000 && graph.startTimeMs == 998'000,
            "a scrolling chart's axis did not end at its newest sample");
    }
    {
        // Nothing recorded yet: every slot is empty and the axis still has a span.
        auto graph = EmptyGraph();
        Translation::LayOutPerformanceSamples(ChartSettings(false), Translation::SysmonHistory{}, graph);
        Check(SlotsAre(graph, { none, none, none, none, none }) && graph.barSlot == -1 &&
            graph.endTimeMs > graph.startTimeMs && graph.startTimeMs > 0,
            "an empty history did not give empty slots and a positive axis span");
    }
}

void TestPerformanceChartText() {
    Check(Translation::TrimPerformanceScale(L"1.000") == L"1.0" &&
          Translation::TrimPerformanceScale(L"0.010") == L"0.01" &&
          Translation::TrimPerformanceScale(L"100.000") == L"100.0" &&
          Translation::TrimPerformanceScale(L"0.000010") == L"0.00001" &&
          Translation::TrimPerformanceScale(L"abc") == L"abc",
        "the legend's scale text did not keep only the digits it needs");
    Check(Translation::FormatPerformanceDuration(100) == L"1:40" &&
          Translation::FormatPerformanceDuration(5) == L"0:05" &&
          Translation::FormatPerformanceDuration(3600) == L"1:00:00" &&
          Translation::FormatPerformanceDuration(90061) == L"1:01:01:01" &&
          Translation::FormatPerformanceDuration(-1) == L"0:00",
        "the value strip's duration did not read as the control writes it");
}

void TestPerformanceChartSerialization() {
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"11111111-2222-3333-4444-555555555555";

    Translation::ControlNode graphNode;
    graphNode.nodeId = 1;
    graphNode.generation = 1;
    graphNode.kind = Translation::ControlKind::PerformanceGraph;
    Translation::PerformanceGraphSnapshot graph = EmptyGraph();
    graph.barSlot = 2;
    graph.wrapped = true;
    graph.startTimeMs = 1'760'000'000'000;
    graph.endTimeMs = 1'760'000'003'000;
    graph.controlColor = 0xF0F0F0;
    graph.barColor = 0xFF0000;
    graph.series[0].name = L"\\Processor Information(_Total)\\% Processor Time";
    graph.series[0].style = L"dash";
    graph.series[0].values = { 3.5, std::nullopt, 7.125, std::nullopt, std::nullopt };
    graphNode.performanceGraph = graph;
    snapshot.nodes.push_back(graphNode);

    Translation::ControlNode legendNode;
    legendNode.nodeId = 2;
    legendNode.generation = 1;
    legendNode.kind = Translation::ControlKind::PerformanceLegend;
    Translation::PerformanceLegendSnapshot legend;
    legend.displayType = L"histogram";
    legend.columns = { { L"Show", 61 }, { L"Color", 61 }, { L"Scale", 61 } };
    Translation::PerformanceLegendRowSnapshot row;
    row.selected = true;
    row.color = 0x00FF00;
    row.cells = { L"1.0" };
    legend.rows.push_back(row);
    legendNode.performanceLegend = legend;
    snapshot.nodes.push_back(legendNode);

    Translation::ControlNode barNode;
    barNode.nodeId = 3;
    barNode.generation = 1;
    barNode.kind = Translation::ControlKind::PerformanceValueBar;
    barNode.performanceValueBar = Translation::PerformanceValueBarSnapshot{ { { L"Last", L"2.532" }, { L"Duration", L"1:40" } } };
    snapshot.nodes.push_back(barNode);

    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"kind\":\"performanceGraph\"") != std::string::npos &&
          json.find("\"kind\":\"performanceLegend\"") != std::string::npos &&
          json.find("\"kind\":\"performanceValueBar\"") != std::string::npos,
        "the chart kinds were not serialized by name");
    Check(json.find("\"performanceGraph\":{") != std::string::npos &&
          json.find("\"barSlot\":2") != std::string::npos &&
          json.find("\"wrapped\":true") != std::string::npos &&
          json.find("\"startTimeMs\":1760000000000") != std::string::npos &&
          json.find("\"controlColor\":15790320") != std::string::npos &&
          json.find("\"barColor\":16711680") != std::string::npos &&
          json.find("\"style\":\"dash\"") != std::string::npos,
        "the graph's scale, axis times, colours or line style were not serialized");
    // A slot with no sample is a gap on the wire, never a zero.
    Check(json.find("\"values\":[3.5,null,7.125,null,null]") != std::string::npos,
        "empty graph slots were not serialized as null");
    Check(json.find("\"performanceLegend\":{") != std::string::npos &&
          json.find("\"displayType\":\"histogram\"") != std::string::npos &&
          json.find("\"title\":\"Scale\"") != std::string::npos &&
          json.find("\"selected\":true") != std::string::npos &&
          json.find("\"cells\":[\"1.0\"]") != std::string::npos &&
          json.find("\"color\":65280") != std::string::npos,
        "the legend's columns, row state or cells were not serialized");
    Check(json.find("\"performanceValueBar\":{") != std::string::npos &&
          json.find("\"label\":\"Last\"") != std::string::npos &&
          json.find("\"value\":\"1:40\"") != std::string::npos,
        "the value strip's fields were not serialized");

    // Every chart kind's state is part of the fingerprint, so a change reaches the renderer as a patch.
    const auto before = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[0].performanceGraph->series[0].values[0] = 3.75;
    const auto sample = Translation::SnapshotFingerprint(snapshot);
    Check(before != sample, "a changed sample was omitted from the snapshot fingerprint");
    snapshot.nodes[0].performanceGraph->barSlot = 3;
    Check(sample != Translation::SnapshotFingerprint(snapshot), "the time bar was omitted from the snapshot fingerprint");
    snapshot.nodes[1].performanceLegend->rows[0].visible = false;
    const auto legendHidden = Translation::SnapshotFingerprint(snapshot);
    Check(sample != legendHidden, "a legend row's Show state was omitted from the snapshot fingerprint");
    snapshot.nodes[2].performanceValueBar->fields[0].value = L"2.533";
    Check(legendHidden != Translation::SnapshotFingerprint(snapshot),
        "a value strip field was omitted from the snapshot fingerprint");

    // The fixture the renderer's tests validate has the same field names the Bridge writes.
    std::ifstream shared("tests/ProtocolFixtures/window.open.performance-chart.json", std::ios::binary);
    const std::string sharedText((std::istreambuf_iterator<char>(shared)), std::istreambuf_iterator<char>());
    for (const char* field : { "\"barSlot\"", "\"wrapped\"", "\"startTimeMs\"", "\"endTimeMs\"", "\"controlColor\"", "\"barColor\"",
             "\"showTimeLabels\"", "\"scroll\"", "\"highlighted\"", "\"cells\"", "\"fields\"", "\"label\"" })
        Check(sharedText.find(field) != std::string::npos,
            "the shared performance chart fixture is missing a field the Bridge writes");
}

void TestPerformanceChartActions() {
    Translation::WindowSnapshot snapshot;
    Translation::ControlNode legendNode;
    legendNode.nodeId = 5;
    legendNode.generation = 1;
    legendNode.kind = Translation::ControlKind::PerformanceLegend;
    Translation::PerformanceLegendSnapshot legend;
    legend.columns = { { L"Show", 61 }, { L"Color", 61 } };
    legend.rows.resize(2);
    legendNode.performanceLegend = legend;
    snapshot.nodes.push_back(legendNode);
    Translation::ControlNode toolbar;
    toolbar.nodeId = 6;
    toolbar.generation = 1;
    toolbar.kind = Translation::ControlKind::Toolbar;
    Translation::ToolbarItemSnapshot split;
    split.commandId = 4321;
    split.dropDown = true;
    split.text = L"Change graph type";
    Translation::ToolbarItemSnapshot plain;
    plain.commandId = 4322;
    plain.text = L"Add";
    Translation::ToolbarItemSnapshot arrowless;
    arrowless.commandId = 4323;
    arrowless.dropDown = true;
    arrowless.enabled = false;
    toolbar.toolbarItems = { split, plain, arrowless };
    snapshot.nodes.push_back(toolbar);

    std::wstring error;
    Translation::ActionRequest select;
    select.nodeId = 5;
    select.action = L"select";
    select.integerValue = 1;
    Check(Translation::ValidateActionForSnapshot(select, snapshot, error), "the legend refused to select a counter it lists");
    select.integerValue = 2;
    Check(!Translation::ValidateActionForSnapshot(select, snapshot, error), "the legend accepted a row it does not have");
    select.integerValue = -1;
    Check(!Translation::ValidateActionForSnapshot(select, snapshot, error), "the legend accepted an empty selection");

    Translation::ActionRequest show;
    show.nodeId = 5;
    show.action = L"setItemCheck";
    show.itemIndex = 0;
    show.booleanValue = false;
    Check(Translation::ValidateActionForSnapshot(show, snapshot, error), "the legend refused to hide a counter it lists");
    show.itemIndex = 2;
    Check(!Translation::ValidateActionForSnapshot(show, snapshot, error), "the legend accepted a Show box it does not have");

    Translation::ActionRequest dropDown;
    dropDown.nodeId = 6;
    dropDown.action = L"toolbarDropDown";
    dropDown.menuCommandId = 4321;
    Check(Translation::ValidateActionForSnapshot(dropDown, snapshot, error), "an enabled drop-down button refused its arrow");
    dropDown.menuCommandId = 4322;
    Check(!Translation::ValidateActionForSnapshot(dropDown, snapshot, error), "a button with no arrow accepted an arrow press");
    dropDown.menuCommandId = 4323;
    Check(!Translation::ValidateActionForSnapshot(dropDown, snapshot, error), "a disabled drop-down accepted an arrow press");
    dropDown.menuCommandId = 9999;
    Check(!Translation::ValidateActionForSnapshot(dropDown, snapshot, error), "an unknown button accepted an arrow press");
    dropDown.nodeId = 5;
    dropDown.menuCommandId = 4321;
    Check(!Translation::ValidateActionForSnapshot(dropDown, snapshot, error), "a legend accepted a toolbar's arrow press");
    Check(Translation::IsRequestSemanticAction(L"toolbarDropDown"),
        "a drop-down request is revision-gated like a property change");

    // The wire form of the new action: an integer command id in the standard range.
    const std::string prefix = "{\"messageType\":\"action.invoke\",\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42\",\"nodeId\":\"6\",\"eventId\":\"1\",\"expectedRevision\":\"7\","
        "\"action\":\"toolbarDropDown\",\"value\":";
    Translation::ActionRequest parsed;
    Check(Translation::ParseActionInvoke(prefix + "4321}", L"00112233445566778899aabbccddeeff", parsed, error) &&
          parsed.action == L"toolbarDropDown" && parsed.menuCommandId == 4321 && parsed.nodeId == 6,
        "the native codec did not parse a toolbarDropDown request");
    Check(!Translation::ParseActionInvoke(prefix + "0}", L"00112233445566778899aabbccddeeff", parsed, error) &&
          !Translation::ParseActionInvoke(prefix + "65536}", L"00112233445566778899aabbccddeeff", parsed, error) &&
          !Translation::ParseActionInvoke(prefix + "\"4321\"}", L"00112233445566778899aabbccddeeff", parsed, error),
        "the native codec accepted a toolbarDropDown command id outside WM_COMMAND's range");
}
