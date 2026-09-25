// Ghidra headless script for Phase 1 startup-requirement evidence.
// Run with analyzeHeadless -postScript StartupRequirements.java.

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class StartupRequirements extends GhidraScript {
    private static final String[] TARGETS = {
        "GetVersion", "GetVersionExA", "GetVersionExW",
        "RegOpenKeyA", "RegOpenKeyW", "RegOpenKeyExA", "RegOpenKeyExW",
        "RegCreateKeyA", "RegCreateKeyW", "RegCreateKeyExA", "RegCreateKeyExW",
        "CreateFileA", "CreateFileW", "WriteFile", "GetDeviceCaps"
    };

    private boolean matches(String name, String target) {
        if (name == null) {
            return false;
        }
        String normalized = name;
        if (normalized.startsWith("_")) {
            normalized = normalized.substring(1);
        }
        int at = normalized.indexOf('@');
        if (at >= 0) {
            normalized = normalized.substring(0, at);
        }
        return normalized.equalsIgnoreCase(target);
    }

    @Override
    public void run() throws Exception {
        println("# Phase 1 startup requirement xrefs");
        println("program=" + currentProgram.getExecutablePath());
        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        while (symbols.hasNext() && !monitor.isCancelled()) {
            Symbol symbol = symbols.next();
            for (String target : TARGETS) {
                if (!matches(symbol.getName(), target)) {
                    continue;
                }
                println("IMPORT " + target + " " + symbol.getAddress());
                ReferenceIterator references = currentProgram.getReferenceManager()
                    .getReferencesTo(symbol.getAddress());
                while (references.hasNext() && !monitor.isCancelled()) {
                    Reference reference = references.next();
                    Function caller = getFunctionContaining(reference.getFromAddress());
                    String callerName = caller == null ? "<unknown>" : caller.getName();
                    println(String.format("XREF %s from=%s caller=%s type=%s",
                        target, reference.getFromAddress(), callerName,
                        reference.getReferenceType()));
                }
            }
        }
    }
}
