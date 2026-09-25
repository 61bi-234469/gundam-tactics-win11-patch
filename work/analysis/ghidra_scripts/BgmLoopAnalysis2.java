import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.util.LinkedHashSet;
import java.util.Set;

public class BgmLoopAnalysis2 extends GhidraScript {
    private static final long[] TARGETS = { 0x004212B0L, 0x00421540L, 0x00421960L, 0x00421980L, 0x004215E0L };
    private static final long[] CALLEES = { 0x004215A0L, 0x004215E0L };
    private final Set<Long> done = new LinkedHashSet<>();

    private void decomp(Function f, DecompInterface d) throws Exception {
        if (!done.add(f.getEntryPoint().getOffset())) return;
        println("DECOMP_BEGIN " + f.getName() + " @ " + f.getEntryPoint());
        DecompileResults r = d.decompileFunction(f, 120, monitor);
        if (r.getDecompiledFunction() != null) println(r.getDecompiledFunction().getC());
        else println("DECOMP_FAILED " + r.getErrorMessage());
        println("DECOMP_END " + f.getName());
    }

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (long v : TARGETS) {
            Function f = getFunctionAt(toAddr(v));
            if (f != null) decomp(f, d);
        }
        for (long v : CALLEES) {
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(toAddr(v));
            while (it.hasNext()) {
                Reference ref = it.next();
                Address from = ref.getFromAddress();
                Function f = getFunctionContaining(from);
                println("CALLSITE callee=" + Long.toHexString(v) + " from=" + from + " func=" + (f == null ? "?" : f.getName()));
                Instruction ins = currentProgram.getListing().getInstructionAt(from);
                Instruction p = ins;
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < 8 && p != null; i++) { sb.insert(0, "  " + p.getAddress() + " " + p.toString() + "\n"); p = p.getPrevious(); }
                println(sb.toString());
                if (f != null) decomp(f, d);
            }
        }
        d.dispose();
    }
}
