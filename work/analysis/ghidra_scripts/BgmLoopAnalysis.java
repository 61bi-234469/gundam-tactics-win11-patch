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

public class BgmLoopAnalysis extends GhidraScript {
    private static final long[] TARGETS = {
        0x004215A0L, 0x00421600L, 0x004216A0L, 0x00421810L, 0x00421280L,
        0x00409CC0L, 0x0040C010L
    };
    private static final long[] DATA_REFS = { 0x0044A150L, 0x0043DDD8L };
    private static final String[] IMPORTS = { "mciSendCommandA", "mciSendStringA", "sndPlaySoundA" };

    private final Set<Long> done = new LinkedHashSet<>();

    private void decomp(Function f, DecompInterface d) throws Exception {
        long entry = f.getEntryPoint().getOffset();
        if (!done.add(entry)) return;
        println("DECOMP_BEGIN " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        if (r.getDecompiledFunction() != null) println(r.getDecompiledFunction().getC());
        else println("DECOMP_FAILED " + r.getErrorMessage());
        println("DECOMP_END " + f.getName());
    }

    private void refs(Address target, String label, DecompInterface d, boolean decompCallers) throws Exception {
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(target);
        while (it.hasNext()) {
            Reference ref = it.next();
            Address from = ref.getFromAddress();
            Function f = getFunctionContaining(from);
            Instruction ins = currentProgram.getListing().getInstructionAt(from);
            println("XREF " + label + " from=" + from + " type=" + ref.getReferenceType() +
                " func=" + (f == null ? "<none>" : f.getName() + "@" + f.getEntryPoint()) +
                " ins=" + (ins == null ? "?" : ins.toString()));
            if (decompCallers && f != null) decomp(f, d);
        }
    }

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (long v : TARGETS) {
            Function f = getFunctionAt(toAddr(v));
            if (f == null) f = getFunctionContaining(toAddr(v));
            if (f == null) { println("NOFUNC " + Long.toHexString(v)); continue; }
            decomp(f, d);
        }
        for (long v : DATA_REFS) refs(toAddr(v), "DATA_" + Long.toHexString(v), d, true);
        for (long v : new long[]{0x00421600L, 0x004216A0L, 0x004215A0L, 0x00421280L})
            refs(toAddr(v), "CALLERS_" + Long.toHexString(v), d, true);
        for (String name : IMPORTS) {
            SymbolIterator si = currentProgram.getSymbolTable().getSymbols(name);
            while (si.hasNext()) {
                Symbol s = si.next();
                refs(s.getAddress(), "IMPORT_" + name + "@" + s.getAddress(), d, true);
            }
        }
        d.dispose();
    }
}
