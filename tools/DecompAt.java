import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

// Post-script: decompile function(s) at given address(es). Usage: DecompAt.java 0x100000 ...
public class DecompAt extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        FunctionManager fm = currentProgram.getFunctionManager();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        println("=== PROGRAM: " + currentProgram.getName()
                + " lang=" + currentProgram.getLanguageID()
                + " funcs=" + fm.getFunctionCount() + " ===");
        for (String a : args) {
            Address addr = currentProgram.getAddressFactory()
                    .getDefaultAddressSpace().getAddress(Long.decode(a));
            Function fn = fm.getFunctionContaining(addr);
            if (fn == null) { println("no function at " + a); continue; }
            DecompileResults res = di.decompileFunction(fn, 60, monitor);
            println("\n===== " + fn.getName() + " @ " + fn.getEntryPoint() + " =====");
            if (res != null && res.getDecompiledFunction() != null) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("(decompile failed: " + (res != null ? res.getErrorMessage() : "null") + ")");
            }
        }
    }
}
