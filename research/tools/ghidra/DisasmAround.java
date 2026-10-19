// Disassembles the eboot around the given image offsets (hex, guest+0x...): from <offset - before> to
// <offset + after> bytes (defaults 0x180 / 0x80), so a loop seen in a profile and the start of the
// function that holds it can be read even when the project has no function there yet.
// Headless: analyzeHeadless <proj dir> <proj> -process eboot.bin -noanalysis -readOnly
//           -scriptPath <dir> -postScript DisasmAround.java a2208c [before after]
// @category MediEvil
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Instruction;

public class DisasmAround extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long before = args.length > 1 ? Long.parseLong(args[1], 16) : 0x180;
        long after = args.length > 2 ? Long.parseLong(args[2], 16) : 0x80;
        // The port's offsets are eboot image offsets; Ghidra loads the SELF at its own base.
        Address base = currentProgram.getImageBase();
        long offset = Long.parseLong(args[0], 16);
        Address start = base.add(offset - before);
        Address end = base.add(offset + after);
        DisassembleCommand cmd = new DisassembleCommand(new AddressSet(start, end), null, true);
        cmd.applyTo(currentProgram, monitor);
        println("== guest+0x" + Long.toHexString(offset) + " (" + base.add(offset) + "), from -0x" +
                Long.toHexString(before) + " to +0x" + Long.toHexString(after));
        StringBuilder out = new StringBuilder();
        Instruction ins = getInstructionAt(start);
        if (ins == null) {
            ins = getInstructionAfter(start);
        }
        while (ins != null && ins.getAddress().compareTo(end) <= 0) {
            long rel = ins.getAddress().subtract(base);
            out.append(String.format("%s guest+0x%06x  %s%n", ins.getAddress().equals(base.add(offset)) ? ">>" : "  ",
                                     rel, ins.toString()));
            ins = ins.getNext();
        }
        println(out.toString());
    }
}
