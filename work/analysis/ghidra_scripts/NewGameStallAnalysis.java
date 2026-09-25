import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.listing.Program;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.ReferenceManager;

import java.util.HashSet;
import java.util.Set;

/**
 * New Game stall investigation.
 *
 * This script deliberately emits text only.  It imports the pristine executable
 * in a throw-away headless project and does not modify source_exe_01/.
 */
public class NewGameStallAnalysis extends GhidraScript {
    private static final long TARGET = 0x00403260L;
    private static final long[] KNOWN_CALL_SITES = {
        0x00403336L, 0x00403498L, 0x00403516L, 0x00403624L
    };
    private static final long[] KNOWN_RELATED = {
        0x0040C010L
    };
    private final Set<String> emitted = new HashSet<>();
    private DecompInterface decompiler;

    private String functionName(Function function) {
        return function == null ? "<none>" : function.getName() +
            " entry=" + function.getEntryPoint();
    }

    private void decompile(Function function, int timeoutSeconds) throws Exception {
        if (function == null) return;
        String key = function.getEntryPoint().toString();
        if (!emitted.add(key)) return;
        println("FUNCTION " + functionName(function));
        DecompileResults result = decompiler.decompileFunction(function, timeoutSeconds, monitor);
        if (result.getDecompiledFunction() != null) {
            println("DECOMP_BEGIN " + function.getName());
            println(result.getDecompiledFunction().getC());
            println("DECOMP_END " + function.getName());
        } else {
            println("DECOMP_FAILED " + result.getErrorMessage());
        }
    }

    private void printBodyInstructions(Function function) throws Exception {
        if (function == null) return;
        Listing listing = currentProgram.getListing();
        Address address = function.getBody().getMinAddress();
        Address end = function.getBody().getMaxAddress();
        println("BODY " + address + ".." + end);
        while (address.compareTo(end) <= 0) {
            Instruction instruction = listing.getInstructionAt(address);
            if (instruction == null) {
                address = address.add(1);
                continue;
            }
            StringBuilder bytes = new StringBuilder();
            for (byte value : instruction.getBytes()) {
                bytes.append(String.format("%02X", value & 0xff));
            }
            println("INS " + instruction.getAddress() + " bytes=" + bytes +
                " " + instruction.toString());
            Address next = instruction.getMaxAddress().add(1);
            if (next.compareTo(address) <= 0) break;
            address = next;
        }
    }

    private void printCallers(Function target) throws Exception {
        ReferenceManager refs = currentProgram.getReferenceManager();
        ReferenceIterator iterator = refs.getReferencesTo(target.getEntryPoint());
        while (iterator.hasNext()) {
            Reference reference = iterator.next();
            Function caller = getFunctionContaining(reference.getFromAddress());
            println("CALLER ref=" + reference.getFromAddress() +
                " caller=" + functionName(caller) +
                " flow=" + reference.getReferenceType());
            decompile(caller, 120);
        }
    }

    private void printCalledFunctions(Function target) throws Exception {
        println("CALLEES_OF " + functionName(target));
        Set<Function> calledFunctions = target.getCalledFunctions(monitor);
        for (Function callee : calledFunctions) {
            println("CALLEE " + functionName(callee));
            if (!callee.getEntryPoint().isExternalAddress()) {
                decompile(callee, 120);
            }
        }
    }

    private void printCallSites(Function target) throws Exception {
        Listing listing = currentProgram.getListing();
        Address address = target.getBody().getMinAddress();
        Address end = target.getBody().getMaxAddress();
        println("CALLS_IN " + functionName(target));
        while (address.compareTo(end) <= 0) {
            Instruction instruction = listing.getInstructionAt(address);
            if (instruction != null && instruction.getFlowType().isCall()) {
                Address[] flows = instruction.getFlows();
                StringBuilder destinations = new StringBuilder();
                for (Address flow : flows) {
                    Function callee = getFunctionAt(flow);
                    if (destinations.length() != 0) destinations.append(",");
                    destinations.append(flow).append("/").append(functionName(callee));
                }
                println("CALLSITE " + instruction.getAddress() +
                    " instruction=" + instruction + " destinations=" + destinations);
            }
            if (instruction == null) address = address.add(1);
            else address = instruction.getMaxAddress().add(1);
        }
    }

    private void printKnownAddress(long value) throws Exception {
        Address address = toAddr(value);
        Function function = getFunctionAt(address);
        if (function == null) function = getFunctionContaining(address);
        println("KNOWN 0x" + Long.toHexString(value) + " function=" + functionName(function));
        if (function != null) decompile(function, 120);
    }

    @Override
    public void run() throws Exception {
        Program program = currentProgram;
        decompiler = new DecompInterface();
        decompiler.openProgram(program);
        println("PROGRAM " + program.getExecutablePath());

        Function target = getFunctionAt(toAddr(TARGET));
        if (target == null) target = getFunctionContaining(toAddr(TARGET));
        println("TARGET " + functionName(target));
        if (target == null) {
            println("ERROR target function not found");
            decompiler.dispose();
            return;
        }

        decompile(target, 180);
        printBodyInstructions(target);
        printCallSites(target);
        printCallers(target);
        printCalledFunctions(target);

        for (long value : KNOWN_CALL_SITES) printKnownAddress(value);
        for (long value : KNOWN_RELATED) printKnownAddress(value);

        // Emit all imported/API-like functions that appear in the program so the
        // report records whether time, multimedia, message, or input imports are
        // actually present and named by the analyzer.
        FunctionIterator functions = program.getFunctionManager().getFunctions(true);
        while (functions.hasNext()) {
            Function function = functions.next();
            String name = function.getName().toLowerCase();
            if (name.contains("tick") || name.contains("time") ||
                name.contains("wave") || name.contains("mci") ||
                name.contains("peek") || name.contains("message") ||
                name.contains("key") || name.contains("mouse") ||
                name.contains("palette") || name.contains("bitblt")) {
                println("API_CANDIDATE " + functionName(function));
            }
        }
        decompiler.dispose();
    }
}
