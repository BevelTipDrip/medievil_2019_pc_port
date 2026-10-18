// Decompiles the functions at the given eboot image offsets (hex, as the port's logs and patches use:
// guest+0x...), creating them first when needed, and lists the references to each entry.
// Headless: analyzeHeadless <proj dir> <proj> -process eboot.bin -noanalysis
//           -scriptPath <dir> -postScript DecompileAt.java 689c10 689ed0 ...
// @category MediEvil
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address base = currentProgram.getImageBase();
        println("image base " + base + ", language " + currentProgram.getLanguageID());
        // The port's offsets are eboot image offsets; Ghidra loads the SELF at its own base.
        long origin = base.getOffset();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        for (String arg : getScriptArgs()) {
            Address at = toAddr(origin + Long.parseUnsignedLong(arg.replace("0x", ""), 16));
            disassemble(at);
            Function f = getFunctionAt(at);
            if (f == null) f = createFunction(at, null);
            if (f == null) f = getFunctionContaining(at);
            if (f == null) { println("== " + arg + ": no function"); continue; }
            println("\n== guest+0x" + arg + " = " + f.getEntryPoint() + " " + f.getName());
            for (Reference r : getReferencesTo(f.getEntryPoint())) {
                Function from = getFunctionContaining(r.getFromAddress());
                println("   referenced from " + r.getFromAddress() + " (" + r.getReferenceType() + ")"
                        + (from != null ? " in " + from.getName() : ""));
            }
            DecompileResults res = decomp.decompileFunction(f, 120, monitor);
            println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "decompile failed: " + res.getErrorMessage());
        }
        decomp.dispose();
    }
}
