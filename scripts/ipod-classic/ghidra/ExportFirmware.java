// Export selected firmware functions for peripheral reverse engineering.
// @category iPod
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import java.io.PrintWriter;

public class ExportFirmware extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            throw new IllegalArgumentException("Output path and addresses required");
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0])) {
            out.println("/* Ghidra pseudocode: " + currentProgram.getName() + " */");
            for (int i = 1; i < args.length; i++) {
                Address address = toAddr(Long.parseUnsignedLong(args[i], 16));
                Function function = getFunctionContaining(address);
                if (function == null) {
                    disassemble(address);
                    function = createFunction(address, null);
                }
                if (function == null) {
                    out.println("/* No function at " + address + " */");
                    continue;
                }
                out.println("\n/* Requested " + address + ", containing function "
                            + function.getEntryPoint() + " */");
                DecompileResults result =
                    decompiler.decompileFunction(function, 45, monitor);
                if (result.decompileCompleted()) {
                    out.println(result.getDecompiledFunction().getC());
                } else {
                    out.println("/* " + result.getErrorMessage() + " */");
                }
            }
        } finally {
            decompiler.dispose();
        }
    }
}
