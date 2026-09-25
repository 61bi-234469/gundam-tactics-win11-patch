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

public class BattleClickAnalysis extends GhidraScript {
    // QTIM selector wrappers: DisposeMovie(0x07) FUN_00430451, DisposeMovieController(0x37) FUN_004306c1
    private static final long[] WRAPPERS = { 0x00430451L, 0x004306C1L };
    private static final long[] SOUND = { 0x00421250L, 0x00421220L, 0x004212B0L, 0x00421540L };
    private static final String[] IMPORTS = { "PeekMessageA", "GetMessageA", "Sleep", "SetCursorPos", "MessageBoxA" };
    private final Set<Long> done = new LinkedHashSet<>();

    private void decomp(Function f, DecompInterface d) throws Exception {
        if (!done.add(f.getEntryPoint().getOffset())) return;
        println("DECOMP_BEGIN " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        if (r.getDecompiledFunction() != null) println(r.getDecompiledFunction().getC());
        else println("DECOMP_FAILED " + r.getErrorMessage());
        println("DECOMP_END " + f.getName());
    }

    private void callers(Address target, String label, DecompInterface d, boolean dec) throws Exception {
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(target);
        while (it.hasNext()) {
            Reference ref = it.next();
            Function f = getFunctionContaining(ref.getFromAddress());
            println("XREF " + label + " from=" + ref.getFromAddress() + " func=" + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
            if (dec && f != null) decomp(f, d);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (long v : WRAPPERS) callers(toAddr(v), "WRAPPER_" + Long.toHexString(v), d, true);
        for (long v : SOUND) callers(toAddr(v), "SOUND_" + Long.toHexString(v), d, false);
        for (String name : IMPORTS) {
            SymbolIterator si = currentProgram.getSymbolTable().getSymbols(name);
            while (si.hasNext()) callers(si.next().getAddress(), "IMPORT_" + name, d, false);
        }
        d.dispose();
    }
}
