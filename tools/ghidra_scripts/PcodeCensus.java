// Census of raw P-code across a program: op counts, CALLOTHER (user-op) names,
// unresolved indirect branches, and the mnemonics behind user ops. Decides
// what a P-code -> C static recompiler must implement.
// Usage (headless): -postScript PcodeCensus.java <outdir>
// @category SharkTale
import java.io.File;
import java.io.PrintWriter;
import java.util.Map;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.pcode.PcodeOp;

public class PcodeCensus extends GhidraScript {
	@Override
	protected void run() throws Exception {
		Map<String, Integer> ops = new TreeMap<>();
		Map<String, Integer> userOps = new TreeMap<>();
		Map<String, Integer> userMnemonics = new TreeMap<>();
		int insns = 0, branchInd = 0, branchIndResolved = 0;
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext()) {
			Instruction insn = it.next();
			insns++;
			for (PcodeOp op : insn.getPcode()) {
				ops.merge(op.getMnemonic(), 1, Integer::sum);
				if (op.getOpcode() == PcodeOp.CALLOTHER) {
					String name = currentProgram.getLanguage().getUserDefinedOpName((int) op.getInput(0).getOffset());
					userOps.merge(name, 1, Integer::sum);
					userMnemonics.merge(name + " <- " + insn.getMnemonicString(), 1, Integer::sum);
				}
				if (op.getOpcode() == PcodeOp.BRANCHIND) {
					branchInd++;
					if (insn.getFlows().length > 0) {
						branchIndResolved++;
					}
				}
			}
		}
		String base = currentProgram.getName().replaceAll("\\.[^.]+$", "");
		File out = new File(getScriptArgs()[0], base + ".pcode_census.txt");
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.printf("instructions %d%nBRANCHIND %d (resolved %d)%n%n", insns, branchInd, branchIndResolved);
			for (Map.Entry<String, Integer> e : ops.entrySet()) {
				w.printf("op %-16s %d%n", e.getKey(), e.getValue());
			}
			for (Map.Entry<String, Integer> e : userMnemonics.entrySet()) {
				w.printf("userop %-40s %d%n", e.getKey(), e.getValue());
			}
		}
		println("Census: " + insns + " instructions, " + userOps.size() + " user ops -> " + out);
	}
}
