import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import java.io.PrintWriter;

// Export function inventory: addr,name,size,numCallers  -> arg[0] csv path
public class DumpFuncs extends GhidraScript {
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        FunctionManager fm = currentProgram.getFunctionManager();
        PrintWriter pw = new PrintWriter(out);
        pw.println("addr,name,size,callers");
        int n = 0;
        for (Function f : fm.getFunctions(true)) {
            long size = f.getBody().getNumAddresses();
            int callers = f.getSymbol() == null ? 0
                    : getReferencesTo(f.getEntryPoint()).length;
            pw.println(f.getEntryPoint() + "," + f.getName() + "," + size + "," + callers);
            n++;
        }
        pw.close();
        println("wrote " + n + " functions to " + out);
    }
}
