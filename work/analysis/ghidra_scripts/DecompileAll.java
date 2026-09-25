import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

// Game-logic survey: dump every function's decompilation to one file.
public class DecompileAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        if (getScriptArgs().length == 0) {
            printerr("usage: DecompileAll.java <output.c>");
            return;
        }
        String out = getScriptArgs()[0];
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                w.println("//==== " + f.getName() + " @ " + f.getEntryPoint());
                DecompileResults r = d.decompileFunction(f, 120, monitor);
                if (r.getDecompiledFunction() != null) w.println(r.getDecompiledFunction().getC());
                else w.println("// DECOMP_FAILED " + r.getErrorMessage());
            }
        }
    }
}
