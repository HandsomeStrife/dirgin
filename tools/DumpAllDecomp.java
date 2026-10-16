import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import java.io.PrintWriter;
import java.io.File;

// Decompile every function to analysis/decomp/<addr>_<name>.c   arg[0]=outdir
public class DumpAllDecomp extends GhidraScript {
    public void run() throws Exception {
        String outdir = getScriptArgs()[0];
        new File(outdir).mkdirs();
        FunctionManager fm = currentProgram.getFunctionManager();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int n = 0, ok = 0, fail = 0;
        for (Function f : fm.getFunctions(true)) {
            n++;
            String addr = f.getEntryPoint().toString();
            String name = f.getName().replaceAll("[^A-Za-z0-9_]", "_");
            File of = new File(outdir, addr + "_" + name + ".c");
            try {
                DecompileResults res = di.decompileFunction(f, 45, monitor);
                PrintWriter pw = new PrintWriter(of);
                if (res != null && res.getDecompiledFunction() != null) {
                    pw.println(res.getDecompiledFunction().getC());
                    ok++;
                } else {
                    pw.println("// decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
                    fail++;
                }
                pw.close();
            } catch (Exception e) {
                fail++;
            }
            if (n % 1000 == 0) println("progress " + n + " (ok=" + ok + " fail=" + fail + ")");
        }
        println("DONE total=" + n + " ok=" + ok + " fail=" + fail);
    }
}
