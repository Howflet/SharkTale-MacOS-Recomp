// Incremental-link stubs (jmp <body>) carry the export names; the real bodies are
// FUN_xxxxxxxx. Copy each stub's name (and namespace) onto its target so decompiled
// code and call sites read with real names.
// Usage (headless, without -readOnly so the result is saved):
//   -postScript PropagateThunkNames.java
// @category SharkTale
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.SourceType;

public class PropagateThunkNames extends GhidraScript {
	@Override
	protected void run() throws Exception {
		int renamed = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (f.getName().startsWith("FUN_") || f.getName().startsWith("thunk_")) {
				continue;
			}
			Function body = f.isThunk() ? f.getThunkedFunction(false) : jumpTarget(f);
			if (body == null || body.isExternal() || !body.getName().startsWith("FUN_")) {
				continue;
			}
			body.setParentNamespace(f.getParentNamespace());
			body.setName(f.getName(), SourceType.ANALYSIS);
			if (f.getSignatureSource().isHigherPriorityThan(body.getSignatureSource())) {
				body.setCallingConvention(f.getCallingConventionName());
			}
			renamed++;
		}
		println("Renamed " + renamed + " thunk targets in " + currentProgram.getName());
	}

	/** A named 5-byte stub that is a single JMP: return the function it jumps to. */
	private Function jumpTarget(Function f) {
		if (f.getBody().getNumAddresses() > 5) {
			return null;
		}
		Instruction insn = getInstructionAt(f.getEntryPoint());
		if (insn == null || !insn.getMnemonicString().equalsIgnoreCase("JMP")) {
			return null;
		}
		Address[] flows = insn.getFlows();
		return flows.length == 1 ? getFunctionAt(flows[0]) : null;
	}
}
