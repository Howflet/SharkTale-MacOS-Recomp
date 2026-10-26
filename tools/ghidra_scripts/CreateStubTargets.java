// Incremental-link thunk tables: runs of 5-byte "E9 rel32" jumps. Every entry
// is a function (it's what vtables, exports and calls point at) and every
// target is the real body. Create both so the recompiler emits them.
// Usage (headless, saving): -postScript CreateStubTargets.java
// @category SharkTale
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class CreateStubTargets extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Memory mem = currentProgram.getMemory();
		int stubs = 0, bodies = 0;
		for (MemoryBlock b : mem.getBlocks()) {
			if (!b.isExecute() || !b.isInitialized()) {
				continue;
			}
			Address a = b.getStart();
			Address end = b.getEnd().subtract(5);
			while (a.compareTo(end) <= 0) {
				// A run of at least 3 consecutive E9 jumps is a thunk table.
				int run = 0;
				while (run < 3 && a.add(5L * run).compareTo(end) <= 0 && mem.getByte(a.add(5L * run)) == (byte) 0xe9) {
					run++;
				}
				if (run < 3) {
					a = a.add(1);
					continue;
				}
				while (a.compareTo(end) <= 0 && mem.getByte(a) == (byte) 0xe9) {
					Address target;
					try {
						target = a.add(5 + mem.getInt(a.add(1)));
					}
					catch (Exception e) {
						a = a.add(5);
						continue;  // stray E9 byte: target outside the address space
					}
					if (mem.contains(target) && mem.getBlock(target).isExecute()) {
						if (getFunctionAt(a) == null && getInstructionContaining(a) == null) {
							new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
						}
						if (getFunctionAt(a) == null && new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) {
							stubs++;
						}
						if (getFunctionAt(target) == null) {
							if (getInstructionAt(target) == null && getInstructionContaining(target) == null) {
								new DisassembleCommand(target, null, true).applyTo(currentProgram, monitor);
							}
							if (getInstructionAt(target) != null
									&& new CreateFunctionCmd(target).applyTo(currentProgram, monitor)) {
								bodies++;
							}
						}
					}
					a = a.add(5);
				}
			}
		}
		println("Stub tables: created " + stubs + " stub functions and " + bodies + " body functions in "
			+ currentProgram.getName());
	}
}
