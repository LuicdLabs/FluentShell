#pragma once

namespace FluentShell::Injector {

// The manual-start global watcher.  It offers the Bridge to every eligible top-level window of this
// user's interactive session until the panic hotkey, the GUI, or the process ends it.  It starts only
// when the user has accepted the current risk warning; it never persists across logon.  Status lines
// go to stdout as "FSW|kind|field|..." (ASCII; see Watcher.cpp).  dryRun reports what would be
// injected and injects nothing; it exists so the eligibility census can be checked without side
// effects.  Returns the exit code.
int RunWatch(bool dryRun);

// Writes the risk warning the user must accept, as UTF-8, to stdout.  The GUI hashes these exact bytes.
int PrintRiskWarning();

} // namespace FluentShell::Injector
