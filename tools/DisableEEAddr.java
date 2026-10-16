import ghidra.app.script.GhidraScript;
import ghidra.framework.options.Options;

// Pre-script: list EE-related analysis options and disable the crashing one.
public class DisableEEAddr extends GhidraScript {
    public void run() throws Exception {
        Options opts = currentProgram.getOptions("Analysis");
        println("=== EE-related analysis options ===");
        for (String n : opts.getOptionNames()) {
            if (n.contains("R5900") || n.contains("5900") || n.contains("MIPS")
                    || n.contains("Reference") || n.contains("Constant")) {
                println("OPT: " + n);
            }
        }
        // Disable the analyzer that throws AddressOutOfBounds on 128-bit MMI constants.
        setAnalysisOption(currentProgram, "MIPS-R5900", "false");
        println("[prescript] set MIPS-R5900 = false");
    }
}
