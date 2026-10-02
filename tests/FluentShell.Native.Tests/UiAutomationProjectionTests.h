#pragma once

#include "../../src/Bridge/Translation/UiAutomationProjection.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace FluentShell::Tests {

inline void TestUiAutomationProjectionScope(void (*check)(bool, const char*)) {
    using namespace Bridge::Translation;
    struct Element final {
        std::wstring_view framework;
        CONTROLTYPEID controlType;
        std::wstring_view automationId;
        std::wstring_view name;
    };
    const Element viewport{ L"XAML", UIA_PaneControlTypeId,
        kContentViewportAutomationId, L"" };
    const Element mdi{ L"XAML", UIA_WindowControlTypeId,
        L"FluentShell.Node.12.1", L"Device Manager" };
    const Element projection{ L"XAML", UIA_PaneControlTypeId, L"", L"" };
    const Element menu{ L"XAML", UIA_MenuItemControlTypeId,
        L"FluentShell.Menu.1", L"File" };

    // MMC can expose a title-matching semantic Window below its content pane.
    // Marker selection remains tied to identity even when enumeration order or
    // native titles change, and does not scope the menu bar out with that child.
    std::vector<Element> descendants{ projection, viewport, mdi, menu };
    auto selected = FindUniqueContentViewportIndex(descendants);
    check(selected && *selected == 1,
        "same-titled MMC MDI child displaced the unique content viewport");
    std::reverse(descendants.begin(), descendants.end());
    selected = FindUniqueContentViewportIndex(descendants);
    check(selected && *selected == 2,
        "content viewport selection depends on UIA enumeration order");
    descendants.push_back(viewport);
    check(!FindUniqueContentViewportIndex(descendants),
        "a duplicate viewport in another host branch was accepted");
    descendants.back().framework = L"Win32";
    check(!FindUniqueContentViewportIndex(descendants),
        "an invalid-provider viewport duplicate was filtered out before uniqueness validation");
    check(!FindUniqueContentViewportIndex(std::vector<Element>{ projection, mdi, menu }),
        "an MMC title match was accepted without a semantic content viewport");

    // These are real ancestors ordered from the viewport's parent to the host,
    // not an arbitrary list of similarly named descendants. Select the outer
    // container so siblings of the viewport (notably the menu bar) stay in scope.
    const Element inner{ L"XAML", UIA_GroupControlTypeId, L"", L"Device Manager" };
    const Element hostBridge{ L"Win32", UIA_PaneControlTypeId, L"", L"Device Manager" };
    std::vector<Element> ancestors{ inner, projection, hostBridge };
    selected = FindOutermostXamlProjectionAncestorIndex(ancestors);
    check(selected && *selected == 1,
        "XAML projection scope chose an inner titled ancestor and omitted sibling content");
    check(!FindOutermostXamlProjectionAncestorIndex(std::vector<Element>{ viewport, mdi, hostBridge }),
        "a semantic node or Win32 host was accepted as the XAML projection container");
    const Element xamlWindow{ L"XAML", UIA_WindowControlTypeId, L"", L"Device Manager" };
    check(FindOutermostXamlProjectionAncestorIndex(std::vector<Element>{ xamlWindow }) == 0,
        "a genuine XAML window ancestor was rejected");
}

} // namespace FluentShell::Tests
