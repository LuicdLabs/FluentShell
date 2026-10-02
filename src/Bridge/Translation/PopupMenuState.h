#pragma once

#include "WindowSnapshot.h"

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FluentShell::Bridge::Translation {

// A terminal answer to one intercepted native tracking call. The source thread
// revalidates a selection against the still-live HMENU before returning it to the
// application. A dismissal has no itemId, commandId or text.
struct PopupMenuDecision final {
    uint64_t popupId = 0;
    std::optional<std::wstring> itemId;
    uint32_t commandId = 0;
    std::wstring text;
    // Branch labels disambiguate a leaf reused by another dynamic MMC context.
    // This is a Bridge-only witness, not additional wire state.
    std::vector<std::wstring> ancestors;

    bool IsDismissal() const noexcept { return !itemId.has_value(); }
};

// Owned by one source thread. Tokens are never reused during this state's
// lifetime, including after cancellation. Only one native tracking call may
// await a projected answer at a time.
class PopupMenuState final {
public:
    uint64_t Begin(uint64_t nodeId, int itemIndex, std::vector<MenuItemSnapshot> items) {
        if (current_ || nextPopupId_ == 0 || nodeId == 0 || itemIndex < 0 || items.empty())
            return 0;
        PopupMenuSnapshot next;
        next.popupId = nextPopupId_;
        next.nodeId = nodeId;
        next.itemIndex = itemIndex;
        next.items = std::move(items);
        current_ = std::move(next);
        nextPopupId_ = nextPopupId_ == std::numeric_limits<uint64_t>::max()
            ? 0 : nextPopupId_ + 1;
        return current_->popupId;
    }

    const std::optional<PopupMenuSnapshot>& Current() const noexcept { return current_; }

    // Invalid/stale answers are benign and leave the pending popup untouched.
    // A valid selection or null dismissal consumes the popup exactly once.
    bool Resolve(uint64_t popupId, const std::optional<std::wstring>& itemId,
        PopupMenuDecision& decision) {
        if (!current_ || popupId == 0 || popupId != current_->popupId) return false;
        PopupMenuDecision next;
        next.popupId = popupId;
        if (itemId) {
            CommandPath path{};
            const size_t length = FindEnabledCommand(current_->items, *itemId, path);
            if (length == 0) return false;
            const auto& item = *path[length - 1];
            next.itemId = item.itemId;
            next.commandId = item.commandId;
            next.text = item.text;
            for (size_t index = 0; index + 1 < length; ++index)
                next.ancestors.push_back(path[index]->text);
        }
        // Build the answer before consuming the snapshot, so allocation failure
        // cannot retire a popup without giving its owner a terminal decision.
        decision = std::move(next);
        current_.reset();
        return true;
    }

    bool Cancel() noexcept {
        const bool pending = current_.has_value();
        current_.reset();
        return pending;
    }

    // The selected leaf must still occupy the same path, with the same command
    // ID and label, and every ancestor must retain its label and permit selection. Check/radio
    // state may legitimately change without changing the command's identity.
    static bool MatchLiveChoice(const std::vector<MenuItemSnapshot>& items,
        const PopupMenuDecision& decision) noexcept {
        if (decision.popupId == 0) return false;
        if (decision.IsDismissal()) return decision.commandId == 0 && decision.text.empty() &&
            decision.ancestors.empty();
        if (decision.commandId == 0) return false;
        CommandPath path{};
        const size_t length = FindEnabledCommand(items, *decision.itemId, path);
        if (length == 0 || decision.ancestors.size() != length - 1) return false;
        for (size_t index = 0; index + 1 < length; ++index)
            if (path[index]->text != decision.ancestors[index]) return false;
        const auto& item = *path[length - 1];
        return item.commandId == decision.commandId && item.text == decision.text;
    }

private:
    using CommandPath = std::array<const MenuItemSnapshot*, 8>;

    // Bounded, allocation-free lookup also records the enabled ancestry. A leaf
    // path alone does not prove that its enclosing native command context survived.
    static size_t FindEnabledCommand(
        const std::vector<MenuItemSnapshot>& items,
        std::wstring_view itemId,
        CommandPath& path,
        size_t depth = 0) noexcept {
        if (depth >= path.size()) return 0;
        for (const auto& item : items) {
            if (!item.enabled) continue;
            path[depth] = &item;
            if (item.kind == MenuItemKind::Command && item.commandId != 0 &&
                item.items.empty() && item.itemId == itemId) return depth + 1;
            if (item.kind == MenuItemKind::Popup) {
                if (const size_t length = FindEnabledCommand(item.items, itemId, path, depth + 1))
                    return length;
            }
        }
        return 0;
    }

    uint64_t nextPopupId_ = 1;
    std::optional<PopupMenuSnapshot> current_;
};

} // namespace FluentShell::Bridge::Translation
