import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

/** Follow-up, read-only decompilation for the New Game side-selection path. */
public class NewGameFollowup extends GhidraScript {
    private static final long[] TARGETS = {
        0x00403640L, // side selection and its -1 return
        0x00407200L, // bitmap file loader
        0x004076D0L, // BWH/BIN-style loader
        0x004215A0L, // side-selection palette/resource setup
        0x00421600L, // input/window update
        0x00421280L, // side-selection cleanup
        0x00421220L, // selection sound
        0x00407890L, // rectangle/image copy used by hover redraw
        0x004090F0L, // palette fade
        0x004039D0L, // Movie\\Side%d.mov after a valid side
        0x00403C30L, // Bina\\Pilot.bin / PSTAT%d.INI after movie
        0x0040CDC0L,
        0x0040CD70L,
        0x0040CF70L,
        0x0040CFF0L,
        0x00430F60L  // path-prefix construction helper
    };

    private DecompInterface decompiler;

    private Function functionAt(long value) {
        Function f = getFunctionAt(toAddr(value));
        return f != null ? f : getFunctionContaining(toAddr(value));
    }

    private String label(Function f) {
        return f == null ? "<none>" : f.getName() + " entry=" + f.getEntryPoint();
    }

    private void decompile(long value) throws Exception {
        Function f = functionAt(value);
        println(String.format("TARGET 0x%08X function=%s", value, label(f)));
        if (f == null) return;
        DecompileResults r = decompiler.decompileFunction(f, 180, monitor);
        if (r.getDecompiledFunction() == null) {
            println("DECOMP_FAILED " + r.getErrorMessage());
            return;
        }
        println("DECOMP_BEGIN " + f.getName());
        println(r.getDecompiledFunction().getC());
        println("DECOMP_END " + f.getName());
    }

    private void callsites(long value) throws Exception {
        Function f = functionAt(value);
        if (f == null) return;
        Listing listing = currentProgram.getListing();
        println("CALLS_IN " + label(f));
        Address a = f.getBody().getMinAddress();
        Address end = f.getBody().getMaxAddress();
        while (a.compareTo(end) <= 0) {
            Instruction ins = listing.getInstructionAt(a);
            if (ins != null && ins.getFlowType().isCall()) {
                StringBuilder flows = new StringBuilder();
                for (Address flow : ins.getFlows()) {
                    Function callee = getFunctionAt(flow);
                    if (flows.length() != 0) flows.append(",");
                    flows.append(flow).append("/").append(label(callee));
                }
                println("CALLSITE " + ins.getAddress() + " bytes=" + bytes(ins) +
                    " instruction=" + ins + " destinations=" + flows);
            }
            a = ins == null ? a.add(1) : ins.getMaxAddress().add(1);
        }
    }

    private String bytes(Instruction ins) throws Exception {
        StringBuilder b = new StringBuilder();
        for (byte v : ins.getBytes()) b.append(String.format("%02X", v & 0xff));
        return b.toString();
    }

    private void xrefs(long value) throws Exception {
        Address a = toAddr(value);
        println(String.format("XREFS 0x%08X", value));
        ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
        while (it.hasNext()) {
            Reference ref = it.next();
            Function owner = getFunctionContaining(ref.getFromAddress());
            println("XREF from=" + ref.getFromAddress() + " owner=" + label(owner) +
                " type=" + ref.getReferenceType());
        }
    }

    @Override
    public void run() throws Exception {
        println("PROGRAM " + currentProgram.getExecutablePath());
        decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        for (long target : TARGETS) {
            decompile(target);
            callsites(target);
        }
        // These globals decide whether side selection exits while the selected
        // rectangle is still -1.  Emit all writers/readers for attribution.
        xrefs(0x00449F28L);
        xrefs(0x00449F30L);
        xrefs(0x0043B4A0L);
        xrefs(0x0043FC4CL); // loader/status flag used by Pilot initialization
        decompiler.dispose();
    }
}
