// Create functions at code addresses that are only referenced from data
// (static-initializer tables, vtables, function-pointer tables, EH tables).
// Recompiled code can only call addresses that are function entries, so
// every such target must be one.
// Usage (headless, saving): -postScript CreateDataReferencedFunctions.java
// @category SharkTale
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class CreateDataReferencedFunctions extends GhidraScript {
	@Override
	protected void run() throws Exception {
		AddressSet code = new AddressSet();
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (b.isExecute() && b.isInitialized()) {
				code.add(b.getStart(), b.getEnd());
			}
		}
		int created = 0, scanned = 0;
		for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
			if (b.isExecute() || !b.isInitialized()) {
				continue;
			}
			for (Address a = b.getStart(); a.compareTo(b.getEnd().subtract(3)) <= 0; a = a.add(4)) {
				long v = getInt(a) & 0xffffffffL;
				Address t;
				try {
					t = toAddr(v);
				}
				catch (Exception e) {
					continue;
				}
				if (!code.contains(t) || getFunctionAt(t) != null) {
					continue;
				}
				scanned++;
				Instruction at = getInstructionAt(t);
				Instruction containing = getInstructionContaining(t);
				if (at == null && containing != null) {
					continue;  // middle of an instruction: not a code pointer
				}
				if (at == null) {
					DisassembleCommand dis = new DisassembleCommand(t, null, true);
					if (!dis.applyTo(currentProgram, monitor) || getInstructionAt(t) == null) {
						continue;
					}
				}
				// Only create a function if this address is not inside another function's body
				// (a pointer to the middle of a function is a label, e.g. a catch continuation).
				if (getFunctionContaining(t) != null) {
					continue;
				}
				if (new CreateFunctionCmd(t).applyTo(currentProgram, monitor)) {
					created++;
				}
			}
		}
		println("Created " + created + " functions from " + scanned + " data references in " + currentProgram.getName());
	}
}
