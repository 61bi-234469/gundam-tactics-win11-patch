import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import java.util.LinkedHashSet;
import java.util.Set;

// Click latency: decompile every function that pumps messages or reads the tick clock,
// plus readers of the WM_LBUTTONUP flag (0x0044a0c8) and button-down flag (0x00449aa4).
public class ClickLatencyAnalysis extends GhidraScript {
    private static final String[] IMPORTS = { "PeekMessageA", "GetMessageA", "GetTickCount", "timeGetTime", "Sleep", "GetMessageTime" };
    private static final long[] DATA = { 0x0044a0c8L, 0x00449aa4L, 0x00449f30L };
    private final Set<Long> done = new LinkedHashSet<>();

    private void decomp(Function f, DecompInterface d) throws Exception {
        if (!done.add(f.getEntryPoint().getOffset())) return;
        println("DECOMP_BEGIN " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults r = d.decompileFunction(f, 180, monitor);
        if (r.getDecompiledFunction() != null) println(r.getDecompiledFunction().getC());
        else println("DECOMP_FAILED " + r.getErrorMessage());
        println("DECOMP_END " + f.getName());
    }

    private void callers(Address target, String label, DecompInterface d) throws Exception {
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(target);
        while (it.hasNext()) {
            Reference ref = it.next();
            Function f = getFunctionContaining(ref.getFromAddress());
            println("XREF " + label + " from=" + ref.getFromAddress() + " type=" + ref.getReferenceType() + " func=" + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
            if (f != null) decomp(f, d);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String name : IMPORTS) {
            SymbolIterator si = currentProgram.getSymbolTable().getSymbols(name);
            while (si.hasNext()) {
                Symbol s = si.next();
                callers(s.getAddress(), "IMPORT_" + name, d);
            }
        }
        for (long v : DATA) callers(toAddr(v), "DATA_" + Long.toHexString(v), d);
        d.dispose();
    }
}
