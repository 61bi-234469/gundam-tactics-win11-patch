import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.listing.Program;

public class DecompilePhase3Targets extends GhidraScript {
    private static final long[] TARGETS = {
        0x00407200L, 0x004090F0L, 0x0040F8E0L
    };
    private static final long[] CALL_SITES = {
        0x0040738CL, 0x00409283L, 0x0040F9C8L
    };

    private void printInstructions(Function function) throws Exception {
        Listing listing = currentProgram.getListing();
        Address start = function.getBody().getMinAddress();
        Address end = function.getBody().getMaxAddress();
        println("BODY " + start + ".." + end);
        for (Address address = start; address.compareTo(end) <= 0;) {
            Instruction instruction = listing.getInstructionAt(address);
            if (instruction == null) {
                address = address.add(1);
                continue;
            }
            StringBuilder bytes = new StringBuilder();
            byte[] raw = instruction.getBytes();
            for (byte value : raw) bytes.append(String.format("%02X", value & 0xff));
            println("INS " + instruction.getAddress() + " bytes=" + bytes +
                    " " + instruction.toString());
            Address next = instruction.getMaxAddress().add(1);
            if (next.compareTo(address) <= 0) break;
            address = next;
        }
    }

    private void printFunction(long value, DecompInterface decompiler) throws Exception {
        Address address = toAddr(value);
        Function function = getFunctionAt(address);
        if (function == null) function = getFunctionContaining(address);
        println("TARGET 0x" + Long.toHexString(value) + " function=" +
                (function == null ? "<none>" : function.getName()));
        if (function == null) return;
        DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
        if (result.getDecompiledFunction() != null) {
            println("DECOMP_BEGIN " + function.getName());
            println(result.getDecompiledFunction().getC());
            println("DECOMP_END " + function.getName());
        } else {
            println("DECOMP_FAILED " + result.getErrorMessage());
        }
        printInstructions(function);
    }

    @Override
    public void run() throws Exception {
        Program program = currentProgram;
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(program);
        println("PROGRAM " + program.getExecutablePath());
        for (long value : TARGETS) printFunction(value, decompiler);
        for (long value : CALL_SITES) {
            Address address = toAddr(value);
            Instruction instruction = program.getListing().getInstructionAt(address);
            Function function = getFunctionContaining(address);
            println("CALLSITE 0x" + Long.toHexString(value) +
                    " function=" + (function == null ? "<none>" : function.getName()) +
                    " instruction=" + (instruction == null ? "<none>" : instruction.toString()));
        }
        decompiler.dispose();
    }
}
