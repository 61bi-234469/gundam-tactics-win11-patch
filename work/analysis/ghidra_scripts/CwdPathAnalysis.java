import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import java.util.LinkedHashSet;
import java.util.Set;

// Plan D survey: where gundam.exe obtains the directory it prefixes to asset
// paths, and every consumer of that 80-byte buffer.
public class CwdPathAnalysis extends GhidraScript {
    private static final long[] TARGETS = { 0x00430F60L, 0x00430F80L, 0x00408060L };
    private static final long[] ORDER = { 0x0040D460L };
    private static final long[] CALLEES = { 0x00430F60L, 0x00430F80L };
    private static final String[] APIS = { "GetCurrentDirectoryA", "GetModuleFileNameA", "GetFullPathNameA",
        "SetCurrentDirectoryA", "LoadLibraryA", "GetPrivateProfileStringA", "CreateDirectoryA", "GetFileAttributesA" };
    private final Set<Long> done = new LinkedHashSet<>();

    private void decomp(Function f, DecompInterface d) throws Exception {
        if (!done.add(f.getEntryPoint().getOffset())) return;
        println("DECOMP_BEGIN " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        if (r.getDecompiledFunction() != null) println(r.getDecompiledFunction().getC());
        else println("DECOMP_FAILED " + r.getErrorMessage());
        println("DECOMP_END " + f.getName());
    }

    private void callsites(Address target, String label, DecompInterface d) throws Exception {
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(target);
        while (it.hasNext()) {
            Reference ref = it.next();
            Address from = ref.getFromAddress();
            Function f = getFunctionContaining(from);
            println("CALLSITE callee=" + label + " from=" + from + " func=" + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
            Instruction p = currentProgram.getListing().getInstructionAt(from);
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < 6 && p != null; i++) { sb.insert(0, "  " + p.getAddress() + " " + p.toString() + "\n"); p = p.getPrevious(); }
            println(sb.toString());
            if (f != null) decomp(f, d);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (long v : TARGETS) {
            Function f = getFunctionAt(toAddr(v));
            if (f != null) decomp(f, d);
        }
        for (long v : ORDER) callsites(toAddr(v), "ORDER_" + Long.toHexString(v), d);
        for (long v : CALLEES) callsites(toAddr(v), "FUN_" + Long.toHexString(v), d);
        for (String api : APIS) {
            SymbolIterator si = currentProgram.getSymbolTable().getSymbols(api);
            while (si.hasNext()) {
                Symbol s = si.next();
                println("API " + api + " sym@" + s.getAddress());
                for (Reference r : getReferencesTo(s.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    println("  APIREF " + api + " from=" + r.getFromAddress() + " func=" + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
                    // IAT slot: follow references to the pointer too
                    for (Reference r2 : getReferencesTo(r.getFromAddress())) {
                        Function f2 = getFunctionContaining(r2.getFromAddress());
                        println("    VIA " + r2.getFromAddress() + " func=" + (f2 == null ? "?" : f2.getName() + "@" + f2.getEntryPoint()));
                        if (f2 != null && api.equals("LoadLibraryA")) { decomp(f2, d); callsites(f2.getEntryPoint(), "LOADER_" + f2.getName(), d); }
                    }
                }
            }
        }
        d.dispose();
    }
}
