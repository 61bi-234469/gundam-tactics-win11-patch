import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

/** Small-output selector used to inspect one follow-up function per headless run. */
public class NewGameSelected extends GhidraScript {
    private Function find(long value) {
        Function f = getFunctionAt(toAddr(value));
        return f != null ? f : getFunctionContaining(toAddr(value));
    }

    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        String[] args = getScriptArgs();
        if (args.length == 0) args = new String[] {"0x004076D0"};
        for (String arg : args) {
            long value = Long.decode(arg);
            Function f = find(value);
            println(String.format("TARGET 0x%08X function=%s", value,
                f == null ? "<none>" : f.getName() + " entry=" + f.getEntryPoint()));
            if (f == null) continue;
            DecompileResults r = d.decompileFunction(f, 180, monitor);
            if (r.getDecompiledFunction() == null) {
                println("DECOMP_FAILED " + r.getErrorMessage());
            } else {
                println("DECOMP_BEGIN " + f.getName());
                println(r.getDecompiledFunction().getC());
                println("DECOMP_END " + f.getName());
            }
        }
        d.dispose();
    }
}
